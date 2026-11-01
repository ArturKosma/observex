#pragma once

#include "core/Tracker.h"
#include "core/TripleBuffer.h"
#include "engine/Detector.h"
#include "engine/Gpu.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <stop_token>
#include <string>
#include <thread>

namespace observex {

class ImageChannel;
class ImageReader;
class LiveLog;

// What the recogniser is following on the screen after one image (see Tracker), surest first.
struct Found {
    static constexpr int kMax = 100;
    std::array<Tracked, kMax> things{};
    int count = 0;
    std::uint64_t image = 0; // Counts the images looked at; 0 before the first.
};

// How the recogniser is doing, refreshed once per second.
struct RecognizerStats {
    float imagesPerSecond = 0.0f; // Images looked at.
    float workAvgMs = 0.0f;       // From taking an image to having what is on it.
    float workMaxMs = 0.0f;
};

// Finds one category of things (e.g. characters) on the observed screen with a trained model (see Detector), on every
// new image, as fast as the GPU allows, and follows them from image to image (see Tracker), so each keeps a steady
// box and an identity. It works on its own thread with its own GPU device, so it never holds up capture: the
// pipeline hands it the newest image through an ImageChannel the moment it arrives, and images that arrive while it
// is busy replace each other. Talks to the UI only through lock-free hand-overs and atomics.
class Recognizer {
public:
    enum class State : std::uint8_t {
        Starting, // Loading TensorRT and the model; the first time after a new model, preparing it (preparing()).
        Running,
        Failed, // See error().
    };

    // Things the model is less sure of than this are not followed (the live log goes lower, see setLog).
    static constexpr float kMinSure = 0.3f;

    // Starts on the images of `channel`, which come from `screen`, with the model in `modelFolder`
    // (models/<category>). Returns at once: the model loads on the recogniser's own thread. Throws on failure.
    Recognizer(const Screen& screen, ImageChannel& channel, std::filesystem::path modelFolder);
    // Stops, also while the model is being prepared. Destroy it before the channel.
    ~Recognizer();

    Recognizer(const Recognizer&) = delete;
    Recognizer& operator=(const Recognizer&) = delete;

    State state() const { return state_.load(std::memory_order_acquire); }
    // While Starting: how far preparing the model is, 0 to 1; stays 0 when a prepared one is just loaded.
    float preparing() const { return progress_.load(std::memory_order_relaxed); }
    const std::string& error() const { return error_; } // Valid once state() is Failed.

    // For one reader thread (the UI): what is followed after the newest image looked at. Valid until the next call.
    const Found& found();
    const RecognizerStats& stats();

    // While set (recording), every image looked at and everything found on it at least LiveLog::kMinSure sure also
    // goes into `log`. Any thread; null stops it. The log stays alive while the recogniser is writing to it.
    void setLog(std::shared_ptr<LiveLog> log) { log_.store(std::move(log), std::memory_order_release); }

private:
    void run(std::stop_token stop);
    void fail(std::string message);

    ComPtr<IDXGIAdapter1> adapter_;
    GpuDevice gpu_;
    std::unique_ptr<ImageReader> reader_;
    std::filesystem::path modelFolder_;
    unsigned width_ = 0;
    unsigned height_ = 0;
    TripleBuffer<Found> found_;
    TripleBuffer<RecognizerStats> stats_;
    std::atomic<std::shared_ptr<LiveLog>> log_;
    std::atomic<State> state_{State::Starting};
    std::atomic<float> progress_{0.0f};
    std::string error_;
    std::jthread thread_; // Last, so it stops before the members it uses are destroyed.
};

} // namespace observex
