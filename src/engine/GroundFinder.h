#pragma once

#include "core/Detection.h"
#include "core/Ground.h"
#include "core/Heuristics.h"
#include "core/TripleBuffer.h"
#include "core/UiLayout.h"
#include "engine/Gpu.h"
#include "engine/HeuristicsLibrary.h"
#include "engine/ImageChannel.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <span>
#include <stop_token>
#include <string>
#include <thread>

namespace observex {

// How the ground finder is doing, refreshed once per second.
struct GroundFinderStats {
    float imagesPerSecond = 0.0f; // Images it followed the ground on.
    float workAvgMs = 0.0f;       // From taking an image to having the camera: the grey image on the GPU, its
    float workMaxMs = 0.0f;       // patches read back, then matching and fitting,
    float fitAvgMs = 0.0f;        // of which the matching and fitting (on the CPU).
    float fitted = 0.0f;          // Of the images, the share whose ground's motion fitted,
    float searched = 0.0f;        // and whose camera followed the search instead (GroundFit::Motion::Searched).
    float matches = 0.0f;         // Patches found again on an image, on average,
    float agreeing = 0.0f;        // of them agreeing with the camera fitted,
    float missPixels = 0.0f;      // how far off those were.
    float headed = 0.0f;          // Of the images, the share the minimap's cone was seen on (heuristics/MinimapCone).
    float headsPerSecond = 0.0f;  // Images between them it only read the cone on (Grounding::head),
    float headAvgMs = 0.0f;       // taking this long each.
};

// The camera as the ground finder knows it on the newest image it took.
struct GroundSeen {
    GroundCamera camera; // Its timeMs: the image's (ImageStamp::shownQpc in milliseconds, groundMs).
    std::uint64_t image = 0;    // ImageStamp::number; 0 before the first.
    std::uint64_t followed = 0; // The newest followed whole (the camera on images since only turned with the minimap).
};

// The screen's QPC time `qpc` in the milliseconds GroundSeen's camera counts.
double groundMs(std::int64_t qpc);

// Follows the game's camera on the observed screen (core/Ground, heuristics/GroundMotion: the ground's motion between
// images and the own character's box), for the Show flag Ground (the user asked, 2026-11-12: a ground plane that
// follows the camera, the first step of the depth plan). kImagesPerSecond: a camera turn moves the ground 20-60
// pixels between them, within the patches' reach; more would only cost more. Each goes grey on the GPU
// (engine/GreyMaker), and only its quarter and eighth sizes and the minimap's cone (60 x 60 full-size pixels,
// heuristics/MinimapCone) come back. On every image between them only the cone comes back, and the camera turns with it
// (Grounding::head): at 15 a second alone the grid stood still for the first 75 ms of a turn and went on turning 75 ms
// after it stopped (clip 2026-11-12_18-58-01, frames 561 to 604). The following itself (core/Ground's Grounding) is
// today's observex_heuristics.dll's, made again whenever the app loads a rebuilt one (HeuristicsSource::version), so a
// change to it shows without closing the app.
//
// It works on its own thread with its own GPU device, so it never holds up capture, the recogniser or the snapper:
// the pipeline (while Pipeline::setGroundEnabled is on) hands it images through an ImageChannel, and the UI what it
// shows (hint()). Talks to the UI only through lock-free hand-overs and atomics.
class GroundFinder {
public:
    enum class State : std::uint8_t {
        Starting,
        Running,
        Failed, // See error().
    };

    static constexpr unsigned kImagesPerSecond = 15;
    // A hint older than this says nothing of the own character any more (the UI stopped giving them).
    static constexpr double kHintMs = 500.0;

    // Starts on the images of `channel`, with a device of its own on `gpu` (the channel's), following with
    // `heuristics`' (polled by the app). Returns at once. Throws on failure.
    GroundFinder(IDXGIAdapter1* gpu, ImageChannel& channel, std::shared_ptr<const HeuristicsSource> heuristics);
    // Stops. Destroy it before the channel.
    ~GroundFinder();

    GroundFinder(const GroundFinder&) = delete;
    GroundFinder& operator=(const GroundFinder&) = delete;

    State state() const { return state_.load(std::memory_order_acquire); }
    const std::string& error() const { return error_; } // Valid once state() is Failed.

    // For one writer thread (the UI): what is on the screen now: the things shown (the model's kind `ownKind` being
    // the own character's; -1: none) and the interface's pieces (`ui`, null: not known).
    void hint(std::span<const Tracked> things, int ownKind, const UiPieces* ui);
    // Forgets the camera, e.g. when the Show flag goes on again.
    void restart() { restart_.store(true, std::memory_order_relaxed); }

    // For one reader thread (the UI). Valid until the next call.
    const GroundSeen& seen();
    const GroundFinderStats& stats();

private:
    static constexpr int kMaxAvoid = 100 + UiPieces::kMax; // The things shown (Found::kMax) and the pieces.
    struct Hint {
        std::int64_t qpc = 0; // When it was given; 0: never.
        bool hasOwn = false;
        float ownTop = 0.0f;
        float ownBottom = 0.0f;
        int avoidCount = 0;
        std::array<GreyArea, kMaxAvoid> avoid{};
        int windowCount = 0; // The interface's pieces alone (the minimap among them).
        std::array<GreyArea, UiPieces::kMax> windows{};
    };

    void run(std::stop_token stop);
    void fail(std::string message);

    GpuDevice gpu_;
    std::unique_ptr<ImageReader> reader_;
    std::shared_ptr<const HeuristicsSource> heuristics_;
    unsigned width_ = 0;
    unsigned height_ = 0;
    TripleBuffer<Hint> hints_;
    TripleBuffer<GroundSeen> seen_;
    TripleBuffer<GroundFinderStats> stats_;
    std::atomic<bool> restart_{false};
    std::atomic<State> state_{State::Starting};
    std::string error_;
    std::jthread thread_; // Last, so it stops before the members it uses are destroyed.
};

} // namespace observex
