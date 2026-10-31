#include "engine/Pipeline.h"

#include "core/StatWindow.h"
#include "engine/ImageChannel.h"
#include "engine/ScreenCapture.h"

#include <chrono>
#include <utility>

namespace observex {

namespace {

// Longest wait for a new image. Also how quickly the loop notices a stop request.
constexpr unsigned kAcquireTimeoutMs = 50;

std::int64_t qpcNow() {
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return now.QuadPart;
}

} // namespace

Pipeline::~Pipeline() {
    stop();
}

bool Pipeline::start(const Screen& screen) {
    stop();
    try {
        const Screen own = findOnOwnAdapter(screen); // So the UI's device cannot hold up capture.
        gpu_ = createGpuDevice(own.adapter.Get());
        capture_ = std::make_unique<ScreenCapture>(gpu_.device.Get(), own.output.Get());
        preview_ = std::make_unique<ImageChannel>(gpu_.device.Get(), capture_->width(), capture_->height(),
                                                  ScreenCapture::kImageFormat);
        recording_ = std::make_unique<ImageChannel>(gpu_.device.Get(), capture_->width(), capture_->height(),
                                                    ScreenCapture::kImageFormat);
    } catch (const std::exception& e) {
        stop();
        fail(std::string("Could not start screen capture: ") + e.what());
        return false;
    }

    width_ = capture_->width();
    height_ = capture_->height();
    refreshHz_ = capture_->refreshHz();
    stats_ = std::make_unique<TripleBuffer<PipelineStats>>();
    error_.clear();
    state_.store(State::Running, std::memory_order_release);

    thread_ = std::jthread([this](std::stop_token stop) { run(stop); });
    return true;
}

void Pipeline::stop() {
    if (thread_.joinable()) {
        thread_.request_stop();
        thread_.join();
    }
    preview_.reset();
    recording_.reset();
    capture_.reset();
    gpu_ = {};
    state_.store(State::Stopped, std::memory_order_release);
}

const PipelineStats& Pipeline::stats() {
    static const PipelineStats kNone{};
    if (!stats_) {
        return kNone;
    }
    stats_->fetch();
    return stats_->readSlot();
}

void Pipeline::run(std::stop_token stop) {
    // The thread mostly sleeps inside acquire(); top priority makes it run the moment a new image arrives.
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    SetThreadDescription(GetCurrentThread(), L"observex pipeline");

    LARGE_INTEGER frequency;
    QueryPerformanceFrequency(&frequency);
    const double msPerTick = 1000.0 / static_cast<double>(frequency.QuadPart);

    StatWindow latency;
    StatWindow work;
    std::uint64_t images = 0;
    std::uint64_t merged = 0;
    std::int64_t windowStart = qpcNow();
    std::int64_t nextRecording = 0; // When the next image is due for recording.
    ScreenCapture::Frame frame;

    while (!stop.stop_requested()) {
        const ScreenCapture::Status status = capture_->acquire(kAcquireTimeoutMs, frame);
        const std::int64_t now = qpcNow();

        if (status == ScreenCapture::Status::NewImage) {
            ++images;
            merged += frame.updates > 1 ? frame.updates - 1 : 0;
            latency.add(static_cast<float>(static_cast<double>(now - frame.presentQpc) * msPerTick));

            // Recognise and react will run here, on the image while it is still on the GPU.

            if (previewEnabled_.load(std::memory_order_relaxed)) {
                preview_->write(gpu_.context.Get(), frame.texture);
            }
            const unsigned recordingRate = recordingRate_.load(std::memory_order_relaxed);
            if (recordingRate != 0 && now >= nextRecording) {
                recording_->write(gpu_.context.Get(), frame.texture);
                const std::int64_t period = frequency.QuadPart / recordingRate;
                nextRecording += period;
                if (nextRecording <= now) {
                    nextRecording = now + period; // Starting, or the screen was still: count from now.
                }
            }
            work.add(static_cast<float>(static_cast<double>(qpcNow() - now) * msPerTick));
        } else if (status == ScreenCapture::Status::Lost) {
            if (!reconnect(stop)) {
                return;
            }
        } else if (status == ScreenCapture::Status::Failed) {
            fail("Screen capture stopped working (the GPU may have been reset or removed)");
            return;
        }

        // Once a second, summarise and hand the numbers to the UI (lock-free).
        const std::int64_t elapsed = now - windowStart;
        if (elapsed >= frequency.QuadPart) {
            const StatWindow::Summary latencySummary = latency.take();
            const StatWindow::Summary workSummary = work.take();
            PipelineStats& stats = stats_->writeSlot();
            stats.images = images;
            stats.mergedUpdates = merged;
            stats.imagesPerSecond =
                static_cast<float>(latencySummary.count / (static_cast<double>(elapsed) * msPerTick / 1000.0));
            stats.latencyAvgMs = latencySummary.average;
            stats.latencyMaxMs = latencySummary.max;
            stats.workAvgMs = workSummary.average;
            stats.workMaxMs = workSummary.max;
            stats_->publish();
            windowStart = now;
        }
    }
}

bool Pipeline::reconnect(std::stop_token stop) {
    // Interrupted by a display change, fullscreen switch, UAC prompt or the lock screen.
    state_.store(State::Reconnecting, std::memory_order_release);
    while (!stop.stop_requested()) {
        if (capture_->reopen()) {
            if (capture_->width() != width_ || capture_->height() != height_ || capture_->refreshHz() != refreshHz_) {
                state_.store(State::NeedsRestart, std::memory_order_release);
                return false;
            }
            state_.store(State::Running, std::memory_order_release);
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    return false;
}

void Pipeline::fail(std::string message) {
    error_ = std::move(message);
    state_.store(State::Failed, std::memory_order_release);
}

} // namespace observex
