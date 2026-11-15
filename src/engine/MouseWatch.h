#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <thread>

namespace observex {

// One report of the mouse, as Windows' Raw Input has it.
struct MouseInput {
    std::int64_t qpc = 0;      // When it came (QueryPerformanceCounter).
    int dx = 0;                // Counts moved right and down, before Windows' pointer speed and acceleration (0 for
    int dy = 0;                // a mouse that says where it is instead: a pen, a remote desktop).
    std::uint16_t buttons = 0; // RAWMOUSE::usButtonFlags: the buttons pressed and let go with it, the wheel turned.
    std::uint8_t held = 0;     // The buttons down after it: 1 left, 2 right, 4 middle.
    std::int16_t wheel = 0;    // How far the wheel turned (120 a notch, + away from the user).
    int cursorX = 0;           // Where the cursor was then (screen pixels).
    int cursorY = 0;
};

// Watches the mouse wherever it is used (the game in front), every report it makes, with when it came: so a clip
// keeps how far it moved while the right button turned the camera (the user asked, 2026-11-15: from how much a count
// turns the camera, the camera's every turn and tilt is known exactly, even in a turn too fast for the screen to
// follow). Raw Input (RIDEV_INPUTSINK) on a window of its own that is never shown, on a thread of its own; it reads,
// never moves the mouse. A process has one Raw Input mouse, so make one at a time.
class MouseWatch {
public:
    // Called on the watch's thread for each report; must not wait.
    using Sink = std::function<void(const MouseInput&)>;

    // Watching once it returns. Throws on failure.
    explicit MouseWatch(Sink sink);
    ~MouseWatch();

    MouseWatch(const MouseWatch&) = delete;
    MouseWatch& operator=(const MouseWatch&) = delete;

    // Windows' pointer settings now, e.g. "pointer speed 10 of 20 (as moved), enhance pointer precision off": what the
    // cursor makes of the counts (the counts themselves are before them).
    static std::string pointerSettings();

private:
    void run(std::string* error, void* ready);

    Sink sink_;
    unsigned long threadId_ = 0;
    std::thread thread_;
};

} // namespace observex
