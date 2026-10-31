#include "engine/Pipeline.h"

#include "core/LogStore.h"
#include "core/StatWindow.h"
#include "engine/PreviewChannel.h"
#include "engine/ScreenCapture.h"

#include <chrono>
#include <cstdio>
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

Pipeline::Pipeline(LogStore& log) : log_(log) {}

Pipeline::~Pipeline() {
    stop();
}

bool Pipeline::start(const CaptureTarget& target) {
    stop();
    try {
        gpu_ = createGpuDevice(target.adapter.Get());
        capture_ = std::make_unique<ScreenCapture>(gpu_.device.Get(), target.output.Get());
        preview_ = std::make_unique<PreviewChannel>(gpu_.device.Get(), capture_->width(), capture_->height(),
                                                    ScreenCapture::kImageFormat);
    } catch (const std::exception& e) {
        stop();
        error_ = std::string("Could not start screen capture: ") + e.what();
        log(LogLevel::Error, error_);
        state_.store(State::Failed, std::memory_order_release);
        return false;
    }

    targetName_ = target.name;
    width_ = capture_->width();
    height_ = capture_->height();
    refreshHz_ = capture_->refreshHz();
    stats_ = std::make_unique<TripleBuffer<PipelineStats>>();
    error_.clear();
    state_.store(State::Running, std::memory_order_release);

    char text[256];
    std::snprintf(text, sizeof(text), "Capturing %s, %u x %u at %.0f Hz", targetName_.c_str(), width_, height_,
                  refreshHz_);
    log(LogLevel::Info, text);

    thread_ = std::jthread([this](std::stop_token stop) { run(stop); });
    return true;
}

void Pipeline::stop() {
    if (thread_.joinable()) {
        thread_.request_stop();
        thread_.join();
    }
    preview_.reset();
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
    state_.store(State::Reconnecting, std::memory_order_release);
    log(LogLevel::Warning,
        "Screen capture interrupted (display change, fullscreen switch, UAC prompt or lock screen). Reconnecting.");

    while (!stop.stop_requested()) {
        if (capture_->reopen()) {
            if (capture_->width() != width_ || capture_->height() != height_ || capture_->refreshHz() != refreshHz_) {
                log(LogLevel::Info, "Display mode changed. Restarting capture.");
                state_.store(State::NeedsRestart, std::memory_order_release);
                return false;
            }
            log(LogLevel::Info, "Screen capture reconnected.");
            state_.store(State::Running, std::memory_order_release);
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    return false;
}

void Pipeline::fail(std::string message) {
    error_ = std::move(message);
    log(LogLevel::Error, error_);
    state_.store(State::Failed, std::memory_order_release);
}

void Pipeline::log(LogLevel level, std::string message) {
    // Takes the log's lock, so the loop calls this only for rare events, never per image.
    LogEntry entry;
    entry.time = Clock::now();
    entry.level = level;
    entry.source = "pipeline";
    entry.message = std::move(message);
    log_.push(std::move(entry));
}

} // namespace observex
