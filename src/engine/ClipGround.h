#pragma once

#include "core/ClipGuesses.h"
#include "core/Ground.h"
#include "core/UiLayout.h"
#include "engine/Gpu.h"

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

// The game's camera over a saved clip, followed as the ground finder follows it live (engine/GroundFinder; the user
// asked, 2026-11-12: the Ground flag on clips too): the same following (core/Ground's Grounding, today's
// observex_heuristics.dll's) on grey images made from the clip's video
// on the GPU (engine/GreyMaker), the frames of the images live followed (clip.txt's ground lines; on a clip saved
// before them GroundFinder::kImagesPerSecond of its frames), on the frames between only the minimap's cone (the
// camera turned with it, Grounding::head, as live), the video decoded in order once
// (engine/ClipPlayer's ClipReader). The own character's box and the characters to leave out come from a guesser's
// sets (as live, from what the app shows), the interface from core/UiLayout on a frame a second. It works in the
// background, on its own thread and GPU device, and is stopped by going.
class ClipGround {
public:
    enum class State : std::uint8_t {
        Working,
        Done,   // cameras() has them.
        Failed, // error() says why.
    };

    // Starts following the camera with `ground` on the clip's video `video`, decoded on `gpu`, with `shown`'s sets
    // (a guesser's over `clip`) and the interface's layout in `uiFolder` (models/ui; only framed windows if it cannot
    // be read). Returns at once.
    ClipGround(IDXGIAdapter1* gpu, std::unique_ptr<Grounding> ground, std::filesystem::path video,
               std::shared_ptr<const ClipText> clip, Guesses shown, std::filesystem::path uiFolder);
    // Stops it (within an image) and waits.
    ~ClipGround();

    ClipGround(const ClipGround&) = delete;
    ClipGround& operator=(const ClipGround&) = delete;

    State state() const { return state_.load(std::memory_order_acquire); }
    float progress() const { return progress_.load(std::memory_order_relaxed); } // 0 to 1.
    const std::string& error() const { return error_; } // Valid once state() is Failed.
    // The camera at each frame followed (its timeMs: clip.txt's), in order. Valid once state() is Done.
    const std::vector<GroundCamera>& cameras() const { return cameras_; }

    // How a follow went, for the bench.
    struct Stats {
        int images = 0; // Frames followed,
        int fitted = 0; // of them, ones whose ground's motion fitted;
        double matches = 0.0;  // patches found again, on average,
        double agreeing = 0.0; // of them agreeing (on the fitted ones),
        double missPixels = 0.0;
        double workMs = 0.0; // Reading back and fitting, on average (not decoding).
        std::array<int, GroundFit::kMotions> motions{}; // How many frames' motions went each way, by GroundFit::Motion.
        int headed = 0; // Frames the minimap's cone was seen on (heuristics/MinimapCone).
        int heads = 0;  // Frames between, only the cone read on (Grounding::head), of them seen on headsSeen.
        int headsSeen = 0;
        // Each frame followed (with `notes` on): its time, the patches learned on the frame before and found again on
        // it, how far they went (the middle of them, pixels) and how its motion went.
        struct Frame {
            double timeMs = 0.0;
            int learned = 0;
            int found = 0;
            float moved = 0.0f;
            GroundFit::Motion motion = GroundFit::Motion::NoCamera;
            int agreeing = 0;
            float turn = 0.0f;  // GroundMotion's search: the turn it found (radians)
            float match = 0.0f; // and how well the patches matched there.
            float heading = -1.0f; // The minimap's (radians from north), -1 if its cone was not seen,
            float yaw = 0.0f;      // and the camera's yaw, pitch and distance then.
            float pitch = 0.0f;
            float distance = 0.0f;
        };
        std::vector<Frame> frames;
    };

    // The same work with `ground` (cleared first) on the calling thread, which must have COM started (Media
    // Foundation needs it), on `device` (made with video support: createGpuDevice(adapter, true)), e.g. the bench's.
    // `layout` may be null (nothing of the interface left out). Throws if the video cannot be decoded.
    static std::vector<GroundCamera> follow(ID3D11Device* device, Grounding& ground, const std::filesystem::path& video,
                                            const ClipText& clip, const Guesses& shown, UiLayout* layout,
                                            std::stop_token stop = {}, std::atomic<float>* progress = nullptr,
                                            Stats* stats = nullptr, bool notes = false);

private:
    void run(std::stop_token stop, ComPtr<IDXGIAdapter1> gpu);

    std::unique_ptr<Grounding> ground_;
    std::filesystem::path video_;
    std::shared_ptr<const ClipText> clip_;
    Guesses shown_;
    std::filesystem::path uiFolder_;
    std::vector<GroundCamera> cameras_;
    std::string error_;
    std::atomic<float> progress_{0.0f};
    std::atomic<State> state_{State::Working};
    std::jthread thread_; // Last, so it stops before the members it uses are destroyed.
};

} // namespace observex
