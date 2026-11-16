#pragma once

#include "core/CameraTurn.h"

#include <cstdint>
#include <string>
#include <string_view>

namespace observex {

// The state tree's Focus Camera task in its mode Character + Target (the user asked, 2026-11-16: "rotate with rmb +
// move gently to make the target box stay within a threshold in 1/4 or 3/4 of the screen", "parallel", "only gently
// quickly move and then stop releasing the mouse", "if task is unsure which direction to follow it can try both yaw
// directions as well as pitch to also stay vertically in proper place, but in general pitch stays around 20-30d",
// "it will work whenever lmb mouse is free"). The camera circles the own character, always in the screen's middle;
// the target is held at a quarter of the width from it, on the side it is on (the user's own play, clip
// 2026-11-15_22-08-21: the fungus held at 0.22-0.27 of the width while it walked in). One step a UI frame:
// 1. Idle: the target's box middle more than kTolerance of the width from its goal (0.25 or 0.75 of the width), or the
//    camera's pitch outside kPitchLow..kPitchHigh, or the target's box above kTop or below kBottom of the height (the
//    pitch moves only within its band for that), plans a burst: how many counts turn the camera (dx; + right) and tilt
//    it (dy; + down), at most kMaxBurstCounts each, kAim of the way (the target walks on, and no overshoot).
//    How far to turn comes from the ground camera (core/Ground): the target's feet (its box's bottom middle) put on
//    the ground at the camera's pitch and distance, turned round the own character's feet count by count until it
//    shows at the goal. That says the way too: a target beyond the character moves against the turn, one between it
//    and the camera with it, one beside it hardly at all (the earlier session's study of that clip). A target no turn
//    brings to its goal (too close) is turned only as far as it comes nearer, and not at all when that is less than
//    the tolerance.
// 2-3. The burst is one core/CameraTurn turn: the right button down, the counts once the game shows it turns the
//    camera (not within kWarpWait: let go, tried again kRetryWait later), a short smooth glide, let go.
// 4. Settled, kSettle after letting go, the looks show the turn; it is judged: nearer the goal, the ground camera is
//    trusted again; not nearer, it is unsure: the next burst goes by how far the target moved a count (or, if it did
//    not move, a small probe the other way: "try both"). kFails bursts in a row not nearer: given up on for kPause.
// Whenever the mouse is not free (Select Target or Click Mouse running, a button held by a task or by the user) it
// lets go of the right button at once and waits.
class CameraFocuser {
public:
    // A box on the screen image, in pixels.
    struct Box {
        float x0 = 0.0f;
        float y0 = 0.0f;
        float x1 = 0.0f;
        float y1 = 0.0f;
    };
    struct Input {
        double now = 0.0;     // Seconds.
        float width = 0.0f;   // The screen image.
        float height = 0.0f;
        bool mouseFree = false;      // Nothing else wants the mouse now.
        bool cursorAtMiddle = false; // The cursor is where the game puts it when it turns the camera.
        bool target = false;         // The target's box is known and seen on the newest look,
        std::uint32_t targetId = 0;  // its tracker id,
        Box targetBox{};             // where,
        std::string_view targetName; // and its name (for the log; may be empty).
        bool own = false;            // The own character's box (its bottom gives the camera's distance).
        Box ownBox{};
        bool pitchKnown = false;     // The camera's pitch, radians down (core/MouseCamera); else kGuessPitch.
        float pitch = 0.0f;
        bool distanceKnown = false;  // Its distance, own heights (the ground finder's); else by the own box, or
        float distance = 0.0f;       // kGuessDistance.
    };
    struct Output {
        int dx = 0;           // Counts to send now: with the right button held they turn (+ right)
        int dy = 0;           // and tilt (+ down) the camera.
        bool right = false;   // The right button held.
        bool inPlace = false; // Nothing to do: the target where wanted (or none, or given up on for now).
        std::string log;      // A line for the log when something happened; empty if nothing did.
    };

    // Starts afresh (the task starts), the right button taken as let go.
    void start();
    Output step(const Input& in);

    static constexpr float kGoal = 0.25f;       // Of the width from the screen's middle.
    static constexpr float kTolerance = 0.03f;  // Of the width either way (the user's own play).
    static constexpr float kSideSwitch = 0.05f; // Past the middle by this share of the width: the other side.
    static constexpr float kAim = 0.85f;
    static constexpr int kMaxBurstCounts = 150; // 30 degrees.
    static constexpr int kMinCounts = 2;
    static constexpr int kScanCounts = 455;     // 90 degrees each way looked at.
    static constexpr double kCountsPerSecond = CameraTurner::kCountsPerSecond; // The burst (core/CameraTurn).
    static constexpr double kWarpWait = CameraTurner::kWarpWait;
    static constexpr double kSettle = CameraTurner::kSettle;
    static constexpr double kRetryWait = 0.3;
    static constexpr int kFails = 3;
    static constexpr double kPause = 1.5;
    static constexpr float kMinGain = 0.3f;     // Pixels a count: a burst that moved the target less did not move it.
    static constexpr int kProbeCounts = 25;     // 5 degrees.
    static constexpr float kPitchLow = 0.349066f;  // 20 degrees down.
    static constexpr float kPitchHigh = 0.523599f; // 30 degrees down.
    static constexpr float kPitchSlack = 0.017453f; // 1 degree: less is no tilt.
    static constexpr float kTop = 0.12f;        // The target's box kept between these shares of the height.
    static constexpr float kBottom = 0.88f;
    static constexpr float kGuessPitch = 0.349066f;
    static constexpr float kGuessDistance = 8.0f; // The user's clips: 7 to 9 own heights mostly.

private:
    struct Plan {
        int dx = 0;
        int dy = 0;
        bool yawByCamera = false; // dx from the ground camera (else from the last burst, or a probe).
        float expected = 0.0f;    // How much nearer its goal the turn should bring the target (pixels).
        bool tooClose = false;    // No turn brings the target to its goal.
        bool canTilt = true;      // The pitch is known.
    };

    Plan plan(const Input& in, float goal) const;
    void judge(const Input& in, Output& out);
    float goalOf(const Input& in);
    std::string who(const Input& in) const;

    CameraTurner turner_;       // The burst under way.
    int burstX_ = 0;            // Its counts.
    float beforeX_ = 0.0f;      // The target's box middle and goal when it was planned,
    float beforeGoal_ = 0.0f;
    bool judgeYaw_ = false;     // and whether the burst's turn is to be judged,
    bool byCamera_ = false;     // whether it was the ground camera's,
    float expected_ = 0.0f;     // and how much nearer it should bring the target (pixels).
    std::uint32_t target_ = 0;  // The target it works on (another: afresh).
    int side_ = 0;              // -1 left, +1 right, 0 not chosen.
    bool unsure_ = false;       // The ground camera's turn did not bring it nearer.
    float gain_ = 0.0f;         // Then: how far the target moved a count on the last burst (pixels).
    int lastX_ = 0;             // That burst's turn.
    int fails_ = 0;
    double pausedUntil_ = -1.0;
    double retryAt_ = -1.0;
    bool saidTooClose_ = false;
    bool saidNoPitch_ = false;
};

} // namespace observex
