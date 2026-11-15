#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace observex {

class PicoController;

// A monster the sweep can point at: the centre of its box on the desktop (pixels, as GetCursorPos reports), and the
// tracker's id, which stays the same while the monster is followed, so the sweep can keep aiming at the same one.
struct MouseTarget {
    std::uint32_t id = 0;
    int x = 0; // Box centre on the desktop.
    int y = 0;
    int w = 0; // Box size on the desktop (for picking the biggest when there are no distances yet).
    int h = 0;
};

// -----------------------------------------------------------------------------------------------------------------
// TEMPORARY DEBUG TEST (the user asked, 2026-11-15; to be removed later). Remove this file, app/MouseTest.cpp,
// engine/PicoController.{h,cpp}, and the controller_/mouseTest_/mouseTestStatus_ members and their uses in App.
// -----------------------------------------------------------------------------------------------------------------
//
// A "mouse debug" sweep: pressed, it counts down 3 seconds, then smoothly moves the real cursor onto each visible
// monster, rests on it for 2 seconds, moves on to another, and keeps looping until the user moves the mouse. It is
// driven one step per UI frame (tick), so it reads the live monster boxes straight off the UI thread and needs no
// locking. It steers the cursor with the Pico controller's relative moves, aiming from the real cursor toward the
// target each frame, so it lands on the box even with Windows pointer acceleration on, and it stops the instant the
// cursor moves somewhere it did not steer it (the user taking over).
class MouseTest {
public:
    // The button: begins the 3 s countdown. `controller` must already be open and must outlive the sweep. Does
    // nothing if a sweep is already running.
    void begin(PicoController& controller);
    // Ends it at once (sends no more moves). Safe to call when not running.
    void stop();
    bool active() const { return phase_ != Phase::Idle; }
    bool countingDown() const { return phase_ == Phase::Countdown; }
    float countdownLeft() const { return countdownLeft_; } // Seconds left, while counting down.
    const std::string& status() const { return status_; }

    // One UI frame while active: `monsters` are the visible monsters' desktop centres right now, (`cursorX`,`cursorY`)
    // is the real cursor, and `dtMs` the milliseconds since the last tick. Advances the sweep and drives the
    // controller; may end the sweep (active() then false).
    void tick(const std::vector<MouseTarget>& monsters, int cursorX, int cursorY, double dtMs);

private:
    enum class Phase { Idle, Countdown, Moving, Resting };

    void pickTarget(const std::vector<MouseTarget>& monsters, int cursorX, int cursorY);

    PicoController* controller_ = nullptr;
    Phase phase_ = Phase::Idle;
    float countdownLeft_ = 0.0f;
    double targetX_ = 0.0; // The monster centre being moved to / rested on.
    double targetY_ = 0.0;
    std::uint32_t targetId_ = 0; // 0: none chosen yet (pick one next frame).
    std::uint32_t lastId_ = 0;   // The monster last rested on, so the next pick prefers a different one.
    float restLeft_ = 0.0f;      // Seconds left resting on a monster.
    double speed_ = 0.0;         // Current sweep speed (px/s), ramped for a smooth start and stop.
    int prevCursorX_ = 0;        // The real cursor last frame, and the move we commanded then: the two together tell
    int prevCursorY_ = 0;        // a user move (sideways or backward, or any move while we held still) from our own.
    int lastCmdX_ = 0;
    int lastCmdY_ = 0;
    bool havePrev_ = false;
    std::string status_;
};

} // namespace observex
