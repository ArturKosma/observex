#pragma once

#include "core/TripleBuffer.h"
#include "engine/Gpu.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <stop_token>
#include <string>
#include <thread>

namespace observex {

class PreviewChannel;
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

// The real-time loop: capture, then (still to come) recognise and react. It runs on its own time-critical
// thread with its own GPU device, so nothing the UI does can slow it down. Talks to the UI only through
// lock-free hand-overs (stats, preview) and atomics.
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

    // Stops the loop and frees its GPU resources. Destroy any PreviewReader of preview() first.
    void stop();

    State state() const { return state_.load(std::memory_order_acquire); }
    const std::string& error() const { return error_; } // Valid once state() is Failed.

    // While on, each new image is also copied for the UI preview. Off costs nothing.
    void setPreviewEnabled(bool enabled) { previewEnabled_.store(enabled, std::memory_order_relaxed); }
    PreviewChannel* preview() const { return preview_.get(); }

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
    std::unique_ptr<PreviewChannel> preview_;
    std::unique_ptr<TripleBuffer<PipelineStats>> stats_;
    std::atomic<State> state_{State::Stopped};
    std::atomic<bool> previewEnabled_{false};
    std::string error_;
    unsigned width_ = 0;
    unsigned height_ = 0;
    double refreshHz_ = 0.0;
    std::jthread thread_;
};

} // namespace observex
