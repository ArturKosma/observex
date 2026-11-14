#pragma once

#include "core/Detection.h"
#include "core/Ground.h"
#include "core/Heuristics.h"
#include "core/Log.h"
#include "core/Recordings.h"
#include "core/TripleBuffer.h"
#include "engine/DepthStore.h"
#include "engine/Gpu.h"
#include "engine/ImageChannel.h"

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace observex {

// How the clip buffer is doing, refreshed once per second.
struct ClipRecorderStats {
    float framesPerSecond = 0.0f; // Frames the video encoder took a second.
    float encodeAvgMs = 0.0f;     // Compressing one frame (the video encoder's work, waited for).
    float encodeMaxMs = 0.0f;
    double seconds = 0.0;      // How much it holds now (up to a little over kClipSeconds).
    std::uint64_t bytes = 0;   // The compressed video it holds.
    std::uint64_t dropped = 0; // Looks and snapped images it had no room for since it started (never waits).
    std::uint64_t depthBytes = 0; // The depth maps it holds, packed (while the Show flag Depth is on).
};

// What a saved clip says about where it came from (clip.observex and the head of clip.txt).
struct ClipAbout {
    std::string saved;              // Local time, "2026-11-10 18:22:05".
    std::string screen;             // The monitor's name.
    std::string model;              // As in ClipInfo::model; empty if the recogniser is not running.
    std::vector<std::string> kinds; // The model's kinds (Recognizer::kinds()), so clip.txt's kind numbers can be read.
};

// The clip buffer: keeps the last kClipSeconds (core/Recordings) of the observed screen in memory, all the time, and
// saves them as a clip when asked (the user asked, 2026-11-10: to compare later what heuristics would do on the same
// seconds). The screen is kept as the GPU's video encoder compresses it (NVENC, H.264, a frame each time the screen
// changed), so keeping it costs the game and the model almost nothing; with it, what the recogniser saw and followed on
// every look (looked()) and notes from the UI (note()). Saving writes, in the background: clip.mp4 (nothing drawn on
// it), clip.txt, picture.jpg, depth.bin (the depth maps shown, if any: engine/DepthStore) and clip.observex (see
// core/Recordings).
//
// It works on its own thread with its own GPU device, reading the pipeline's clip channel (Pipeline::setClipEnabled),
// so it never holds up capture; images that arrive while it is busy replace each other.
class ClipRecorder {
public:
    enum class State : std::uint8_t {
        Starting,
        Running,
        Failed, // See error(); e.g. no NVIDIA video encoder.
    };
    enum class SaveState : std::uint8_t {
        None,   // Nothing saved yet.
        Saving, // In the background.
        Saved,  // savedFolder() and savedInfo() say what.
        Failed, // saveError() says why.
    };

    // Starts on the images of `channel`, which come from `screen`. Returns at once. Throws on failure.
    ClipRecorder(const Screen& screen, ImageChannel& channel);
    // Stops; a save still being written is finished first. Destroy it before the channel.
    ~ClipRecorder();

    ClipRecorder(const ClipRecorder&) = delete;
    ClipRecorder& operator=(const ClipRecorder&) = delete;

    State state() const { return state_.load(std::memory_order_acquire); }
    const std::string& error() const { return error_; } // Valid once state() is Failed.

    // For the recogniser's thread, after each look: the image it looked at, when it had what is on it (`doneQpc`),
    // everything it found (`found`, at least LiveLog::kMinSure sure) and what it follows after it (`followed`, as the
    // UI shows it). Never waits and allocates nothing; a look that finds no room is dropped (stats().dropped).
    void looked(const ImageStamp& stamp, std::int64_t doneQpc, std::span<const Detection> found,
                std::span<const Tracked> followed);

    // For the snapper's thread (engine/BoxSnapper), after each image: the boxes it put onto the image `stamp`, each
    // `leadMs` ahead along its speed, as the UI shows them from `doneQpc` on, and how it got them: the look it took in
    // just before (its image number, 0: none) and its searches (Snapping::finds), so a replay is exact. Never waits
    // and allocates nothing; an image that finds no room is dropped (stats().dropped).
    void snapped(const ImageStamp& stamp, std::int64_t doneQpc, double leadMs, std::span<const Tracked> things,
                 std::uint64_t lookImage, std::span<const SnapFind> finds);

    // A line about this moment for the clip's text, e.g. "recording started" or "mark 2". UI thread.
    void note(std::string_view text);
    // A line of observex's log (core/Log) made at `qpc` (now or a moment before), for the clip's text. UI thread.
    void logged(std::int64_t qpc, const LogLine& line);
    // UI thread, whenever what the Show flag Ground draws changes: the grid `drawn` over screen image `image` (shown at
    // `shownQpc`), from the ground finder's camera on image `followed`; `drawn` not known: nothing drawn from then on.
    // So a clip's Recorded shows the grid exactly as it was seen (core/ClipLines' ground line).
    void grounded(std::int64_t shownQpc, std::uint64_t image, std::uint64_t followed, const GroundCamera& drawn);
    // For the depth finder's thread (engine/DepthFinder), after each map: the map, packed, and how it was shown.
    void depthed(std::shared_ptr<const DepthRecord> record);
    // UI thread, whenever the depth map the Show flag Depth draws changes: map `map` (DepthRecord::image) drawn over
    // screen image `image` (shown at `shownQpc`); 0: nothing drawn from then on. So a clip's Recorded shows the depth
    // exactly as it was seen (core/ClipLines' depth line); the clip keeps only the maps drawn.
    void depthShown(std::int64_t shownQpc, std::uint64_t image, std::uint64_t map);

    // UI thread: saves what it holds (the last kClipSeconds) into `folder` (created), in the background. False if a
    // save is still being written, it holds nothing yet or it is not running (state()).
    bool save(std::filesystem::path folder, ClipAbout about);
    SaveState saveState() const { return saveState_.load(std::memory_order_acquire); }
    const std::string& saveError() const { return saveError_; }             // Valid once saveState() is Failed.
    const std::filesystem::path& savedFolder() const { return savedFolder_; } // Valid once Saving or later.
    const ClipInfo& savedInfo() const { return savedInfo_; }                 // Valid once saveState() is Saved.

    // Newest statistics, for one reader thread (the UI). Valid until the next call.
    const ClipRecorderStats& stats();

private:
    struct Impl;
    void run(std::stop_token stop);
    void fail(std::string message);

    std::unique_ptr<Impl> impl_;
    std::atomic<State> state_{State::Starting};
    std::string error_;
    std::atomic<SaveState> saveState_{SaveState::None};
    std::string saveError_;
    std::filesystem::path savedFolder_;
    ClipInfo savedInfo_;
    TripleBuffer<ClipRecorderStats> stats_;
    std::jthread thread_; // Last, so it stops before the members it uses are destroyed.
};

} // namespace observex
