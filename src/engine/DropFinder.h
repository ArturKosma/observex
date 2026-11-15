#pragma once

#include "core/Costs.h"
#include "core/Detection.h"
#include "core/LootWatch.h"
#include "core/TripleBuffer.h"
#include "engine/Gpu.h"
#include "engine/ImageChannel.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <stop_token>
#include <string>
#include <thread>
#include <vector>

namespace observex {

class Detector;

// One look of the drop finder's: what it found on one square of one screen image.
struct DropLook {
    static constexpr int kMax = 16;
    std::array<Detection, kMax> drops{}; // Surest first, boxes in screen image pixels.
    int count = 0;
    LootSquare square{};     // Where it looked.
    std::uint64_t image = 0; // The screen image (ImageStamp::number); 0 before the first look.
    std::int64_t imageQpc = 0;
};

// The drop finder's newest looks of each sort.
struct FoundDrops {
    DropLook close; // On the square lookAt() named (round a kill, or drops found).
    DropLook sweep; // On the sweep's square.
};

// How the drop finder is doing, refreshed once per second.
struct DropFinderStats {
    float looksPerSecond = 0.0f;
    float workAvgMs = 0.0f; // From taking an image to having its drops: the copy, the squeeze and the network.
    float workMaxMs = 0.0f;
    CostPart costs;         // The same work, step by step (the Debug tab).
};

// Finds drops (adena, items) anywhere on the screen (the user asked, 2026-11-15: "teach the model how to recognize
// drops", first "only around the target that we recognized as dead", then "at all times": the camera turns away from
// a kill, the character runs off and comes back, and drops seen while seeking monsters are worth picking up). The drop
// model (models/drop, trained by tools/train/train.py --category drop on square cuts, tools/train/loot.py) looks at one
// full-resolution square of each image, scaled to the model's size (drops are a few pixels across on the whole screen
// squeezed to the character model's size):
// - with no square named (lookAt), the sweep: the next of core/LootWatch's lootTiles, the whole screen in turn;
// - with one (round a kill, or round drops found: core/LootWatch), that square and the sweep's next in turn.
//
// It works on its own thread with its own GPU device, so it never holds up capture or the recogniser: the pipeline
// (while Pipeline::setDropsEnabled is on) hands it kImagesPerSecond images through an ImageChannel. Talks to the UI
// only through lock-free hand-overs and atomics.
class DropFinder {
public:
    enum class State : std::uint8_t {
        Starting, // Loading the model; the first time after a new model, preparing it (progress()).
        Running,
        Failed, // See error().
    };

    static constexpr unsigned kImagesPerSecond = 20;
    // Drops it is less sure of are not handed on.
    static constexpr float kMinSure = 0.5f;

    // Starts on the images of `channel`, with a device of its own on `gpu` (the channel's), the model in `modelFolder`
    // (models/drop). Returns at once: the model loads on its own thread. Throws on failure.
    DropFinder(IDXGIAdapter1* gpu, ImageChannel& channel, std::filesystem::path modelFolder);
    // Stops, also while the model is being prepared. Destroy it before the channel.
    ~DropFinder();

    DropFinder(const DropFinder&) = delete;
    DropFinder& operator=(const DropFinder&) = delete;

    State state() const { return state_.load(std::memory_order_acquire); }
    const std::string& error() const { return error_; } // Valid once state() is Failed.
    float progress() const { return progress_.load(std::memory_order_relaxed); } // Preparing the model: 0 to 1.

    // For any thread: where to look closely on the next images, every other one (empty: only the sweep).
    void lookAt(LootSquare square);

    // For one reader thread (the UI). Valid until the next call.
    const FoundDrops& found();
    const DropFinderStats& stats();

private:
    void run(std::stop_token stop);
    void fail(std::string message);

    ComPtr<IDXGIAdapter1> adapter_; // Its own adapter object, on the channel's GPU.
    GpuDevice gpu_;
    std::unique_ptr<ImageReader> reader_;
    std::filesystem::path modelFolder_;
    unsigned width_ = 0; // The screen's.
    unsigned height_ = 0;
    std::atomic<std::uint64_t> square_{0}; // lookAt's, packed: x, y and side, 16 bits each.
    std::vector<LootSquare> sweep_;        // The sweep's squares, in turn.
    TripleBuffer<FoundDrops> found_;
    TripleBuffer<DropFinderStats> stats_;
    std::atomic<float> progress_{0.0f};
    std::atomic<State> state_{State::Starting};
    std::string error_;
    std::jthread thread_; // Last, so it stops before the members it uses are destroyed.
};

} // namespace observex
