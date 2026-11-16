#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace observex {

// Watches the target's HP bar (the Target window's, core/GameStats) and says when it fell while the own character did
// not hit it: the state tree's Target Hit By Others event (the user asked, 2026-11-16: "an event ... which fires
// whenever monster lost hp, but it wasnt due to our spell", to leave a monster someone nearer started hitting while
// the own character runs to it: no kill steal). Over the saved clips (2026-11-16, 13 falls): each hit of the own Wind
// Strike showed on the very reading its MP was taken, the cast bar going then or up to 0.2 s later; the two hits by
// another came mid-cast, with no MP taken. So:
// - A fall: the bar read at least kMinFall lower on two readings in a row (within kSame of each other: a misread on
//   one says nothing), the same target (GameStats::target) all along, timed by the first. A bar that rose
//   (regeneration), one not read, and a target's first reading say nothing.
// - The own: MP points fell (a skill's MP taken, at its cast's start or its end), or the cast bar went (shown on the
//   reading before, gone now), from kOurs before the fall to kOurs after it.
// - So a fall is said kOurs after it showed (on the 3rd reading after, at 10 a second) when nothing of the own came
//   that near it, with a line for the trace:
//     Target hit by others: Goblin 100% to 68%
//   Falls within kOurs of one another are one, from the first to the last.
// What it cannot tell: the own skills' damage over time, the own auto attack or pet (said as others'), and another's
// hit that lands within kOurs of the own MP taken (said as the own).
class TargetHitWatch {
public:
    static constexpr float kMinFall = 0.01f;
    static constexpr float kSame = 0.004f;
    static constexpr double kOurs = 0.25;

    struct Reading {
        double seconds = 0.0;    // When the screen showed it (any clock, going on).
        bool hasTarget = false;  // GameStats::hasTarget.
        std::string_view target; // GameStats::target: its name (empty: none).
        float targetHp = -1.0f;  // The Target window's HP bar, 0 to 1; below 0 if not read.
        int mp = -1;             // The own MP in points (the Status MP number); below 0 if not read.
        bool casting = false;    // The cast bar shows.
    };
    struct Hit {
        float from = 0.0f; // The bar before the fall,
        float to = 0.0f;   // and after it.
        double seconds = 0.0; // When it first showed lower.
        std::string text;
    };

    // The next reading; a hit by others when one is said now.
    std::optional<Hit> read(const Reading& reading);

private:
    std::string target_;   // The target watched.
    float held_ = -1.0f;   // Its bar as taken last; below 0: none yet.
    float seen_ = -1.0f;   // Read once, not yet taken; below 0: none.
    double seenSeconds_ = 0.0;
    bool falling_ = false; // A fall not said yet:
    float fallFrom_ = 0.0f;
    float fallTo_ = 0.0f;
    double fallFirst_ = 0.0;
    double fallLast_ = 0.0;
    int mp_ = -1; // The own MP last read.
    bool casting_ = false;
    bool ours_ = false; // The own MP taken or a cast ended, last at oursSeconds_.
    double oursSeconds_ = 0.0;
};

} // namespace observex
