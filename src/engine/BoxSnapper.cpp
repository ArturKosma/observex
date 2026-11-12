#include "engine/BoxSnapper.h"

#include "core/StatWindow.h"
#include "engine/ClipRecorder.h"
#include "engine/GreyMaker.h"

#include <algorithm>
#include <array>
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

BoxSnapper::BoxSnapper(const Screen& screen, ImageChannel& channel, std::shared_ptr<const HeuristicsSource> heuristics)
    : heuristics_(std::move(heuristics)), width_(channel.width()), height_(channel.height()) {
    const Screen own = findOnOwnAdapter(screen); // So this device cannot hold up the others.
    gpu_ = createGpuDevice(own.adapter.Get());
    reader_ = std::make_unique<ImageReader>(gpu_.device.Get(), channel);
    thread_ = std::jthread([this](std::stop_token stop) { run(stop); });
}

BoxSnapper::~BoxSnapper() {
    if (thread_.joinable()) {
        thread_.request_stop();
        thread_.join();
    }
}

void BoxSnapper::looked(const ImageStamp& stamp, std::span<const Tracked> shown) {
    Look& look = looks_.writeSlot();
    look.stamp = stamp;
    look.count = static_cast<int>(std::min(shown.size(), look.shown.size()));
    std::copy_n(shown.begin(), look.count, look.shown.begin());
    looks_.publish();
}

const Found& BoxSnapper::found() {
    found_.fetch();
    return found_.readSlot();
}

const SnapperStats& BoxSnapper::stats() {
    stats_.fetch();
    return stats_.readSlot();
}

