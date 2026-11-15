#pragma once

#include "core/StatWindow.h"

#include <array>
#include <chrono>
#include <cstdint>
#include <initializer_list>
#include <span>
#include <string_view>

namespace observex {

// What observex spends, part by part (the Debug tab; the user asked, 2026-11-15: "sum up every ms cost of the
// observex", to know what to optimize). A part is one thread's work (capture, the recogniser, the UI finder, the app's
// own frame, ...): its run on each image it takes, and the steps of that run, each timed where it happens and summed up
// once a second. The total the top bar shows right of Show is the parts' runs added up (costTotalMs).

// One timed thing over a second: a part's run on each image it takes, or one step of it.
struct Cost {
    std::array<char, 32> name{}; // "Network"; a part's run has the part's name ("Characters").
    float eachMs = 0.0f;         // One run's average.
    float slowestMs = 0.0f;      // The slowest run.
    float perSecond = 0.0f;      // Runs a second.

    // The share of the second spent on it (each x runs), of its thread's time: 0 to 1.
    float busy() const { return eachMs * perSecond / 1000.0f; }
};

// A part over the last second. Plain (no allocation), so it goes between threads with the part's stats.
struct CostPart {
    static constexpr int kMaxSteps = 14;
    Cost run; // run.name is the part's.
    std::array<Cost, kMaxSteps> steps{};
    int stepCount = 0;
};

// Sets `cost`'s name (cut to fit).
void nameCost(Cost& cost, std::string_view name);

// The top bar's total: each part's run added up.
float costTotalMs(std::span<const CostPart> parts);

// Times one thread's part, with no allocation or locking (only that thread calls it). A run starts at begin(); each
// lap(step) adds the time since the last mark (begin() or the lap before) to that step and marks now; end() adds the
// run's time since begin(). A run begun and never ended (no image after all) is left out of the run, its laps not.
class CostMeter {
public:
    using Clock = std::chrono::steady_clock;

    // `steps`' names in their order (at most CostPart::kMaxSteps); the part is named by whoever shows it.
    explicit CostMeter(std::initializer_list<const char*> steps);

    void begin() { start_ = mark_ = Clock::now(); }
    void lap(int step);
    void end();

    // What was measured since the last take(), `seconds` ago, then it starts over.
    CostPart take(double seconds);

private:
    static float msBetween(Clock::time_point from, Clock::time_point to) {
        return std::chrono::duration<float, std::milli>(to - from).count();
    }

    CostPart names_; // The steps' names, nothing measured.
    StatWindow run_;
    std::array<StatWindow, CostPart::kMaxSteps> steps_{};
    Clock::time_point start_{};
    Clock::time_point mark_{};
};

} // namespace observex
