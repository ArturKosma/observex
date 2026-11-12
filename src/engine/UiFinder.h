#pragma once

#include "core/TripleBuffer.h"
#include "core/UiLayout.h"
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

namespace observex {

// How the UI finder is doing, refreshed once per second.
struct UiFinderStats {
    float imagesPerSecond = 0.0f;    // Images it found the interface on.
    float workAvgMs = 0.0f;          // From taking an image to having its pieces: the grey image on the GPU, read
    float workMaxMs = 0.0f;          // back, then finding.
    float findAvgMs = 0.0f;          // Of that, the finding (core/UiLayout).
    std::array<char, 256> problem{}; // Why ui.txt is not what it finds with (see UiFinder); empty when it is.
};

// Finds where the game draws its interface on the screen (core/UiLayout: the framed windows and ui.txt's pieces), all
// the time while the app observes a screen, and on the frames of an open clip (app/ClipView); the flag UI (Show, and
// the clip's Guesses) only draws what it found (the user asked, 2026-11-12: to check every piece is found, as a depth
// model is to leave them out later, and the bot will read from them). The interface moves only when the user moves
// it, so ten images a second are enough (kImagesPerSecond).
// Each goes grey on the GPU (engine/GreyMaker) and only its full-size level comes back (3.7 MB for a 2560 x 1440
// screen); the finding takes 3-5 ms on the CPU (bench --ui, 2026-11-12, eight clips).
// ui.txt and its anchors are read again within a second of changing; while they cannot be read, the one before goes
// on (with none, only framed windows are found) and stats().problem says why.
//
// It works on its own thread with its own GPU device, so it never holds up capture, the recogniser or the snapper:
// the pipeline (while Pipeline::setUiEnabled is on) or the clip screen hands it images through an ImageChannel. Talks
// to the UI only through lock-free hand-overs and atomics.
class UiFinder {
public:
    enum class State : std::uint8_t {
        Starting,
        Running,
        Failed, // See error().
    };

    // How many images a second it needs: the most its feeder hands it.
    static constexpr unsigned kImagesPerSecond = 10;

    // Starts on the images of `channel`, with a device of its own on `gpu` (the channel's), with the layout in `folder`
    // (ui.txt and its anchors' pictures, e.g. models/ui). Returns at once. Throws on failure.
    UiFinder(IDXGIAdapter1* gpu, ImageChannel& channel, std::filesystem::path folder);
    // Stops. Destroy it before the channel.
    ~UiFinder();

    UiFinder(const UiFinder&) = delete;
    UiFinder& operator=(const UiFinder&) = delete;

    State state() const { return state_.load(std::memory_order_acquire); }
    const std::string& error() const { return error_; } // Valid once state() is Failed.

    // For one reader thread (the UI): the pieces on the newest image it looked at (UiPieces::image 0 before the
    // first). Valid until the next call.
    const UiPieces& found();
    const UiFinderStats& stats();

private:
    void run(std::stop_token stop);
    void fail(std::string message);

    GpuDevice gpu_;
    std::unique_ptr<ImageReader> reader_;
    std::filesystem::path folder_;
    unsigned width_ = 0;
    unsigned height_ = 0;
    TripleBuffer<UiPieces> found_;
    TripleBuffer<UiFinderStats> stats_;
    std::atomic<State> state_{State::Starting};
    std::string error_;
    std::jthread thread_; // Last, so it stops before the members it uses are destroyed.
};

} // namespace observex
