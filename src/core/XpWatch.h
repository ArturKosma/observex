#pragma once

#include "core/GameStats.h"

#include <optional>
#include <string>

namespace observex {

// Watches the XP read from the game's status window (core/GameStats: the Status piece's XP gauge, e.g. "26.64%") and
// says when it changed, for the log (the user asked, 2026-11-13: XP got, by the difference between readings):
//   Got 0.12% XP: 26.64% to 26.76%
//   Level up, got 0.32% XP: 99.80% to 0.12%   (it fell by more than half: the next level started)
//   Lost 1.20% XP: 26.64% to 25.44%
// A number counts once it was read on two readings in a row (a misread digit that still agrees with the bar shows on
// one), timed by the first. The first one only starts it, and so does one after kForgetSeconds with none (the
// character select screen, another character).
class XpWatch {
public:
    static constexpr double kForgetSeconds = 30.0;

    struct Change {
        std::string text;
        double seconds = 0.0; // When the new number was first read (the `seconds` read() was given then).
    };

    // The next reading: its XP gauge (null or without a number: not read) at `seconds` (any clock, going on).
    std::optional<Change> read(const GameGauge* xp, double seconds);

private:
    double held_ = -1.0; // The XP taken last, in percent; below 0: none yet.
    double heldSeconds_ = 0.0; // When it was last read.
    double seen_ = -1.0; // Read once, not yet taken; below 0: none.
    double seenSeconds_ = 0.0;
};

} // namespace observex
