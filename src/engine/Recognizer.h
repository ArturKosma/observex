#pragma once

#include "core/Costs.h"
#include "core/Heuristics.h"
#include "core/Question.h"
#include "core/TripleBuffer.h"
#include "engine/Detector.h"
#include "engine/Gpu.h"
#include "engine/HeuristicsLibrary.h"

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

class BoxSnapper;
class ClipRecorder;
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
    CostPart costs;               // Its work on each image, step by step (the Debug tab).
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

    // Things the model is less sure of than this are not followed (core/Heuristics; the live log goes lower, see
    // setLog).
    static constexpr float kMinSure = Heuristics::kMinSure;
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
    // What it finds goes through heuristics from `heuristics` (made again between two looks whenever it loads new
    // ones, so a rebuilt observex_heuristics.dll is followed at once; the tracker starts over then).
    // `question`, if set, is answered after the next look once asked (see kMirrorDisagrees; the app passes
    // Pipeline::question()); it must outlive the recogniser.
    Recognizer(const Screen& screen, ImageChannel& channel, std::filesystem::path modelFolder,
               std::shared_ptr<const HeuristicsSource> heuristics, Question* question = nullptr);
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

    // While set, every look also goes into `clip` (the clip buffer: what was seen and followed in the last seconds),
    // and everything found down to LiveLog::kMinSure sure. Any thread; null stops it. It stays alive while the
    // recogniser is handing a look to it.
    void setClip(std::shared_ptr<ClipRecorder> clip) { clip_.store(std::move(clip), std::memory_order_release); }

    // While set, what it shows after every look also goes to `snapper`, which puts the boxes onto every newer image.
    // Any thread; null stops it.
    void setSnapper(std::shared_ptr<BoxSnapper> snapper) {
        snapper_.store(std::move(snapper), std::memory_order_release);
    }

    // Loads the kind update next to the model again before its next look (on its own thread; see Detector), e.g.
    // after a new one was written, or drops it if there is no valid one any more. Any thread.
    void reloadKinds() { reloadKinds_.store(true, std::memory_order_release); }
    // How many answers the kind update it picks kinds with learned from; 0 while it uses none. Any thread.
    std::size_t kindAnswers() const { return kindAnswers_.load(std::memory_order_relaxed); }

    // The thing followed as `id` (Tracked::id) is dead: the game's stats said its target died (the user asked,
    // 2026-11-15: "once we discover the death from stats we can shift the weights of the box towards dead";
    // core/TargetWatch finds which box). From its next look on, while it is followed, it is shown as the model's "dead"
    // kind at least kDeadSure sure, its kind before as the second (so to the snapper, the clip buffer's Recorded
    // things and every reader of found()). Nothing for a model without a "dead" kind. The last kDeadKept ids are kept
    // (an id is never reused, but the tracker can follow another thing under it once two boxes were one: see
    // unmarkDead). One the tracker shows as the "own" kind is never shown dead (the own
    // character marked by mistake hid it from the ground finder: 2026-11-16). Any thread.
    void markDead(std::uint32_t id);
    // `id` is not dead after all (core/TargetWatch read the dead target's plate over another box: the tracker had
    // handed the corpse's id to a live one, 2026-11-16): shown as the model says again from its next look. Any thread.
    void unmarkDead(std::uint32_t id);
    static constexpr float kDeadSure = 0.9f;
    static constexpr std::size_t kDeadKept = 8;

    // Close-up looks while the own character is being hit (the user's clip 2026-11-16_14-04-25: an Imp hitting the own
    // character from right beside it was boxed together with it, as the own character, on a quarter of the looks,
    // found on its own at 60% on only 14% and called dead on a third of those, so it was never taken for the
    // attacker). Until `untilQpc` (QueryPerformanceCounter's), each look also looks at a part of the screen
    // kCloseUpShare of its width and height round the own character (the "own" kind's box on the look before,
    // core/Ground's ownCharacter), squeezed to the model's size as the whole screen is (so 2x as large, the same
    // shape). What it finds inside that part, kCloseUpMargin of its size in from each side, replaces what the look at
    // the whole screen found there; the rest stays the whole screen's. On 24 frames of that clip the live model found
    // the Imp at least 60% sure on 11 (10 as a monster) this way, against 2 on the whole screen; at least 30% sure on
    // 19 against 12. The heuristics, the live log and the clip buffer take the joined look as the look. It costs one
    // more run of the network (about 4 ms) on every look meanwhile. The app calls it each time HP is lost. Any thread.
    void lookCloser(std::int64_t untilQpc) { closeUntil_.store(untilQpc, std::memory_order_relaxed); }
    bool lookingCloser() const; // Whether it still is (any thread).
    static constexpr int kCloseUpShare = 2;          // The part is 1/2 the screen's width and height.
    static constexpr float kCloseUpMargin = 0.125f;  // Of the part's width and height.
    static constexpr double kCloseUpSeconds = 3.0;   // How long the app has it look closer after HP was lost.

private:
    void run(std::stop_token stop);
    void fail(std::string message);
    // markDead's things among `things` (as the heuristics show them) as the model's kind `deadKind`, but not those of
    // the kind `ownKind` (-1: none).
    void markDeadThings(std::span<Tracked> things, int deadKind, int ownKind) const;

    ComPtr<IDXGIAdapter1> adapter_;
    GpuDevice gpu_;
    std::unique_ptr<ImageReader> reader_;
    std::filesystem::path modelFolder_;
    std::shared_ptr<const HeuristicsSource> heuristics_;
    Question* question_ = nullptr;
    unsigned width_ = 0;
    unsigned height_ = 0;
    TripleBuffer<Found> found_;
    TripleBuffer<RecognizerStats> stats_;
    std::atomic<std::shared_ptr<LiveLog>> log_;
    std::atomic<std::shared_ptr<ClipRecorder>> clip_;
    std::atomic<std::shared_ptr<BoxSnapper>> snapper_;
    std::atomic<bool> reloadKinds_{false};
    std::atomic<std::size_t> kindAnswers_{0};
    std::array<std::atomic<std::uint32_t>, kDeadKept> dead_{}; // markDead's ids (0: none), oldest overwritten.
    std::atomic<std::size_t> deadNext_{0};
    std::atomic<std::int64_t> closeUntil_{0}; // lookCloser's.
    std::atomic<State> state_{State::Starting};
    std::atomic<float> progress_{0.0f};
    std::string error_;
    std::vector<std::string> kinds_;
    std::jthread thread_; // Last, so it stops before the members it uses are destroyed.
};

} // namespace observex
