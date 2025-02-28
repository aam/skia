/*
 * Copyright 2018 Google Inc.
 *
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include "include/core/SkCanvas.h"
#include "include/core/SkGraphics.h"
#include "include/core/SkPictureRecorder.h"
#include "include/core/SkStream.h"
#include "include/core/SkSurface.h"
#include "include/encode/SkPngEncoder.h"
#include "include/private/SkSpinlock.h"
#include "modules/skottie/include/Skottie.h"
#include "modules/skottie/utils/SkottieUtils.h"
#include "src/core/SkMakeUnique.h"
#include "src/core/SkOSFile.h"
#include "src/core/SkTaskGroup.h"
#include "src/utils/SkOSPath.h"
#include "tools/flags/CommandLineFlags.h"

#include <algorithm>
#include <chrono>
#include <numeric>
#include <vector>

static DEFINE_string2(input    , i, nullptr, "Input .json file.");
static DEFINE_string2(writePath, w, nullptr, "Output directory.  Frames are names [0-9]{6}.png.");
static DEFINE_string2(format   , f, "png"  , "Output format (png, skp or null)");

static DEFINE_double(t0,   0, "Timeline start [0..1].");
static DEFINE_double(t1,   1, "Timeline stop [0..1].");
static DEFINE_double(fps, 30, "Decode frames per second.");

static DEFINE_int(width , 800, "Render width.");
static DEFINE_int(height, 600, "Render height.");
static DEFINE_int(threads,  0, "Number of worker threads (0 -> cores count).");

namespace {

std::unique_ptr<SkFILEWStream> MakeFrameStream(size_t idx, const char* ext) {
    const auto frame_file = SkStringPrintf("0%06d.%s", idx, ext);
    auto stream = skstd::make_unique<SkFILEWStream>(SkOSPath::Join(FLAGS_writePath[0],
                                                                   frame_file.c_str()).c_str());
    if (!stream->isValid()) {
        return nullptr;
    }

    return stream;
}

class Sink {
public:
    Sink() = default;
    virtual ~Sink() = default;
    Sink(const Sink&) = delete;
    Sink& operator=(const Sink&) = delete;

    virtual SkCanvas* beginFrame(size_t idx) = 0;
    virtual bool endFrame(size_t idx) = 0;
};

class PNGSink final : public Sink {
public:
    static std::unique_ptr<Sink> Make(const SkMatrix& scale_matrix) {
        auto surface = SkSurface::MakeRasterN32Premul(FLAGS_width, FLAGS_height);
        if (!surface) {
            SkDebugf("Could not allocate a %d x %d surface.\n", FLAGS_width, FLAGS_height);
            return nullptr;
        }

        return std::unique_ptr<Sink>(new PNGSink(std::move(surface), scale_matrix));
    }

private:
    PNGSink(sk_sp<SkSurface> surface, const SkMatrix& scale_matrix)
        : fSurface(std::move(surface)) {
        fSurface->getCanvas()->concat(scale_matrix);
    }

    SkCanvas* beginFrame(size_t) override {
        auto* canvas = fSurface->getCanvas();
        canvas->clear(SK_ColorTRANSPARENT);
        return canvas;
    }

    bool endFrame(size_t idx) override {
        auto stream = MakeFrameStream(idx, "png");
        if (!stream) {
            return false;
        }

        // Set encoding options to favor speed over size.
        SkPngEncoder::Options options;
        options.fZLibLevel   = 1;
        options.fFilterFlags = SkPngEncoder::FilterFlag::kNone;

        sk_sp<SkImage> img = fSurface->makeImageSnapshot();
        SkPixmap pixmap;
        return img->peekPixels(&pixmap)
            && SkPngEncoder::Encode(stream.get(), pixmap, options);
    }

    const sk_sp<SkSurface> fSurface;
};

class SKPSink final : public Sink {
public:
    static std::unique_ptr<Sink> Make(const SkMatrix& scale_matrix) {
        return std::unique_ptr<Sink>(new SKPSink(scale_matrix));
    }

private:
    explicit SKPSink(const SkMatrix& scale_matrix)
        : fScaleMatrix(scale_matrix) {}

    SkCanvas* beginFrame(size_t) override {
        auto canvas = fRecorder.beginRecording(FLAGS_width, FLAGS_height);
        canvas->concat(fScaleMatrix);
        return canvas;
    }

    bool endFrame(size_t idx) override {
        auto stream = MakeFrameStream(idx, "skp");
        if (!stream) {
            return false;
        }

        fRecorder.finishRecordingAsPicture()->serialize(stream.get());
        return true;
    }

    const SkMatrix    fScaleMatrix;
    SkPictureRecorder fRecorder;
};

class NullSink final : public Sink {
public:
    static std::unique_ptr<Sink> Make(const SkMatrix& scale_matrix) {
        auto surface = SkSurface::MakeRasterN32Premul(FLAGS_width, FLAGS_height);
        if (!surface) {
            SkDebugf("Could not allocate a %d x %d surface.\n", FLAGS_width, FLAGS_height);
            return nullptr;
        }

        return std::unique_ptr<Sink>(new NullSink(std::move(surface), scale_matrix));
    }

private:
    NullSink(sk_sp<SkSurface> surface, const SkMatrix& scale_matrix)
        : fSurface(std::move(surface)) {
        fSurface->getCanvas()->concat(scale_matrix);
    }

    SkCanvas* beginFrame(size_t) override {
        auto* canvas = fSurface->getCanvas();
        canvas->clear(SK_ColorTRANSPARENT);
        return canvas;
    }

    bool endFrame(size_t) override {
        return true;
    }

    const sk_sp<SkSurface> fSurface;
};

class Logger final : public skottie::Logger {
public:
    struct LogEntry {
        SkString fMessage,
                 fJSON;
    };

    void log(skottie::Logger::Level lvl, const char message[], const char json[]) override {
        auto& log = lvl == skottie::Logger::Level::kError ? fErrors : fWarnings;
        log.push_back({ SkString(message), json ? SkString(json) : SkString() });
    }

    void report() const {
        SkDebugf("Animation loaded with %lu error%s, %lu warning%s.\n",
                 fErrors.size(), fErrors.size() == 1 ? "" : "s",
                 fWarnings.size(), fWarnings.size() == 1 ? "" : "s");

        const auto& show = [](const LogEntry& log, const char prefix[]) {
            SkDebugf("%s%s", prefix, log.fMessage.c_str());
            if (!log.fJSON.isEmpty())
                SkDebugf(" : %s", log.fJSON.c_str());
            SkDebugf("\n");
        };

        for (const auto& err : fErrors)   show(err, "  !! ");
        for (const auto& wrn : fWarnings) show(wrn, "  ?? ");
    }

private:
    std::vector<LogEntry> fErrors,
                          fWarnings;
};

std::unique_ptr<Sink> MakeSink(const char* fmt, const SkMatrix& scale_matrix) {
    if (0 == strcmp(fmt,  "png")) return  PNGSink::Make(scale_matrix);
    if (0 == strcmp(fmt,  "skp")) return  SKPSink::Make(scale_matrix);
    if (0 == strcmp(fmt, "null")) return NullSink::Make(scale_matrix);

    SkDebugf("Unknown format: %s\n", FLAGS_format[0]);
    return nullptr;
}

} // namespace

extern bool gSkUseThreadLocalStrikeCaches_IAcknowledgeThisIsIncrediblyExperimental;

int main(int argc, char** argv) {
    gSkUseThreadLocalStrikeCaches_IAcknowledgeThisIsIncrediblyExperimental = true;
    CommandLineFlags::Parse(argc, argv);
    SkAutoGraphics ag;

    if (FLAGS_input.isEmpty() || FLAGS_writePath.isEmpty()) {
        SkDebugf("Missing required 'input' and 'writePath' args.\n");
        return 1;
    }

    if (FLAGS_fps <= 0) {
        SkDebugf("Invalid fps: %f.\n", FLAGS_fps);
        return 1;
    }

    if (!sk_mkdir(FLAGS_writePath[0])) {
        return 1;
    }

    auto logger = sk_make_sp<Logger>();
    auto     rp = skottie_utils::CachingResourceProvider::Make(
                      skottie_utils::FileResourceProvider::Make(SkOSPath::Dirname(FLAGS_input[0]),
                                                                /*predecode=*/true));
    auto data   = SkData::MakeFromFileName(FLAGS_input[0]);

    if (!data) {
        SkDebugf("Could not load %s.\n", FLAGS_input[0]);
        return 1;
    }

    // Instantiate an animation on the main thread for two reasons:
    //   - we need to know its duration upfront
    //   - we want to only report parsing errors once
    auto anim = skottie::Animation::Builder()
            .setLogger(logger)
            .setResourceProvider(rp)
            .make(static_cast<const char*>(data->data()), data->size());
    if (!anim) {
        SkDebugf("Could not parse animation: '%s'.\n", FLAGS_input[0]);
        return 1;
    }

    const auto scale_matrix = SkMatrix::MakeRectToRect(SkRect::MakeSize(anim->size()),
                                                       SkRect::MakeIWH(FLAGS_width, FLAGS_height),
                                                       SkMatrix::kCenter_ScaleToFit);
    logger->report();

    static constexpr double kMaxFrames = 10000;
    const auto t0 = SkTPin(FLAGS_t0, 0.0, 1.0),
               t1 = SkTPin(FLAGS_t1,  t0, 1.0),
               dt = 1 / std::min(anim->duration() * FLAGS_fps, kMaxFrames);

    const auto frame_count = static_cast<int>((t1 - t0) / dt);

    SkSpinlock lock;
    std::vector<double> frames_ms;
    frames_ms.reserve(frame_count);

    SkTaskGroup::Enabler enabler(FLAGS_threads - 1);
    SkTaskGroup{}.batch(frame_count, [&](int i) {
        const auto start = std::chrono::steady_clock::now();
#if defined(SK_BUILD_FOR_IOS)
        // iOS doesn't support thread_local on versions less than 9.0.
        auto anim = skottie::Animation::Builder()
                            .setResourceProvider(rp)
                            .make(static_cast<const char*>(data->data()), data->size());
        auto sink = MakeSink(FLAGS_format[0], scale_matrix);
#else
        thread_local static auto* anim =
                skottie::Animation::Builder()
                    .setResourceProvider(rp)
                    .make(static_cast<const char*>(data->data()), data->size())
                    .release();
        thread_local static auto* sink = MakeSink(FLAGS_format[0], scale_matrix).release();
#endif

        if (sink && anim) {
            anim->seek(t0 + dt * i);
            anim->render(sink->beginFrame(i));
            sink->endFrame(i);
        }

        const auto elapsed = std::chrono::steady_clock::now() - start;
        double ms = std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count();
        lock.acquire();
            frames_ms.push_back(ms);
        lock.release();
    });

    std::sort(frames_ms.begin(), frames_ms.end());
    double sum = std::accumulate(frames_ms.begin(), frames_ms.end(), 0);
    SkDebugf("frame time min %gms, med %gms, avg %gms, max %gms, sum %gms\n",
             frames_ms[0], frames_ms[frame_count/2], sum/frame_count, frames_ms.back(), sum);
    return 0;
}
