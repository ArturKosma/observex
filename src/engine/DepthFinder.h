#pragma once

#include "core/Depth.h"
#include "core/TripleBuffer.h"
#include "core/UiLayout.h"
#include "engine/DepthStore.h"
#include "engine/Gpu.h"
#include "engine/HeuristicsLibrary.h"
#include "engine/ImageChannel.h"

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <stop_token>
#include <string>
#include <thread>
#include <vector>

namespace observex {

// How the depth finder is doing, refreshed once per second.
struct DepthFinderStats {
    float mapsPerSecond = 0.0f; // Maps made.
    float networkAvgMs = 0.0f;  // From taking an image to having its map back: squeezing it and the network, on the
    float networkMaxMs = 0.0f;  // GPU (waited for),
    float shadeAvgMs = 0.0f;    // shading it (today's heuristics, on the CPU),
    float packAvgMs = 0.0f;     // and packing it for the clip buffer.
    float packedKb = 0.0f;      // A map packed, on average: the network's map and the levels shown.
    float changed = 0.0f;       // Of the pixels, the share whose level moved more than 8 from the map before (flicker).
};

// The newest map shaded, for the UI to show.
struct DepthSeen {
    std::uint64_t image = 0; // The screen image it was made from (ImageStamp::number); 0 before the first.
    std::int64_t imageQpc = 0;
    int width = 0;
    int height = 0;
    std::vector<std::uint32_t> pixels; // BGRA, width x height, row by row (Depthing::shade's levels in its palette).
};

// Guesses how near each pixel of the observed screen is, for the Show flag Depth (the user asked, 2026-11-12: step 2 of
// the depth plan). kImagesPerSecond, each through the depth network (engine/DepthNet), then shaded with today's
// observex_heuristics.dll (core/Depth's Depthing, made again whenever the app loads a rebuilt one, so a change shows
// without closing the app), the interface the UI finder found on the screen (hint()) left out. Each map, packed with
// what was shown of it, goes to `record` (the clip buffer, engine/ClipRecorder::depthed) if given.
//
// The app hands it images while the flag Depth or Ground is on (the user asked, 2026-11-12 late: the grid and the depth
// work only with their flags on): each map goes to made() at once (engine/GroundFinder tilts the camera by it) and to
// the clip buffer; it is shaded (8 ms of the CPU) only while show() says the flag Depth draws it.
//
// It works on its own thread with its own GPU device, so it never holds up capture or the recogniser (its network
// runs on a CUDA stream of the lowest priority): the pipeline (while Pipeline::setDepthEnabled is on) hands it images
// through an ImageChannel. Talks to the UI only through lock-free hand-overs and atomics.
class DepthFinder {
public:
    enum class State : std::uint8_t {
        Starting, // Loading the network, or building its engine the first time (progress()).
        Running,
        Failed, // See error().
    };

    static constexpr unsigned kImagesPerSecond = 20;

    using Record = std::function<void(std::shared_ptr<const DepthRecord>)>;

    // Starts on the images of `channel`, with a device of its own on `gpu` (the channel's), the network in
    // `modelFolder` (models/depth), shading with `heuristics`' (polled by the app). Returns at once. Throws on failure.
    DepthFinder(IDXGIAdapter1* gpu, ImageChannel& channel, std::shared_ptr<const HeuristicsSource> heuristics,
                std::filesystem::path modelFolder, Record record = {});
    // Stops. Destroy it before the channel.
    ~DepthFinder();

    DepthFinder(const DepthFinder&) = delete;
    DepthFinder& operator=(const DepthFinder&) = delete;

    State state() const { return state_.load(std::memory_order_acquire); }
    const std::string& error() const { return error_; } // Valid once state() is Failed.
    float progress() const { return progress_.load(std::memory_order_relaxed); } // Building the engine: 0 to 1.

    // For one writer thread (the UI): the interface's pieces on the screen now (null: not known).
    void hint(const UiPieces* ui);
    // For one writer thread (the UI): whether the Show flag Depth draws the maps (seen()).
    void show(bool shown) { shown_.store(shown, std::memory_order_relaxed); }
    // For any thread: the newest map made (null before the first).
    std::shared_ptr<const DepthMade> made() const { return made_.load(std::memory_order_acquire); }

    // For one reader thread (the UI). Valid until the next call.
    const DepthSeen& seen();
    const DepthFinderStats& stats();

private:
    void run(std::stop_token stop);
    void fail(std::string message);

    ComPtr<IDXGIAdapter1> adapter_; // Its own adapter object, on the channel's GPU.
    GpuDevice gpu_;
    std::unique_ptr<ImageReader> reader_;
    std::shared_ptr<const HeuristicsSource> heuristics_;
    unsigned width_ = 0; // The screen's.
    unsigned height_ = 0;
    std::filesystem::path modelFolder_;
    Record record_;
    TripleBuffer<UiPieces> hints_;
    TripleBuffer<DepthSeen> seen_;
    TripleBuffer<DepthFinderStats> stats_;
    std::atomic<float> progress_{0.0f};
    std::atomic<bool> shown_{false};
    std::atomic<std::shared_ptr<const DepthMade>> made_;
    std::atomic<State> state_{State::Starting};
    std::string error_;
    std::jthread thread_; // Last, so it stops before the members it uses are destroyed.
};

} // namespace observex