void BoxSnapper::run(std::stop_token stop) {
    // Highest: what it hands out is how far off the boxes are, and its work is short (about 1 ms an image).
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
    SetThreadDescription(GetCurrentThread(), L"observex snapper");

    std::unique_ptr<GreyMaker> greys;
    std::unique_ptr<Snapping> snapping;
    std::uint64_t heuristicsVersion = heuristics_->version();
    try {
        greys = std::make_unique<GreyMaker>(gpu_.device.Get(), width_, height_);
        snapping = heuristics_->makeSnapping();
    } catch (const std::exception& e) {
        fail(e.what());
        return;
    }
    state_.store(State::Running, std::memory_order_release);

    LARGE_INTEGER frequency;
    QueryPerformanceFrequency(&frequency);
    const double msPerTick = 1000.0 / static_cast<double>(frequency.QuadPart);
    const std::int64_t timeOrigin = qpcNow();
    const auto msOf = [&](std::int64_t qpc) { return static_cast<double>(qpc - timeOrigin) * msPerTick; };
    StatWindow work;
    StatWindow late;
    std::uint64_t boxes = 0;
    std::uint64_t snapped = 0;
    std::uint64_t images = 0;
    std::int64_t windowStart = timeOrigin;
    std::array<GreyArea, Found::kMax> learnAreas;
    std::array<GreyArea, Found::kMax> snapAreas;

    ImageStamp stamp;
    while (!stop.stop_requested()) {
        if (reader_->waitForImage(kWaitMs)) {
            const std::int64_t start = qpcNow();
            if (ID3D11Texture2D* image = reader_->useNewImage(&stamp)) {
                // Its grey levels first, so the image goes back to the channel at once.
                bool made = true;
                try {
                    greys->make(gpu_.context.Get(), stamp.number, image);
                } catch (const std::exception&) {
                    made = false;
                }
                reader_->endUse();
                if (!made) {
                    fail("The GPU stopped making grey images (it may have been reset)");
                    return;
                }
                // New snapping once the app loaded a rebuilt DLL (if it cannot be made, the one it has goes on).
                if (const std::uint64_t version = heuristics_->version(); version != heuristicsVersion) {
                    heuristicsVersion = version;
                    try {
                        snapping = heuristics_->makeSnapping();
                    } catch (const std::exception&) {
                    }
                }
                const std::int64_t shownQpc = stamp.shownQpc != 0 ? stamp.shownQpc : start;
                const double timeMs = msOf(shownQpc);
                // The newest look, if a new one came: learned from its own image if that is still kept (it is a few
                // images old). Only the parts of the images the snapping reads are read back (GreyMaker).
                const Look* look = looks_.fetch() ? &looks_.readSlot() : nullptr;
                double lookMs = 0.0;
                std::span<const Tracked> shown;
                int learnCount = 0;
                if (look != nullptr) {
                    lookMs = msOf(look->stamp.shownQpc != 0 ? look->stamp.shownQpc : start);
                    shown = {look->shown.data(), static_cast<std::size_t>(look->count)};
                    learnCount = snapping->learnAreas(shown, learnAreas.data(), static_cast<int>(learnAreas.size()));
                }
                const int snapCount = snapping->snapAreas(timeMs, look != nullptr, lookMs, shown, snapAreas.data(),
                                                          static_cast<int>(snapAreas.size()));
                const GreyView* grey = nullptr;
                try {
                    if (look != nullptr) {
                        greys->read(gpu_.context.Get(), look->stamp.number,
                                    {learnAreas.data(), static_cast<std::size_t>(learnCount)});
                    }
                    greys->read(gpu_.context.Get(), stamp.number, {snapAreas.data(), static_cast<std::size_t>(snapCount)});
                    greys->wait(gpu_.context.Get());
                    grey = greys->find(stamp.number);
                } catch (const std::exception&) {
                    fail("The GPU stopped making grey images (it may have been reset)");
                    return;
                }
                if (look != nullptr) {
                    snapping->looked(lookMs, greys->find(look->stamp.number), shown);
                }
                Found& found = found_.writeSlot();
                found.count = snapping->snap(timeMs, *grey, found.things.data(), Found::kMax);
                found.image = ++images;
                found_.publish();
                const std::int64_t done = qpcNow();
                work.add(static_cast<float>(static_cast<double>(done - start) * msPerTick));
                late.add(static_cast<float>(static_cast<double>(done - shownQpc) * msPerTick));
                boxes += static_cast<std::uint64_t>(found.count);
                for (int i = 0; i < found.count; ++i) {
                    snapped += found.things[static_cast<std::size_t>(i)].held ? 0 : 1;
                }
                // The published slot is not written again until it comes back as a write slot, so it can be read here.
                if (const std::shared_ptr<ClipRecorder> clip = clip_.load(std::memory_order_acquire)) {
                    clip->snapped(stamp, done, snapping->leadMs(),
                                  {found.things.data(), static_cast<std::size_t>(found.count)},
                                  look != nullptr ? look->stamp.number : 0, snapping->finds());
                }
            }
        }

        // Once a second, summarise and hand the numbers to the UI (lock-free).
        const std::int64_t now = qpcNow();
        const std::int64_t elapsed = now - windowStart;
        if (elapsed >= frequency.QuadPart) {
            const StatWindow::Summary workSummary = work.take();
            const StatWindow::Summary lateSummary = late.take();
            SnapperStats& stats = stats_.writeSlot();
            stats.imagesPerSecond =
                static_cast<float>(workSummary.count / (static_cast<double>(elapsed) * msPerTick / 1000.0));
            stats.workAvgMs = workSummary.average;
            stats.workMaxMs = workSummary.max;
            stats.lateAvgMs = lateSummary.average;
            stats.snapped = boxes > 0 ? static_cast<float>(static_cast<double>(snapped) / static_cast<double>(boxes))
                                      : 0.0f;
            stats_.publish();
            boxes = 0;
            snapped = 0;
            windowStart = now;
        }
    }
}

void BoxSnapper::fail(std::string message) {
    error_ = std::move(message);
    state_.store(State::Failed, std::memory_order_release);
}

} // namespace observex
