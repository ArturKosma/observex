#include "engine/DropFinder.h"

#include "core/StatWindow.h"
#include "engine/Detector.h"

#include <algorithm>
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

std::uint64_t pack(LootSquare square) {
    if (square.empty()) {
        return 0;
    }
    return (static_cast<std::uint64_t>(square.x & 0xFFFF) << 32) | (static_cast<std::uint64_t>(square.y & 0xFFFF) << 16) |
           static_cast<std::uint64_t>(square.side & 0xFFFF);
}

LootSquare unpack(std::uint64_t packed) {
    return {static_cast<int>((packed >> 32) & 0xFFFF), static_cast<int>((packed >> 16) & 0xFFFF),
            static_cast<int>(packed & 0xFFFF)};
}

} // namespace

DropFinder::DropFinder(IDXGIAdapter1* gpu, ImageChannel& channel, std::filesystem::path modelFolder)
    : modelFolder_(std::move(modelFolder)), width_(channel.width()), height_(channel.height()),
      sweep_(lootTiles(static_cast<int>(width_), static_cast<int>(height_))) {
    adapter_ = findOwnAdapter(gpu); // An adapter object of its own: cannot hold up the others.
    gpu_ = createGpuDevice(adapter_.Get());
    reader_ = std::make_unique<ImageReader>(gpu_.device.Get(), channel);
    thread_ = std::jthread([this](std::stop_token stop) { run(stop); });
}

DropFinder::~DropFinder() {
    if (thread_.joinable()) {
        thread_.request_stop();
        thread_.join();
    }
}

void DropFinder::lookAt(LootSquare square) {
    square_.store(pack(square), std::memory_order_relaxed);
}

const FoundDrops& DropFinder::found() {
    found_.fetch();
    return found_.readSlot();
}

const DropFinderStats& DropFinder::stats() {
    stats_.fetch();
    return stats_.readSlot();
}

void DropFinder::run(std::stop_token stop) {
    // Below the capture and the recogniser: a kill's drops lie there for many seconds.
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    SetThreadDescription(GetCurrentThread(), L"observex drop finder");

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
    const double perTick = 1000.0 / static_cast<double>(frequency.QuadPart);
    StatWindow work;
    // The Debug tab's steps, in the order they run on each image.
    enum Step { kCopy, kNetwork, kHandOver };
    CostMeter meter({"Copy in", "Squeeze and network", "Hand-over"});
    std::int64_t windowStart = qpcNow();
    FoundDrops newest;       // Both sorts of look, each handed on again with the other's newest.
    std::size_t next = 0;    // The sweep's next square.
    bool lastClose = false;  // The last look was the close one.

    ImageStamp stamp;
    while (!stop.stop_requested()) {
        // Once a second, the numbers to the UI (lock-free).
        if (const std::int64_t now = qpcNow(), elapsed = now - windowStart; elapsed >= frequency.QuadPart) {
            const StatWindow::Summary summary = work.take();
            DropFinderStats& stats = stats_.writeSlot();
            stats.looksPerSecond = static_cast<float>(summary.count / (static_cast<double>(elapsed) * perTick / 1000.0));
            stats.workAvgMs = summary.average;
            stats.workMaxMs = summary.max;
            stats.costs = meter.take(static_cast<double>(elapsed) * perTick / 1000.0);
            stats_.publish();
            windowStart = now;
        }
        if (!reader_->waitForImage(kWaitMs)) {
            continue;
        }
        const std::int64_t start = qpcNow();
        meter.begin();
        if (!reader_->copyNewImage(gpu_.context.Get(), detector->input(), &stamp) || sweep_.empty()) {
            continue;
        }
        meter.lap(kCopy);
        // The close square every other image while there is one; the sweep's next on the others.
        const LootSquare close = unpack(square_.load(std::memory_order_relaxed));
        lastClose = !close.empty() && !lastClose;
        DropLook& look = lastClose ? newest.close : newest.sweep;
        look.square = lastClose ? close : sweep_[next++ % sweep_.size()];
        look.count = detector->detect(gpu_.context.Get(), kMinSure, look.drops.data(), DropLook::kMax, false,
                                      {look.square.x, look.square.y, look.square.side});
        if (look.count < 0) {
            fail("The GPU stopped running the drop model (it may have been reset)");
            return;
        }
        meter.lap(kNetwork);
        look.image = stamp.number;
        look.imageQpc = stamp.shownQpc != 0 ? stamp.shownQpc : start;
        found_.writeSlot() = newest;
        found_.publish();
        meter.lap(kHandOver);
        meter.end();
        work.add(static_cast<float>(static_cast<double>(qpcNow() - start) * perTick));
    }
}

void DropFinder::fail(std::string message) {
    error_ = std::move(message);
    state_.store(State::Failed, std::memory_order_release);
}

} // namespace observex
