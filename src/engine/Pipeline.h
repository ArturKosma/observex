#pragma once

#include "core/Question.h"
#include "core/TripleBuffer.h"
#include "engine/Gpu.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <stop_token>
#include <string>
#include <thread>

namespace observex {

class ImageChannel;
class ScreenCapture;

// What the pipeline reports to the UI, refreshed once per second.
struct PipelineStats {
    std::uint64_t images = 0;        // New screen images captured since start.
    std::uint64_t mergedUpdates = 0; // Screen updates that came between two captures and were merged into one image.
    float imagesPerSecond = 0.0f;
    float latencyAvgMs = 0.0f; // From the screen update to the pipeline holding the image.
    float latencyMaxMs = 0.0f;
    float workAvgMs = 0.0f; // CPU time the pipeline spends on each image.
    float workMaxMs = 0.0f;
};

// The real-time loop: capture, then hand each image on to recognise (see Recognizer) and, still to come, react. It
// runs on its own time-critical thread with its own GPU device, so nothing the UI does can slow it down. Talks to the
// UI only through lock-free hand-overs (stats, preview, recording, recognition) and atomics.
class Pipeline {
public:
    enum class State : std::uint8_t {
        Stopped,
        Running,
        Reconnecting, // Capture was interrupted; trying to get it back.
        NeedsRestart, // The display mode changed; the owner should call start() again.
        Failed,       // See error().
    };

    Pipeline() = default;
    ~Pipeline();

    Pipeline(const Pipeline&) = delete;
    Pipeline& operator=(const Pipeline&) = delete;

    // Everything below is for the owner's (UI) thread.

    // Sets up capture of `screen` on a new GPU device and starts the loop. Returns false on failure;
    // state() is then Failed and error() says why.
    bool start(const Screen& screen);

    // Stops the loop and frees its GPU resources. Destroy any reader of preview() or recording() first.
    void stop();

    State state() const { return state_.load(std::memory_order_acquire); }
    const std::string& error() const { return error_; } // Valid once state() is Failed.

    // While on, each new image is also copied for the UI preview. Off costs nothing.
    void setPreviewEnabled(bool enabled) { previewEnabled_.store(enabled, std::memory_order_relaxed); }
    ImageChannel* preview() const { return preview_.get(); }

    // While above zero, one image every that many seconds is also copied for recording (see Recorder).
    // Only images the screen actually updated are recorded. Zero (off) costs nothing.
    void setRecordingInterval(unsigned seconds) { recordingInterval_.store(seconds, std::memory_order_relaxed); }
    // While on, when an image is due for recording the pipeline asks question() whether it is worth it (the
    // recogniser answers, see Recognizer::kMirrorDisagrees): on yes it records the next new image, on no it waits
    // for the next due time. Off: recorded when due. Either way the interval counts on from the recorded image (or
    // the no), so images are at least that far apart.
    void setRecordOnlyQuestions(bool on) { onlyQuestions_.store(on, std::memory_order_relaxed); }
    // For the recogniser to answer; it must not outlive the pipeline.
    Question& question() { return question_; }
    // While recording: also records the next new image, then the interval counts on from it. Costs nothing.
    void recordNow() { recordNow_.store(true, std::memory_order_relaxed); }
    ImageChannel* recording() const { return recording_.get(); }

    // While on, every new image is also copied for recognition (see Recognizer). Off costs nothing.
    void setRecognitionEnabled(bool enabled) { recognitionEnabled_.store(enabled, std::memory_order_relaxed); }
    ImageChannel* recognition() const { return recognition_.get(); }

    // Newest statistics. The reference stays valid until the next call or start().
    const PipelineStats& stats();

    unsigned width() const { return width_; }
    unsigned height() const { return height_; }
    double refreshHz() const { return refreshHz_; }

private:
    void run(std::stop_token stop);
    bool reconnect(std::stop_token stop);
    void fail(std::string message);

    GpuDevice gpu_;
    std::unique_ptr<ScreenCapture> capture_;
    std::unique_ptr<ImageChannel> preview_;
    std::unique_ptr<ImageChannel> recording_;
    std::unique_ptr<ImageChannel> recognition_;
    std::unique_ptr<TripleBuffer<PipelineStats>> stats_;
    std::atomic<State> state_{State::Stopped};
    std::atomic<bool> previewEnabled_{false};
    std::atomic<unsigned> recordingInterval_{0};
    std::atomic<bool> onlyQuestions_{false};
    Question question_;
    std::atomic<bool> recordNow_{false};
    std::atomic<bool> recognitionEnabled_{false};
    std::string error_;
    unsigned width_ = 0;
    unsigned height_ = 0;
    double refreshHz_ = 0.0;
    std::jthread thread_;
};

} // namespace observex
