#pragma once

#include <cstdint>

namespace observex {

// One turn of the game's camera by the right button (Lineage 2 turns it by the mouse's counts while the right button
// is held and it took the press for turning; core/MouseCamera), shared by the state tree's tasks that turn it (Focus
// Camera's bursts, Look Around, Select Target turning to a monster off the screen). One step a UI frame:
// 1. Pressing: the right button down; the counts wait until the game shows it turns the camera (it puts the cursor in
//    its window's middle: 7 ms to 0.45 s after a press, and some presses it never took). Not within kWarpWait: let go
//    (Event::NotTaken), the counts never sent.
// 2. Turning: the counts go out over a smooth glide (minimum jerk: no jolt at either end) at `countsPerSecond` on
//    average (at least kMinSeconds), then the button is held kHoldAfter
// 3. and let go (the game puts the cursor back where it was).
// 4. Settling: kSettle after letting go the looks show the turn: Event::Settled, and it is idle.
// letGo() lets go at once (the mouse is wanted, the task stopped): settling then, with the counts sent so far.
class CameraTurner {
public:
    enum class Event : std::uint8_t {
        None,
        NotTaken, // The game never took the press for turning (let go, nothing sent; settling now).
        Settled,  // Let go kSettle ago: the looks show the turn (idle now).
    };
    struct Step {
        int dx = 0;         // Counts to send now: with the right button held they turn (+ right)
        int dy = 0;         // and tilt (+ down) the camera.
        bool right = false; // The right button held after this step.
        Event event = Event::None;
    };

    // A turn of `dx` and `dy` counts begins now: the right button goes down (Step::right from the next step on).
    // `cursorAtMiddle`: the cursor is at the window's middle already (then it waits kSameMiddleWait for the game).
    void begin(double now, int dx, int dy, bool cursorAtMiddle, double countsPerSecond = kCountsPerSecond);
    Step step(double now, bool cursorAtMiddle);
    // Lets go of the right button at once, if it is held (settling then); whether it was.
    bool letGo(double now);
    // Back to idle at once, the button taken as let go (the task starts afresh).
    void reset() { *this = CameraTurner(); }

    bool idle() const { return phase_ == Phase::Idle; }
    bool holds() const { return phase_ == Phase::Pressing || phase_ == Phase::Turning || phase_ == Phase::Holding; }
    // The game took the press and the counts go out (or went out).
    bool taken() const { return taken_; }
    // The counts of this turn sent so far.
    int sentX() const { return sentX_; }
    int sentY() const { return sentY_; }

    static constexpr double kCountsPerSecond = 600.0;
    static constexpr double kMinSeconds = 0.06;
    static constexpr double kWarpWait = 0.5;
    static constexpr double kSameMiddleWait = 0.06; // The cursor at the middle already when pressed: go after this.
    static constexpr double kHoldAfter = 0.03;
    static constexpr double kSettle = 0.12;

private:
    enum class Phase : std::uint8_t { Idle, Pressing, Turning, Holding, Settling };

    Phase phase_ = Phase::Idle;
    double phaseAt_ = 0.0;      // When the phase began.
    bool middleBefore_ = false; // The cursor at the middle when the right button went down.
    bool taken_ = false;
    int turnX_ = 0;
    int turnY_ = 0;
    int sentX_ = 0;
    int sentY_ = 0;
    double glideSeconds_ = 0.0;
};

} // namespace observex
