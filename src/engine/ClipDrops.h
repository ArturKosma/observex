#pragma once

#include "core/Detection.h"
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

namespace observex {

// The drops on one frame of a clip, as today's drop model finds them (ClipDrops).
struct ClipFrameDrops {
    static constexpr int kMax = 64;
    std::uint64_t frame = 0; // The frame (ImageStamp::number as the clip screen hands it); 0 before the first.
    int count = 0;
    std::array<Detection, kMax> drops{}; // Surest first, boxes in the frame's pixels.
};

// Today's drops on a clip (the user asked, 2026-11-15: the Drops flag on the clip screen too): the drop model
// (models/drop, as engine/DropFinder uses it live) on each frame the clip screen hands it, looking at the whole frame
// square by square as the review's finder looks at a screenshot (core/LootWatch's lootTiles, the same drop found in two
// squares kept once: keepApart), not live's one square an image, so a frame shows every drop the model finds on it.
// About 31 squares a frame on 2560 x 1440. It works on its own thread with its own GPU device, at below-normal
// priority, on the newest frame of its ImageChannel (one written while it looks replaces the one before).
class ClipDrops {
public:
    enum class State : std::uint8_t {
        Starting, // Loading the model; the first time after a new model, preparing it (progress()).
        Running,
        Failed, // See error().
    };

    // Starts on the frames of `channel`, with a device of its own on `gpu` (the channel's), the model in `modelFolder`.
    // Returns at once: the model loads on its own thread. Throws on failure.
    ClipDrops(IDXGIAdapter1* gpu, ImageChannel& channel, std::filesystem::path modelFolder);
    // Stops, also while the model is being prepared. Destroy it before the channel.
    ~ClipDrops();

    ClipDrops(const ClipDrops&) = delete;
    ClipDrops& operator=(const ClipDrops&) = delete;

    State state() const { return state_.load(std::memory_order_acquire); }
    const std::string& error() const { return error_; } // Valid once state() is Failed.
    float progress() const { return progress_.load(std::memory_order_relaxed); } // Preparing the model: 0 to 1.

    // For one reader thread (the UI): the newest frame looked at. Valid until the next call.
    const ClipFrameDrops& found();

private:
    void run(std::stop_token stop);
    void fail(std::string message);

    ComPtr<IDXGIAdapter1> adapter_; // Its own adapter object, on the channel's GPU.
    GpuDevice gpu_;
    std::unique_ptr<ImageReader> reader_;
    std::filesystem::path modelFolder_;
    unsigned width_ = 0;
    unsigned height_ = 0;
    TripleBuffer<ClipFrameDrops> found_;
    std::atomic<float> progress_{0.0f};
    std::atomic<State> state_{State::Starting};
    std::string error_;
    std::jthread thread_; // Last, so it stops before the members it uses are destroyed.
};

} // namespace observex
