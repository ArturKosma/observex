#include "engine/DepthFinder.h"

#include "core/StatWindow.h"
#include "engine/DepthNet.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <utility>

namespace observex {

namespace {

// Longest wait for a new image. Also how quickly the thread notices a stop request.
constexpr unsigned kWaitMs = 50;
// A level moving more than this from one map to the next counts as changed (DepthFinderStats::changed).
constexpr int kChangedLevels = 8;

std::int64_t qpcNow() {
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return now.QuadPart;
}

} // namespace

DepthFinder::DepthFinder(IDXGIAdapter1* gpu, ImageChannel& channel, std::shared_ptr<const HeuristicsSource> heuristics,
                         std::filesystem::path modelFolder, Record record)
    : heuristics_(std::move(heuristics)), width_(channel.width()), height_(channel.height()),
      modelFolder_(std::move(modelFolder)), record_(std::move(record)) {
    adapter_ = findOwnAdapter(gpu); // An adapter object of its own: cannot hold up the others.
    gpu_ = createGpuDevice(adapter_.Get());
    reader_ = std::make_unique<ImageReader>(gpu_.device.Get(), channel);
    thread_ = std::jthread([this](std::stop_token stop) { run(stop); });
}

DepthFinder::~DepthFinder() {
    if (thread_.joinable()) {
        thread_.request_stop();
        thread_.join();
    }
}

void DepthFinder::hint(const UiPieces* ui) {
    UiPieces& slot = hints_.writeSlot();
    if (ui != nullptr) {
        slot = *ui;
    } else {
        slot.count = 0;
    }
    hints_.publish();
}

const DepthSeen& DepthFinder::seen() {
    seen_.fetch();
    return seen_.readSlot();
}

const DepthFinderStats& DepthFinder::stats() {
    stats_.fetch();
    return stats_.readSlot();
}

void DepthFinder::run(std::stop_token stop) {
    // Below the capture, the recogniser and the snapper: depth may come a frame late.
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    SetThreadDescription(GetCurrentThread(), L"observex depth finder");

    std::unique_ptr<DepthNet> net;
    std::unique_ptr<Depthing> depthing;
    std::unique_ptr<DepthPacker> packer;
    std::uint64_t heuristicsVersion = heuristics_->version();
    try {
        net = std::make_unique<DepthNet>(gpu_.device.Get(), adapter_.Get(), modelFolder_, stop, progress_);
        depthing = heuristics_->makeDepth();
        if (record_) {
            packer = std::make_unique<DepthPacker>();
        }
    } catch (const std::exception& e) {
        fail(e.what());
        return;
    }
    const int width = net->width();
    const int height = net->height();
    const std::size_t pixels = static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
    std::vector<std::uint8_t> levels(pixels);
    std::vector<std::uint8_t> before; // The levels of the map before, for DepthFinderStats::changed.
    std::array<std::uint32_t, Depthing::kPaletteSize> palette{};
    std::vector<GreyArea> ignore;
    ignore.reserve(UiPieces::kMax);

    LARGE_INTEGER frequency;
    QueryPerformanceFrequency(&frequency);
    const double perTick = 1000.0 / static_cast<double>(frequency.QuadPart);
    StatWindow networking;
    StatWindow shading;
    StatWindow packing;
    double packedKb = 0.0;
    double changed = 0.0;
    int compared = 0;
    std::int64_t windowStart = qpcNow();
    state_.store(State::Running, std::memory_order_release);

    ImageStamp stamp;
    while (!stop.stop_requested()) {
        // Once a second, the numbers to the UI (lock-free).
        if (const std::int64_t now = qpcNow(), elapsed = now - windowStart; elapsed >= frequency.QuadPart) {
            const StatWindow::Summary network = networking.take();
            const StatWindow::Summary shade = shading.take();
            const StatWindow::Summary pack = packing.take();
            DepthFinderStats& stats = stats_.writeSlot();
            stats.mapsPerSecond = static_cast<float>(network.count / (static_cast<double>(elapsed) * perTick / 1000.0));
            stats.networkAvgMs = network.average;
            stats.networkMaxMs = network.max;
            stats.shadeAvgMs = shade.average;
            stats.packAvgMs = pack.average;
            stats.packedKb = pack.count > 0 ? static_cast<float>(packedKb / pack.count) : 0.0f;
            stats.changed = compared > 0 ? static_cast<float>(changed / compared) : 0.0f;
            stats_.publish();
            packedKb = 0.0;
            changed = 0.0;
            compared = 0;
            windowStart = now;
        }
        // Each image the pipeline hands over (kImagesPerSecond).
        if (!reader_->waitForImage(kWaitMs)) {
            continue;
        }
        const std::int64_t start = qpcNow();
        ID3D11Texture2D* image = reader_->useNewImage(&stamp);
        if (image == nullptr) {
            continue;
        }
        // New shading once the app loaded a rebuilt DLL (if it cannot be made, the one it has goes on).
        if (const std::uint64_t version = heuristics_->version(); version != heuristicsVersion) {
            heuristicsVersion = version;
            try {
                depthing = heuristics_->makeDepth();
            } catch (const std::exception&) {
            }
        }
        net->squeeze(gpu_.context.Get(), image);
        reader_->endUse();
        const std::uint16_t* out = net->run();
        if (out == nullptr) {
            fail("The GPU stopped running the depth network (it may have been reset)");
            return;
        }
        auto made = std::make_shared<DepthMade>();
        made->map.resize(pixels);
        std::memcpy(made->map.data(), out, pixels * sizeof(std::uint16_t));
        const std::int64_t ran = qpcNow();
        networking.add(static_cast<float>(static_cast<double>(ran - start) * perTick));

        // Handed over at once with the interface the UI finder found (left out of the ground's depth and the shading).
        hints_.fetch();
        const UiPieces& ui = hints_.readSlot();
        ignore.clear();
        for (int i = 0; i < ui.count; ++i) {
            const UiPiece& piece = ui.pieces[static_cast<std::size_t>(i)];
            ignore.push_back({piece.x0, piece.y0, piece.x1, piece.y1});
        }
        made->image = stamp.number;
        made->imageQpc = stamp.shownQpc != 0 ? stamp.shownQpc : start;
        made->width = width;
        made->height = height;
        made->screenWidth = static_cast<int>(width_);
        made->screenHeight = static_cast<int>(height_);
        made->ignore = ignore;
        const DepthInput input = made->input();
        made_.store(made, std::memory_order_release);

        // Shaded only while the flag draws it; else the clip buffer keeps the map with no levels shown.
        if (shown_.load(std::memory_order_relaxed)) {
            if (!depthing->shade(input, levels.data(), palette.data())) {
                continue; // Nothing to show of this one.
            }
            DepthSeen& seen = seen_.writeSlot();
            seen.image = stamp.number;
            seen.imageQpc = stamp.shownQpc;
            seen.width = width;
            seen.height = height;
            seen.pixels.resize(pixels);
            paintDepth(levels.data(), palette.data(), pixels, seen.pixels.data());
            seen_.publish();
            shading.add(static_cast<float>(static_cast<double>(qpcNow() - ran) * perTick));
            if (before.size() == pixels) {
                std::size_t moved = 0;
                for (std::size_t i = 0; i < pixels; ++i) {
                    moved +=
                        std::abs(static_cast<int>(levels[i]) - static_cast<int>(before[i])) > kChangedLevels ? 1 : 0;
                }
                changed += static_cast<double>(moved) / static_cast<double>(pixels);
                ++compared;
            }
            before = levels;
        } else {
            std::fill(levels.begin(), levels.end(), std::uint8_t{0});
            palette.fill(0);
            before.clear();
        }
        const std::int64_t shaded = qpcNow();

        // For the clip buffer: the map, what was shown of it, and the interface left out.
        if (record_) {
            auto record = std::make_shared<DepthRecord>();
            record->image = made->image;
            record->imageQpc = made->imageQpc;
            record->width = width;
            record->height = height;
            record->screenWidth = static_cast<int>(width_);
            record->screenHeight = static_cast<int>(height_);
            record->ignore = ignore;
            record->palette = palette;
            if (packer->pack(made->map.data(), levels.data(), *record)) {
                packedKb += static_cast<double>(record->map.size() + record->levels.size()) / 1024.0;
                record_(std::move(record));
            }
            packing.add(static_cast<float>(static_cast<double>(qpcNow() - shaded) * perTick));
        }
    }
}

void DepthFinder::fail(std::string message) {
    error_ = std::move(message);
    state_.store(State::Failed, std::memory_order_release);
}

} // namespace observex
