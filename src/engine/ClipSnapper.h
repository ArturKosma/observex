#pragma once

#include "core/ClipGuesses.h"
#include "core/Heuristics.h"
#include "engine/Gpu.h"

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <stop_token>
#include <string>
#include <thread>

namespace observex {

// Today's boxes on a saved clip snapped as the app's snapper did live (the user asked, 2026-11-11: a new clip's Recorded
// and Today must show exactly the same, so that a heuristics change is all that changes Today): core/ClipGuesses'
// snapGuesses, on grey images made from the clip's video on the GPU (engine/GreyMaker, as engine/BoxSnapper makes them
// from the screen), the video decoded in order once (engine/ClipPlayer's ClipReader). It works in the background, on
// its own thread and GPU device, and is stopped by going; the clip screen shows Today without snapping meanwhile.
class ClipSnapper {
public:
    enum class State : std::uint8_t {
        Working,
        Done,   // guesses() has them.
        Failed, // error() says why.
    };

    // Starts snapping `today` (heuristicGuesses of `clip`) by `snapping` onto the clip's video `video`, decoded on
    // `gpu`. Returns at once.
    ClipSnapper(IDXGIAdapter1* gpu, std::filesystem::path video, std::shared_ptr<const ClipText> clip, Guesses today,
                std::unique_ptr<Snapping> snapping);
    // Stops it (within an image) and waits.
    ~ClipSnapper();

    ClipSnapper(const ClipSnapper&) = delete;
    ClipSnapper& operator=(const ClipSnapper&) = delete;

    State state() const { return state_.load(std::memory_order_acquire); }
    float progress() const { return progress_.load(std::memory_order_relaxed); } // 0 to 1.
    const std::string& error() const { return error_; }  // Valid once state() is Failed.
    const Guesses& guesses() const { return guesses_; } // Valid once state() is Done.

    // The same work on the calling thread, which must have COM started (Media Foundation needs it), on `device` (made
    // with video support: createGpuDevice(adapter, true)), e.g. the bench's. Throws if the video cannot be decoded.
    static Guesses snap(ID3D11Device* device, const std::filesystem::path& video, const ClipText& clip,
                        const Guesses& today, Snapping& snapping, std::stop_token stop = {},
                        std::atomic<float>* progress = nullptr);

private:
    void run(std::stop_token stop, ComPtr<IDXGIAdapter1> gpu);

    std::filesystem::path video_;
    std::shared_ptr<const ClipText> clip_;
    Guesses today_;
    std::unique_ptr<Snapping> snapping_;
    Guesses guesses_;
    std::string error_;
    std::atomic<float> progress_{0.0f};
    std::atomic<State> state_{State::Working};
    std::jthread thread_; // Last, so it stops before the members it uses are destroyed.
};

} // namespace observex
