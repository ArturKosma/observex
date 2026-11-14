#include "engine/GroundFinder.h"

#include "core/StatWindow.h"
#include "engine/GreyMaker.h"

#include <algorithm>
#include <cmath>
#include <exception>
#include <utility>
#include <vector>

namespace observex {

namespace {

// Longest wait for a new image. Also how quickly the thread notices a stop request.
constexpr unsigned kWaitMs = 50;
// Followed kImagesPerSecond: an image shown this much before the one due stands for it (their times jitter by a ms).
constexpr double kFollowGapMs = 1000.0 / GroundFinder::kImagesPerSecond;
constexpr double kJitterMs = 2.0;

std::int64_t qpcNow() {
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return now.QuadPart;
}

double msPerTick() {
    LARGE_INTEGER frequency;
    QueryPerformanceFrequency(&frequency);
    return 1000.0 / static_cast<double>(frequency.QuadPart);
}

} // namespace

double groundMs(std::int64_t qpc) {
    static const double perTick = msPerTick();
    return static_cast<double>(qpc) * perTick;
}

GroundFinder::GroundFinder(IDXGIAdapter1* gpu, ImageChannel& channel,
                           std::shared_ptr<const HeuristicsSource> heuristics)
    : heuristics_(std::move(heuristics)), width_(channel.width()), height_(channel.height()) {
    gpu_ = createGpuDevice(findOwnAdapter(gpu).Get()); // An adapter object of its own: cannot hold up the others.
    reader_ = std::make_unique<ImageReader>(gpu_.device.Get(), channel);
    thread_ = std::jthread([this](std::stop_token stop) { run(stop); });
}

GroundFinder::~GroundFinder() {
    if (thread_.joinable()) {
        thread_.request_stop();
        thread_.join();
    }
}

void GroundFinder::hint(std::span<const Tracked> things, int ownKind, const UiPieces* ui,
                        std::shared_ptr<const DepthMade> depth) {
    Hint& hint = hints_.writeSlot();
    hint.qpc = qpcNow();
    hint.depth = std::move(depth);
    const Tracked* own = ownCharacter(things, ownKind, static_cast<float>(width_));
    hint.hasOwn = own != nullptr;
    hint.ownTop = own != nullptr ? own->y : 0.0f;
    hint.ownBottom = own != nullptr ? own->y + own->height : 0.0f;
    hint.avoidCount = 0;
    for (const Tracked& thing : things) {
        if (hint.avoidCount == kMaxAvoid) {
            break;
        }
        // A little wider: its box may lag it, and its shadow moves with it.
        const float marginX = thing.width * 0.1f;
        const float marginY = thing.height * 0.1f;
        hint.avoid[static_cast<std::size_t>(hint.avoidCount++)] = {
            static_cast<int>(std::floor(thing.x - marginX)), static_cast<int>(std::floor(thing.y - marginY)),
            static_cast<int>(std::ceil(thing.x + thing.width + marginX)),
            static_cast<int>(std::ceil(thing.y + thing.height + marginY))};
    }
    hint.windowCount = 0;
    for (int i = 0; ui != nullptr && i < ui->count; ++i) {
        const UiPiece& piece = ui->pieces[static_cast<std::size_t>(i)];
        const GreyArea area{piece.x0, piece.y0, piece.x1, piece.y1};
        if (hint.avoidCount < kMaxAvoid) {
            hint.avoid[static_cast<std::size_t>(hint.avoidCount++)] = area;
        }
        if (hint.windowCount < static_cast<int>(hint.windows.size())) {
            hint.windows[static_cast<std::size_t>(hint.windowCount++)] = area;
        }
    }
    hints_.publish();
}

const GroundSeen& GroundFinder::seen() {
    seen_.fetch();
    return seen_.readSlot();
}

const GroundFinderStats& GroundFinder::stats() {
    stats_.fetch();
    return stats_.readSlot();
}

void GroundFinder::run(std::stop_token stop) {
    // Below the capture, the recogniser and the snapper: the plane may come a frame late.
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    SetThreadDescription(GetCurrentThread(), L"observex ground finder");

    std::unique_ptr<GreyMaker> greys;
    std::unique_ptr<Grounding> follower;
    std::uint64_t heuristicsVersion = heuristics_->version();
    try {
        greys = std::make_unique<GreyMaker>(gpu_.device.Get(), width_, height_, 1); // Only the newest.
        follower = heuristics_->makeGround();
    } catch (const std::exception& e) {
        fail(e.what());
        return;
    }

    LARGE_INTEGER frequency;
    QueryPerformanceFrequency(&frequency);
    const double perTick = 1000.0 / static_cast<double>(frequency.QuadPart);
    StatWindow work;
    StatWindow fitting;
    StatWindow heading;
    StatWindow depthing;
    int depthMeasured = 0;
    double depthOff = 0.0;
    int images = 0;
    int fitted = 0;
    int searched = 0;
    int headed = 0;
    double matches = 0.0;
    double agreeing = 0.0;
    double missed = 0.0;
    std::int64_t windowStart = qpcNow();
    const auto publishStats = [&](std::int64_t elapsed) {
        const StatWindow::Summary workSummary = work.take();
        const StatWindow::Summary fitSummary = fitting.take();
        const StatWindow::Summary headSummary = heading.take();
        const StatWindow::Summary depthSummary = depthing.take();
        GroundFinderStats& stats = stats_.writeSlot();
        stats.imagesPerSecond =
            elapsed > 0 ? static_cast<float>(workSummary.count / (static_cast<double>(elapsed) * perTick / 1000.0))
                        : 0.0f;
        stats.workAvgMs = workSummary.average;
        stats.workMaxMs = workSummary.max;
        stats.fitAvgMs = fitSummary.average;
        stats.fitted = images > 0 ? static_cast<float>(fitted) / static_cast<float>(images) : 0.0f;
        stats.searched = images > 0 ? static_cast<float>(searched) / static_cast<float>(images) : 0.0f;
        stats.matches = images > 0 ? static_cast<float>(matches / images) : 0.0f;
        stats.agreeing = fitted > 0 ? static_cast<float>(agreeing / fitted) : 0.0f;
        stats.missPixels = fitted > 0 ? static_cast<float>(missed / fitted) : 0.0f;
        stats.headed = images > 0 ? static_cast<float>(headed) / static_cast<float>(images) : 0.0f;
        stats.headsPerSecond =
            elapsed > 0 ? static_cast<float>(headSummary.count / (static_cast<double>(elapsed) * perTick / 1000.0))
                        : 0.0f;
        stats.headAvgMs = headSummary.average;
        stats.depthPerSecond =
            elapsed > 0 ? static_cast<float>(depthSummary.count / (static_cast<double>(elapsed) * perTick / 1000.0))
                        : 0.0f;
        stats.depthMeasured =
            depthSummary.count > 0 ? static_cast<float>(depthMeasured) / static_cast<float>(depthSummary.count) : 0.0f;
        stats.depthOffDegrees = depthMeasured > 0 ? static_cast<float>(depthOff / depthMeasured * 57.29578) : 0.0f;
        stats.depthAvgMs = depthSummary.average;
        depthMeasured = 0;
        depthOff = 0.0;
        stats_.publish();
        images = 0;
        fitted = 0;
        searched = 0;
        headed = 0;
        matches = agreeing = missed = 0.0;
    };
    state_.store(State::Running, std::memory_order_release);

    const GreyArea whole{0, 0, static_cast<int>(width_), static_cast<int>(height_)};
    std::array<GreyArea, 8> full{};
    double nextFollowMs = -1e300;
    std::uint64_t followedImage = 0;
    std::uint64_t depthGiven = 0; // The depth map last given the follower (its image).
    ImageStamp stamp;
    while (!stop.stop_requested()) {
        // Once a second, the numbers to the UI (lock-free).
        if (const std::int64_t now = qpcNow(), elapsed = now - windowStart; elapsed >= frequency.QuadPart) {
            publishStats(elapsed);
            windowStart = now;
        }
        if (reader_->waitForImage(kWaitMs)) {
            const std::int64_t start = qpcNow();
            if (ID3D11Texture2D* image = reader_->useNewImage(&stamp)) {
                // New following once the app loaded a rebuilt DLL (it starts from the start; if it cannot be made,
                // the one it has goes on).
                if (const std::uint64_t version = heuristics_->version(); version != heuristicsVersion) {
                    heuristicsVersion = version;
                    try {
                        follower = heuristics_->makeGround();
                    } catch (const std::exception&) {
                    }
                }
                if (restart_.exchange(false, std::memory_order_relaxed)) {
                    follower->clear();
                }
                hints_.fetch();
                const Hint& hint = hints_.readSlot();
                // A new depth map: the camera's pitch by it, before this image is followed.
                if (hint.depth && hint.depth->image != depthGiven) {
                    depthGiven = hint.depth->image;
                    const std::int64_t depthStart = qpcNow();
                    const GroundDepthNote depth = follower->depth(hint.depth->input(), groundMs(hint.depth->imageQpc));
                    depthing.add(static_cast<float>(static_cast<double>(qpcNow() - depthStart) * perTick));
                    if (depth.measured) {
                        ++depthMeasured;
                        depthOff += std::fabs(depth.pitch - depth.then);
                    }
                }
                const bool fresh = hint.qpc != 0 && (start - hint.qpc) * perTick < kHintMs;
                GroundHints hints;
                hints.hasOwn = fresh && hint.hasOwn;
                hints.ownTop = hint.ownTop;
                hints.ownBottom = hint.ownBottom;
                hints.avoid = hint.avoid.data();
                hints.avoidCount = fresh ? hint.avoidCount : 0;
                hints.windows = hint.windows.data();
                hints.windowCount = hint.qpc != 0 ? hint.windowCount : 0;
                const double timeMs = groundMs(stamp.shownQpc != 0 ? stamp.shownQpc : start);
                if (timeMs < nextFollowMs - kJitterMs) {
                    // Between follows: only the eighth size (58 kB) and the minimap's cone, to turn the camera as
                    // the ground and the cone did.
                    const int count = follower->headAreas(timeMs, hints, full.data(), static_cast<int>(full.size()));
                    const GreyView* grey = nullptr;
                    try {
                        greys->make(gpu_.context.Get(), stamp.number, image);
                        reader_->endUse();
                        if (count > 0) {
                            greys->read(gpu_.context.Get(), stamp.number,
                                        {full.data(), static_cast<std::size_t>(count)}, 1);
                        }
                        greys->read(gpu_.context.Get(), stamp.number, {&whole, 1}, GreyView::kLevels,
                                    follower->headFirstLevel());
                        greys->wait(gpu_.context.Get());
                        grey = greys->find(stamp.number);
                    } catch (const std::exception&) {
                        reader_->endUse(); // Harmless once ended.
                        fail("The GPU stopped making grey images (it may have been reset)");
                        return;
                    }
                    if (grey != nullptr) {
                        follower->head(*grey);
                        GroundSeen& seen = seen_.writeSlot();
                        seen.camera = follower->camera();
                        seen.image = stamp.number;
                        seen.followed = followedImage;
                        seen_.publish();
                    }
                    heading.add(static_cast<float>(static_cast<double>(qpcNow() - start) * perTick));
                    continue;
                }
                nextFollowMs = std::max(nextFollowMs, timeMs - kJitterMs) + kFollowGapMs;
                follower->begin(timeMs, static_cast<int>(width_), static_cast<int>(height_), hints);
                // Its grey image first, so the image goes back to the channel at once; then only the levels the
                // follower matches on come back (quarter size and smaller: 0.3 MB for a 2560 x 1440 screen), and
                // the full size round the minimap's cone.
                const GreyView* grey = nullptr;
                try {
                    greys->make(gpu_.context.Get(), stamp.number, image);
                    reader_->endUse();
                    if (const int count = follower->fullAreas(full.data(), static_cast<int>(full.size())); count > 0) {
                        greys->read(gpu_.context.Get(), stamp.number, {full.data(), static_cast<std::size_t>(count)},
                                    1);
                    }
                    greys->read(gpu_.context.Get(), stamp.number, {&whole, 1}, GreyView::kLevels,
                                follower->firstLevel());
                    greys->wait(gpu_.context.Get());
                    grey = greys->find(stamp.number);
                } catch (const std::exception&) {
                    reader_->endUse(); // Harmless once ended.
                    fail("The GPU stopped making grey images (it may have been reset)");
                    return;
                }
                const std::int64_t read = qpcNow();
                const bool fit = grey != nullptr && follower->end(*grey);
                GroundSeen& seen = seen_.writeSlot();
                followedImage = stamp.number;
                seen.camera = follower->camera();
                seen.image = stamp.number;
                seen.followed = followedImage;
                seen_.publish();
                const std::int64_t done = qpcNow();
                work.add(static_cast<float>(static_cast<double>(done - start) * perTick));
                fitting.add(static_cast<float>(static_cast<double>(done - read) * perTick));
                const GroundNote note = follower->note();
                ++images;
                matches += note.found;
                searched += note.motion == GroundFit::Motion::Searched ? 1 : 0;
                headed += note.headed ? 1 : 0;
                if (fit) {
                    ++fitted;
                    agreeing += note.agreeing;
                    missed += note.miss;
                }
            }
        }
    }
}

void GroundFinder::fail(std::string message) {
    error_ = std::move(message);
    state_.store(State::Failed, std::memory_order_release);
}

} // namespace observex
