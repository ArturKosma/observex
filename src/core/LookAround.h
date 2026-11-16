#pragma once

#include "core/CameraTurn.h"

#include <cstdint>
#include <string>

namespace observex {

// The state tree's Look Around task (the user asked, 2026-11-16: "before we decide to select a monster, we first
// rotate around, remember all monsters and their distances, and then decide on the closest"): the camera turned once
// round the own character by the right button while the monster memory (core/MonsterMemory) fills from every look on
// the way. One step a UI frame:
// 1. Starts: the memory is to be forgotten (Output::clearMemory; the own character may have walked since).
// 2. Pins the pitch if no limit pinned it yet (MouseCamera::View::pitchPinned): a tilt down past the game's limit
//    (kPinCounts, more than from looking up its farthest to the limit), so the counts know the pitch exactly from then
//    on and the distances hang on it (2026-11-16: "the pitch is the error").
// 3. The turn: `degrees` to the right at kDegreesPerSecond in one press (one press: some presses the game never takes),
//    the pitch brought to kPitch on the way when known (20-30 degrees down, as Focus Camera keeps it).
// 4. Done once the turn settled (core/CameraTurn).
// Never stuck: a press the game does not take is tried again, kPresses in all, then it is done with a note; the mouse
// wanted (the user's own button down), it lets go at once and goes on with the rest of the turn once free; and it is
// done with a note kGiveUpSlack seconds past the time the turns should take.
class LookAround {
public:
    struct Input {
        double now = 0.0;            // Seconds.
        bool mouseFree = true;       // No button held by another task or by the user.
        bool cursorAtMiddle = false; // The cursor is where the game puts it when it turns the camera.
        bool pitchKnown = false;     // The camera's pitch (core/MouseCamera), radians down,
        bool pitchPinned = false;    // pinned by a limit (exact since),
        float pitch = 0.0f;
        int degrees = 360;           // How far to turn (statetree::Task::degrees).
    };
    struct Output {
        int dx = 0;           // Counts to send now: with the right button held they turn (+ right)
        int dy = 0;           // and tilt (+ down) the camera.
        bool right = false;   // The right button held.
        bool clearMemory = false; // Forget the monster memory now (on its first step).
        bool done = false;
        std::string log;      // A line for the log when something happened; empty if nothing did.
    };

    void start(); // Afresh (the task starts), the right button taken as let go.
    Output step(const Input& in);
    // Lets go of the right button at once if held (its state left mid-turn); whether it held it.
    bool letGo(double now) { return turner_.letGo(now); }

    static constexpr double kDegreesPerSecond = 90.0; // 455 counts a second: the whole turn in 4 s.
    static constexpr float kPitch = 0.436332f;        // 25 degrees down.
    static constexpr float kPitchSlack = 0.087266f;   // 5 degrees either way: 20-30.
    static constexpr int kPinCounts = 600; // 118.7 degrees: past the 82-degree limit from 28.5 up (the farthest seen).
    static constexpr int kPresses = 3;
    static constexpr double kGiveUpSlack = 6.0;

private:
    enum class Stage : std::uint8_t { Start, Pinning, Turning, Done };

    void beginTurn(const Input& in, Output& out);

    CameraTurner turner_;
    Stage stage_ = Stage::Start;
    double startedAt_ = 0.0;
    int pinSent_ = 0;          // The pin's counts sent so far,
    bool pinnedHere_ = false;  // all of them: the camera is at the limit.
    int total_ = 0;            // The turn's counts,
    int sent_ = 0;             // and those sent in its presses so far.
    bool tiltPlanned_ = false; // The tilt to kPitch worked out (at the turn's first press),
    int tiltLeft_ = 0;         // and what of it is still to send.
    int presses_ = 0;          // Of the stage, not taken.
    bool saidWanted_ = false;
};

} // namespace observex
