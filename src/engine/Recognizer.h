#pragma once

#include "core/Question.h"
#include "core/Tracker.h"
#include "core/TripleBuffer.h"
#include "engine/Detector.h"
#include "engine/Gpu.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <stop_token>
#include <string>
#include <thread>
#include <vector>

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
    // Asked whether an image is worth recording (recording "only unsure", Pipeline::setRecordOnlyQuestions), it
    // looks at it once more mirrored left to right and answers yes when it sees it at least this differently
    // (core/Mirror). At first 0.17, the eighth model's median on its 189 held-out images (2026-11-02), so about half
    // the asked images got recorded; those held 71% of its mistakes there (random half: 52%, the half with the most
    // things 30 to 65% sure: 68%) and 60% of its false alarms 65% sure or more (unsure things: 43%). With kinds it also
    // answers yes when either look is unsure which kind something is (core/Detection's kKindsClose), and the mirror
    // test compares the same kind in both looks. The ninth model, the first with kinds, at 768 (2026-11-03): with
    // 0.17 and kKindsClose it saved 134 of its 211 held-out images (64%), holding 82% of its mistakes at 50% sure and
    // 24 of its 26 wrong kinds. Run at 1024 (the same day) it sees the mirrored image more alike (median 0.197), so
    // 0.17 saved only 118 (56%; 72%, 16 of 20); 0.11 saves 138 (65%), holding 81% of its mistakes and 19 of its 20
    // wrong kinds, as many as before (the user asked for more unsure images, not fewer).
    static constexpr float kMirrorDisagrees = 0.11f;

    // Starts on the images of `channel`, which come from `screen`, with the model in `modelFolder`
    // (models/<category>). Returns at once: the model loads on the recogniser's own thread. Throws on failure.
    // `question`, if set, is answered after the next look once asked (see kMirrorDisagrees; the app passes
    // Pipeline::question()); it must outlive the recogniser.
    Recognizer(const Screen& screen, ImageChannel& channel, std::filesystem::path modelFolder,
               Question* question = nullptr);
    // Stops, also while the model is being prepared. Destroy it before the channel.
    ~Recognizer();

    Recognizer(const Recognizer&) = delete;
    Recognizer& operator=(const Recognizer&) = delete;

    State state() const { return state_.load(std::memory_order_acquire); }
    // While Starting: how far preparing the model is, 0 to 1; stays 0 when a prepared one is just loaded.
    float preparing() const { return progress_.load(std::memory_order_relaxed); }
    const std::string& error() const { return error_; } // Valid once state() is Failed.
    // The kinds the model tells apart (Detector::kinds); each Tracked::kind indexes it. Valid once state() is
    // Running; empty for a model of one category.
    const std::vector<std::string>& kinds() const { return kinds_; }

    // For one reader thread (the UI): what is followed after the newest image looked at. Valid until the next call.
    const Found& found();
    const RecognizerStats& stats();

    // While set (recording), every image looked at and everything found on it at least LiveLog::kMinSure sure also
    // goes into `log`. Any thread; null stops it. The log stays alive while the recogniser is writing to it.
    void setLog(std::shared_ptr<LiveLog> log) { log_.store(std::move(log), std::memory_order_release); }

    // Loads the kind update next to the model again before its next look (on its own thread; see Detector), e.g.
    // after a new one was written, or drops it if there is no valid one any more. Any thread.
    void reloadKinds() { reloadKinds_.store(true, std::memory_order_release); }
    // How many answers the kind update it picks kinds with learned from; 0 while it uses none. Any thread.
    std::size_t kindAnswers() const { return kindAnswers_.load(std::memory_order_relaxed); }

private:
    void run(std::stop_token stop);
    void fail(std::string message);

    ComPtr<IDXGIAdapter1> adapter_;
    GpuDevice gpu_;
    std::unique_ptr<ImageReader> reader_;
    std::filesystem::path modelFolder_;
    Question* question_ = nullptr;
    unsigned width_ = 0;
    unsigned height_ = 0;
    TripleBuffer<Found> found_;
    TripleBuffer<RecognizerStats> stats_;
    std::atomic<std::shared_ptr<LiveLog>> log_;
    std::atomic<bool> reloadKinds_{false};
    std::atomic<std::size_t> kindAnswers_{0};
    std::atomic<State> state_{State::Starting};
    std::atomic<float> progress_{0.0f};
    std::string error_;
    std::vector<std::string> kinds_;
    std::jthread thread_; // Last, so it stops before the members it uses are destroyed.
};

} // namespace observex
