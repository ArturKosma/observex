#pragma once

#include "core/Heuristics.h"
#include "core/TripleBuffer.h"
#include "engine/Gpu.h"
#include "engine/HeuristicsLibrary.h"
#include "engine/ImageChannel.h"
#include "engine/Recognizer.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <span>
#include <stop_token>
#include <string>
#include <thread>

namespace observex {

class ClipRecorder;

// How the snapper is doing, refreshed once per second.
struct SnapperStats {
    float imagesPerSecond = 0.0f; // Images it put the boxes onto.
    float workAvgMs = 0.0f;       // From taking an image to having its boxes: the grey image on the GPU, the parts
    float workMaxMs = 0.0f;       // snapping reads waited for, then snapping.
    float lateAvgMs = 0.0f;       // From the screen showing an image to having its boxes.
    float snapped = 0.0f;         // Of the boxes it handed out, the share snapped (the rest where their speed takes
                                  // them: not found, or nothing learned of them yet).
};

// Puts the boxes the recogniser shows onto every new screen image, the moment it comes (the user asked, 2026-11-11:
// boxes snapped to the characters, as the bot will click their middles): a look takes about 10 ms and the boxes it
// makes come that late, while the screen shows a new image every few milliseconds, and in a fast camera turn the
// things have moved 30-50 pixels by then. Each image goes grey on the GPU (engine/GreyMaker), only the parts the
// snapping reads come back, then today's snapping (core/Heuristics' Snapping, heuristics/SnapRules, from the
// heuristics DLL, made again whenever it is rebuilt) finds each thing on it by how it looked where the model last
// found it. Bench, 2026-11-11, with parts as large as on a fight clip's (350 KB an image): 0.9 ms an image, the boxes
// there 1.1 ms after the screen showed it, on every image (240 a second).
//
// It works on its own thread with its own GPU device, so it never holds up capture: the pipeline hands it every new
// image through an ImageChannel (Pipeline::setSnapEnabled), and the recogniser hands it each look
// (Recognizer::setSnapper). Its GPU work slows the recogniser's looks a little (3.2 -> 3.9 ms; reading whole images
// back made it 5.1). Talks to the UI only through lock-free hand-overs and atomics.
class BoxSnapper {
public:
    enum class State : std::uint8_t {
        Starting,
        Running,
        Failed, // See error().
    };

    // Starts on the images of `channel`, which come from `screen`, with snapping from `heuristics`. Returns at once.
    // Throws on failure.
    BoxSnapper(const Screen& screen, ImageChannel& channel, std::shared_ptr<const HeuristicsSource> heuristics);
    // Stops. Destroy it before the channel.
    ~BoxSnapper();

    BoxSnapper(const BoxSnapper&) = delete;
    BoxSnapper& operator=(const BoxSnapper&) = delete;

    State state() const { return state_.load(std::memory_order_acquire); }
    const std::string& error() const { return error_; } // Valid once state() is Failed.

    // For the recogniser's thread, after each look: the image it looked at and what it showed after it (the
    // heuristics' things). Never waits and allocates nothing; a look the snapper has not taken yet when the next comes
    // is skipped.
    void looked(const ImageStamp& stamp, std::span<const Tracked> shown);

    // For one reader thread (the UI): the boxes on the newest image snapped onto (Found::image counts those images;
    // 0 before the first). Valid until the next call.
    const Found& found();
    const SnapperStats& stats();

    // While set, every image's boxes also go into `clip` (the clip buffer). Any thread; null stops it.
    void setClip(std::shared_ptr<ClipRecorder> clip) { clip_.store(std::move(clip), std::memory_order_release); }

private:
    struct Look {
        ImageStamp stamp;
        int count = 0;
        std::array<Tracked, Found::kMax> shown{};
    };

    void run(std::stop_token stop);
    void fail(std::string message);

    GpuDevice gpu_;
    std::unique_ptr<ImageReader> reader_;
    std::shared_ptr<const HeuristicsSource> heuristics_;
    unsigned width_ = 0;
    unsigned height_ = 0;
    TripleBuffer<Look> looks_;
    TripleBuffer<Found> found_;
    TripleBuffer<SnapperStats> stats_;
    std::atomic<std::shared_ptr<ClipRecorder>> clip_;
    std::atomic<State> state_{State::Starting};
    std::string error_;
    std::jthread thread_; // Last, so it stops before the members it uses are destroyed.
};

} // namespace observex
