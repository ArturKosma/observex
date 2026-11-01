#include "engine/Recognizer.h"

#include "core/StatWindow.h"
#include "engine/ImageChannel.h"

#include <exception>
#include <utility>

namespace observex {

namespace {

// Longest wait for a new image. Also how quickly the thread notices a stop request.
constexpr unsigned kWaitMs = 50;

std::int64_t qpcNow() {
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return now.QuadPart;
}

} // namespace

Recognizer::Recognizer(const Screen& screen, ImageChannel& channel, std::filesystem::path modelFolder)
    : modelFolder_(std::move(modelFolder)), width_(channel.width()), height_(channel.height()) {
    const Screen own = findOnOwnAdapter(screen); // So this device cannot hold up the pipeline's.
    adapter_ = own.adapter;
    gpu_ = createGpuDevice(own.adapter.Get());
    reader_ = std::make_unique<ImageReader>(gpu_.device.Get(), channel);
    thread_ = std::jthread([this](std::stop_token stop) { run(stop); });
}

Recognizer::~Recognizer() {
    if (thread_.joinable()) {
        thread_.request_stop();
        thread_.join();
    }
}

const Found& Recognizer::found() {
    found_.fetch();
    return found_.readSlot();
}

const RecognizerStats& Recognizer::stats() {
    stats_.fetch();
    return stats_.readSlot();
}

void Recognizer::run(std::stop_token stop) {
    // Above normal: it mostly waits for the GPU, and what it finds is what observex reacts to.
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
    SetThreadDescription(GetCurrentThread(), L"observex recogniser");

    std::unique_ptr<Detector> detector;
    try {
        detector = std::make_unique<Detector>(gpu_.device.Get(), adapter_.Get(), modelFolder_, width_, height_, stop,
                                              progress_);
    } catch (const std::exception& e) {
        if (!stop.stop_requested()) {
            fail(e.what());
        }
        return;
    }
    state_.store(State::Running, std::memory_order_release);

    LARGE_INTEGER frequency;
    QueryPerformanceFrequency(&frequency);
    const double msPerTick = 1000.0 / static_cast<double>(frequency.QuadPart);
    StatWindow work;
    std::uint64_t images = 0;
    std::int64_t windowStart = qpcNow();

    while (!stop.stop_requested()) {
        if (reader_->waitForImage(kWaitMs) && reader_->copyNewImage(gpu_.context.Get(), detector->input())) {
            const std::int64_t start = qpcNow();
            Found& found = found_.writeSlot();
            const int count =
                detector->detect(gpu_.context.Get(), kMinSure, found.things.data(), static_cast<int>(Found::kMax));
            if (count < 0) {
                fail("The GPU stopped running the model (it may have been reset)");
                return;
            }
            found.count = count;
            found.image = ++images;
            found_.publish();
            work.add(static_cast<float>(static_cast<double>(qpcNow() - start) * msPerTick));
        }

        // Once a second, summarise and hand the numbers to the UI (lock-free).
        const std::int64_t now = qpcNow();
        const std::int64_t elapsed = now - windowStart;
        if (elapsed >= frequency.QuadPart) {
            const StatWindow::Summary summary = work.take();
            RecognizerStats& stats = stats_.writeSlot();
            stats.imagesPerSecond =
                static_cast<float>(summary.count / (static_cast<double>(elapsed) * msPerTick / 1000.0));
            stats.workAvgMs = summary.average;
            stats.workMaxMs = summary.max;
            stats_.publish();
            windowStart = now;
        }
    }
}

void Recognizer::fail(std::string message) {
    error_ = std::move(message);
    state_.store(State::Failed, std::memory_order_release);
}

} // namespace observex
