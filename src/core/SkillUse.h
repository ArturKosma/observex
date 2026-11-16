#pragma once

#include "core/StateTree.h"

#include <cstdint>
#include <span>
#include <string>

namespace observex {

// A skill of a Use Skill task as the app knows it from the profile (statetree::Skill) and its controller.
struct UsableSkill {
    std::string name;
    std::string key;        // Its shortcut, for the log.
    bool pressable = false; // Its shortcut is known and the controller can press it.
    bool active = true;     // An active skill, else a utility one (the Skills tab's columns).
    int mp = 0;             // Its MP cost (the Skills tab); 0: not set.
};

// The state tree's Use Skill task (the user asked, 2026-11-15: "Use Skill should also accept an array, and it checks
// all the skills top to bottom which one is not on cooldown and uses that"; 2026-11-16: "using active skill has a
// definition of done of mana losing", "utility skills DoD should only be Pressed the skill, held for a small amount of
// time and let go"). One step a UI frame, on the newest stats reading:
// 1. Choose: of its skills, top to bottom, the first ready one (statetree::isReady: found on screen, its cooldown 0,
//    not just pressed) whose shortcut can be pressed. None: it waits (said once in the log), choosing again on every
//    reading.
// 2. Press it (the app holds the key a moment, then lets go). A utility skill is done there (the app finishes the task
//    once the key was let go); pressed again, Sit would stand up.
// 3. An active skill: watch the readings of images shown after the press. Done once its MP was taken: MP in points (the
//    Status bar's number) fell by at least kMpShare of its MP Cost from the most it was since the press (regeneration
//    before the cost); with no MP Cost set, by any point at all.
// Fallbacks, so it never stays stuck (the user, 2026-11-15: "getting stuck is death"):
// - No sign the game took it (no cooldown, no cast bar, no MP taken) on an image shown kRepress after a press: pressed
//   again, and so on until it does; none kGiveUpSeconds after the first press: given up on (done, with a log line).
//   Its pressed look is no sign: the game flashes the key's slot also when it refuses the skill (2026-11-15, the log:
//   Wind Strike read ready ~0.8 s after its cast ended, and the game ignored F2 then and 1.1 s later, flash and all,
//   and took it 3.3 s later, after the old 1 s and 2 s waits; the user saw it Ready with no try for 1-1.5 s). Pressing
//   a skill again while the game casts it does nothing.
// - The game took it (its cast bar showed and ended, or its cooldown began with no cast bar) and no MP was seen taken
//   kMpWait later: done, saying so. With MP not read at all, that sign is all there is: done at the cast's end, or
//   kInstantWait after its cooldown began.
// - A cast that never ends (kCastSeconds), or nothing for kMostSeconds since the first press: done.
class SkillUser {
public:
    struct Input {
        double now = 0.0;        // Seconds, the clock `shown` is on.
        bool canPress = true;    // No other press is still held.
        std::uint64_t image = 0; // The newest reading's screen image (0: none yet),
        double shown = 0.0;      // when the screen showed it,
        const statetree::Variables* variables = nullptr; // and what it said (null: nothing read).
        std::span<const UsableSkill> skills;              // The task's, top to bottom.
    };
    struct Output {
        int press = -1; // The one of skills to press now (its index); -1: none.
        bool done = false;
        std::string log; // A line for the log when something happened; empty if nothing did.
    };

    // Starts afresh (the task starts).
    void start();
    Output step(const Input& in);

    // The skill chosen (empty: none yet).
    const std::string& chosen() const { return chosen_.name; }

    // No sign of the press on an image shown this long after: again (its cooldown showed 0.19 s after a press taken,
    // its cast bar 0.29 s, clip 2026-11-15_20-53-34; with readings 0.1 s apart a press comes about every 0.4 s).
    static constexpr double kRepress = 0.3;
    static constexpr double kGiveUpSeconds = 5.0; // No sign this long since the first press: given up on.
    static constexpr double kInstantWait = 0.4;  // Cooling with no cast bar this long, MP not read: done.
    static constexpr double kMpWait = 2.0;       // Taken (cast ended or cooling), at most this long for its MP.
    static constexpr double kCastSeconds = 15.0; // A cast bar seen this long: done.
    static constexpr double kMostSeconds = 20.0; // Since the first press: done.
    static constexpr float kMpShare = 0.75f;     // Of its cost, fallen: taken (regeneration and rounding aside).

private:
    void press(const Input& in, int index, Output& out);
    void finish(Output& out, std::string why);
    std::string pressesNote() const; // ", after 5 presses over 1.60 s" when it took more than one; else empty.

    UsableSkill chosen_;
    bool done_ = false;
    bool said_ = false;            // Why it waits was logged.
    int presses_ = 0;
    double firstPressAt_ = -1.0;
    double pressedAt_ = -1.0;      // The last press (Input::now).
    std::uint64_t lastImage_ = 0;  // The newest reading looked at.
    double lastShown_ = -1.0;      // When the newest reading after the last press was shown (-1: none yet).
    double castAt_ = -1.0;         // Since the last press: its cast bar first seen (-1: not yet),
    double castEndAt_ = -1.0;      // no cast seen after it,
    double coolingAt_ = -1.0;      // its cooldown first seen with no cast bar,
    float mpTop_ = -1.0f;          // and the most MP in points from the press on (-1: not read).
    bool mpKnown_ = false;         // The newest reading after the press had MP in points.
};

} // namespace observex
