#include "engine/ClipDrops.h"

#include "core/LootWatch.h"
#include "engine/Detector.h"
#include "engine/DropFinder.h"

#include <algorithm>
#include <exception>
#include <utility>
#include <vector>

namespace observex {

namespace {

// Longest wait for a new frame. Also how quickly the thread notices a stop request.
constexpr unsigned kWaitMs = 50;

} // namespace

ClipDrops::ClipDrops(IDXGIAdapter1* gpu, ImageChannel& channel, std::filesystem::path modelFolder)
    : modelFolder_(std::move(modelFolder)), width_(channel.width()), height_(channel.height()) {
    adapter_ = findOwnAdapter(gpu); // An adapter object of its own: cannot hold up the others.
    gpu_ = createGpuDevice(adapter_.Get());
    reader_ = std::make_unique<ImageReader>(gpu_.device.Get(), channel);
    thread_ = std::jthread([this](std::stop_token stop) { run(stop); });
}

ClipDrops::~ClipDrops() {
    if (thread_.joinable()) {
        thread_.request_stop();
        thread_.join();
    }
}

const ClipFrameDrops& ClipDrops::found() {
    found_.fetch();
    return found_.readSlot();
}

void ClipDrops::run(std::stop_token stop) {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    SetThreadDescription(GetCurrentThread(), L"observex clip drops");

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

    const std::vector<LootSquare> squares = lootTiles(static_cast<int>(width_), static_cast<int>(height_));
    std::vector<Detection> fromSquares;
    std::array<Detection, DropLook::kMax> found{};
    ImageStamp stamp;
    while (!stop.stop_requested()) {
        if (!reader_->waitForImage(kWaitMs)) {
            continue;
        }
        if (!reader_->copyNewImage(gpu_.context.Get(), detector->input(), &stamp)) {
            continue;
        }
        // Every square, live's cut-off, then the surest of each place.
        fromSquares.clear();
        for (const LootSquare& square : squares) {
            if (stop.stop_requested()) {
                return;
            }
            const int seen = detector->detect(gpu_.context.Get(), DropFinder::kMinSure, found.data(),
                                              static_cast<int>(found.size()), false,
                                              {square.x, square.y, square.side});
            if (seen < 0) {
                fail("The GPU stopped running the drop model (it may have been reset)");
                return;
            }
            fromSquares.insert(fromSquares.end(), found.begin(), found.begin() + seen);
        }
        const std::vector<int> kept = keepApart(fromSquares);
        ClipFrameDrops& out = found_.writeSlot();
        out.frame = stamp.number;
        out.count = std::min(static_cast<int>(kept.size()), ClipFrameDrops::kMax);
        for (int i = 0; i < out.count; ++i) {
            out.drops[static_cast<std::size_t>(i)] =
                fromSquares[static_cast<std::size_t>(kept[static_cast<std::size_t>(i)])];
        }
        found_.publish();
    }
}

void ClipDrops::fail(std::string message) {
    error_ = std::move(message);
    state_.store(State::Failed, std::memory_order_release);
}

} // namespace observex
