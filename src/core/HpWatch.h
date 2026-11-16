#pragma once

#include "core/GameStats.h"

#include <optional>
#include <string>

namespace observex {

// Watches the own HP read from the game's status window (core/GameStats: the Status piece's HP gauge, its number, e.g.
// "199/256") and says when it fell: the state tree's HP Lost event (the user asked, 2026-11-16: leave Rest once HP was
// lost) and a line for the log:
//   HP lost 12: 199/256 to 187/256
// Only the number counts: it is kept only where it agrees with the bar (the stats reader's `doubted`), and a number
// counts once it was read on two readings in a row (a misread digit that still agrees with the bar shows on one; as
// XpWatch), timed by the first. HP that rose (healing, regeneration while sitting) says nothing, and so does a number
// not read (the bar under a window). A fall that comes with a lower most HP (a buff that raised it wearing off) is not
// damage: taken, nothing said. The first number only starts it, and so does one after kForgetSeconds with none (the
// character select screen, another character).
class HpWatch {
public:
    static constexpr double kForgetSeconds = 30.0;

    struct Lost {
        int points = 0; // How much fell.
        int from = 0;   // The HP before,
        int to = 0;     // and now,
        int most = 0;   // of this most.
        std::string text;
        double seconds = 0.0; // When the new number was first read (the `seconds` read() was given then).
    };

    // The next reading: its HP gauge (null, not seen or without a number: not read) at `seconds` (any clock, going on).
    std::optional<Lost> read(const GameGauge* hp, double seconds);

private:
    int held_ = -1; // The HP taken last; below 0: none yet.
    int heldMost_ = 0;
    double heldSeconds_ = 0.0; // When it was last read.
    int seen_ = -1; // Read once, not yet taken; below 0: none.
    int seenMost_ = 0;
    double seenSeconds_ = 0.0;
};

} // namespace observex
