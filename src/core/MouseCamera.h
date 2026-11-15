#pragma once

#include "core/ClipLines.h"
#include "core/Ground.h"

#include <cmath>
#include <cstdint>
#include <deque>
#include <mutex>
#include <vector>

namespace observex {

// The game's camera turned and tilted by the mouse's counts (the user asked, 2026-11-15 evening: "figure out the
// proper math as to how much the camera is moving ingame based on my mouse stride delta", then for its yaw and pitch
// every frame, confirmed from time to time). Measured on the user's 7 clips of that night (2026-11-15_21-42-40 to
// 21-46-09: a 360, tilts to the limits, flicks; session 3307e15c's scratchpad pitch/):
// - A count turns or tilts the camera 36 of the engine's 65536 rotation units a turn (0.19775 degrees), both ways,
//   at any speed: Windows' pointer speed and acceleration do not touch raw counts. The 360 of clip 21-42-40 lined up
//   with its first image at 361.69 degrees of counts by the images alone, the minimap's cone said 35.98 units between
//   rests, and each frame's points moved in whole steps of it.
// - Only while the right button is held and the game is in its camera mode: it puts the cursor in its window's middle
//   when it starts turning, seen on the first report after (7 ms to 0.45 s after the press). 5 presses of 16 it never
//   did, and their counts turned nothing. The report may move it on at once: (1279, 723) on the user's clip
//   2026-11-15_23-34-30, whose 9 s turn (180 degrees by its cone) the exact middle alone lost, so near the middle
//   counts too, if the cursor jumped there (kWarpSlack, kWarpPixelsPerCount: 5 more presses of the 34 in the clips of
//   that night, none of the 5 never taken). So counted, the cone and the counts stayed within 0.8 degrees (the cone's
//   own error) over up to 3060 degrees turned: integer counts do not drift.
// - The pitch stops at kTopUnits (looking down steepest); the counts beyond go for nothing.
// - A frame shows what the game took of the reports up to about kLagMs before it was captured, each report's counts
//   spread over the kSpreadMs before it (82% of the frames within a count of the points, the rest within two).
// Turns are kept as whole units (no float sums); the pitch as whole units clamped as the game does. The yaw is the
// minimap cone's (anchored while the camera rests), the pitch the limits' (exact once pushed past one) or, before,
// the camera's turns' (heuristics/TurnPitch) or the follower's.
class MouseCamera {
public:
    static constexpr int kUnitsPerCount = 36;
    static constexpr int kUnitsPerTurn = 65536;
    static constexpr double kUnitRadians = 2.0 * 3.14159265358979323846 / kUnitsPerTurn;
    static constexpr double kCountRadians = kUnitsPerCount * kUnitRadians;
    // The pitch's limits: looking down 82.0 degrees at most (the turns' pitch while yawing there, clip 21-44-29: 82.015
    // degrees, 0.2 either way over 120 frames, lens 1741), up 28.5 degrees not yet (the lowest the clips went, into the
    // ground in 21-46-09): none below.
    static constexpr int kTopUnits = 14930;
    static constexpr int kBottomUnits = -16384;
    static constexpr double kLagMs = 13.0;
    static constexpr double kSpreadMs = 12.0;
    // Live, the reports kept (older ones are only summed).
    static constexpr double kKeepMs = 3000.0;
    // The cone is taken only while no counts turned the camera this long before its image (it is drawn about 25 ms
    // late and redrawn every 25 to 42 ms).
    static constexpr double kConeRestMs = 120.0;
    // A rest's cone this far (radians) from the yaw the counts say: the camera was turned by something else, the
    // rests before are forgotten.
    static constexpr float kYawJump = 0.035f;
    // The rests whose cones the yaw is the middle of.
    static constexpr int kRests = 9;
    // A pitch measured (TurnPitch) is taken once this many of them say it, by the middle of what they say: before a
    // limit pinned it kPitchTakes, after only if kPitchChecks say it is kPitchOff off (the counts lost some).
    static constexpr int kPitchTakes = 5;
    static constexpr int kPitchFirst = 3; // First, or over the follower's (it may be 12 degrees off: clip 21-46-09).
    static constexpr float kPitchBeyond = 0.0087f; // A pitch said half a degree past a limit is not taken.
    static constexpr int kPitchChecks = 9;
    static constexpr float kPitchOff = 0.035f; // 2 degrees: the turns' pitch is up to 1 off the counts' between the
                                                // limits (low 0.7-1.0 high, 30-70 degrees 0.6-0.9 low).
    // Pushed this many units past a limit, the pitch is that limit's: exact.
    static constexpr int kPinUnits = 5 * kUnitsPerCount;
    // The game put the cursor in the middle if a report after the press has it there, or within kWarpSlack plus
    // kWarpPixelsPerCount for each of its counts (Windows' pointer speed and acceleration move it about 1 to 3 pixels a
    // count) after a jump farther than that.
    static constexpr int kWarpSlack = 2;
    static constexpr int kWarpPixelsPerCount = 4;

    struct Settings {
        int topUnits = kTopUnits;
        int bottomUnits = kBottomUnits;
        double lagMs = kLagMs;
        double spreadMs = kSpreadMs;
        double keepMs = kKeepMs; // A clip's: as long as it is.
    };
    MouseCamera() = default;
    explicit MouseCamera(const Settings& settings) : settings_(settings) {}

    // The game window's middle (desktop pixels), where it puts the cursor when it starts turning.
    void middle(int x, int y) {
        middleX_ = x;
        middleY_ = y;
    }
    // The next report of the mouse (times must not go back).
    void add(const ClipMouse& report);

    // What said the pitch last: nothing yet, the follower, the camera's turns, a limit (exact).
    enum class PitchBy : std::uint8_t { Nothing, Follower, Turns, Limit };

    struct View {
        bool known = false;      // Any report came.
        bool yawKnown = false;   // The minimap's cone anchored the yaw.
        bool pitchKnown = false; // Something said the pitch: a limit, the turns or the follower;
        bool pitchPinned = false; // a limit (since then the counts alone).
        PitchBy pitchBy = PitchBy::Nothing;
        bool turning = false;    // Counts turned or tilted the camera within kConeRestMs before.
        long long turnUnits = 0; // Turned by counts since the first report (whole units, unwrapped),
        int pitchUnits = 0;      // its pitch in units (+ down).
        float yaw = 0.0f;   // Radians clockwise from north (the cone's), -pi to pi.
        float pitch = 0.0f; // Radians, + down (GroundCamera::pitch).
        double turned = 0.0; // turnUnits in radians.
    };
    // The camera on a frame captured at `timeMs`.
    View at(double timeMs) const;

    // The minimap's cone said `heading` (radians clockwise from north) on an image captured at `timeMs`: taken while
    // the camera rested since kConeRestMs before.
    void heading(double timeMs, float heading);
    // The camera's turns said its pitch was `pitch` (radians) on an image captured at `timeMs`.
    void pitched(double timeMs, float pitch);
    // The follower's pitch (the ground, the depth network): taken only while nothing else said one.
    void guessPitch(double timeMs, float pitch);
    // How far the anchors were off lately (radians): the last rest's cone from the counts' yaw, the middle of the
    // last turns' pitches from the counts' (the user's check of the counts, F3 / Stats).
    float yawOff() const { return yawOff_; }
    float pitchOff() const { return pitchOff_; }
    int pitchSamples() const { return static_cast<int>(pitches_.size()); }
    void clear();

private:
    struct State {
        double timeMs = 0.0;  // The report's time,
        double startMs = 0.0; // when the game started taking its counts.
        long long turn = 0;   // After it: units turned and the pitch.
        int pitch = 0;
        bool turning = false; // It turned or tilted the camera.
    };
    // The state the game had taken by `timeMs` (between the two reports around it).
    void stateAt(double timeMs, long long& turn, int& pitch) const;
    double lastTurningBefore(double timeMs) const;
    void shiftPitch(int units); // The pitch was off: all states moved.
    void closeRest();

    Settings settings_;
    int middleX_ = 1280;
    int middleY_ = 720;
    bool pressed_ = false; // The right button went down and is held,
    bool cameraMode_ = false; // and the game took it for turning.
    bool cursorSeen_ = false; // The cursor on the last report.
    int cursorX_ = 0;
    int cursorY_ = 0;
    std::deque<State> states_;
    State base_; // Before the first kept.
    long long turn_ = 0;
    int pitch_ = 0;
    bool pitchKnown_ = false;
    bool pitchPinned_ = false;
    PitchBy pitchBy_ = PitchBy::Nothing;
    int pinPush_ = 0; // Units pushed past the limit it is at,
    int pinNeed_ = 0; // and how far it must be to be pinned (how far off the pitch may be).
    double lastTurningMs_ = -1e300;
    // The yaw: the cone's heading less the counts' turn (radians), the middle of the last kRests rests' (each the
    // middle of its cones).
    bool yawKnown_ = false;
    float yawOffset_ = 0.0f;
    std::vector<float> restCones_; // This rest's.
    std::deque<float> rests_;
    float yawOff_ = 0.0f;
    std::vector<float> pitches_; // Measured pitches less the counts' since the last taken (radians).
    float pitchOff_ = 0.0f;
};

// MouseCamera for several threads: the mouse's reports come on its own, the follower and the UI ask on theirs.
class SharedMouseCamera {
public:
    explicit SharedMouseCamera(const MouseCamera::Settings& settings = {}) : camera_(settings) {}
    void middle(int x, int y) {
        const std::lock_guard lock(mutex_);
        camera_.middle(x, y);
    }
    void add(const ClipMouse& report) {
        const std::lock_guard lock(mutex_);
        camera_.add(report);
    }
    MouseCamera::View at(double timeMs) const {
        const std::lock_guard lock(mutex_);
        return camera_.at(timeMs);
    }
    void heading(double timeMs, float heading) {
        const std::lock_guard lock(mutex_);
        camera_.heading(timeMs, heading);
    }
    void pitched(double timeMs, float pitch) {
        const std::lock_guard lock(mutex_);
        camera_.pitched(timeMs, pitch);
    }
    void guessPitch(double timeMs, float pitch) {
        const std::lock_guard lock(mutex_);
        camera_.guessPitch(timeMs, pitch);
    }
    void offs(float& yaw, float& pitch) const {
        const std::lock_guard lock(mutex_);
        yaw = camera_.yawOff();
        pitch = camera_.pitchOff();
    }
    void clear() {
        const std::lock_guard lock(mutex_);
        camera_.clear();
    }

private:
    mutable std::mutex mutex_;
    MouseCamera camera_;
};

// GroundHints' mouse from `view` (the counts on the image).
inline void mouseHints(const MouseCamera::View& view, GroundHints& hints) {
    hints.mouseKnown = view.known;
    hints.mouseYawKnown = view.yawKnown;
    hints.mousePitchKnown = view.pitchKnown;
    hints.mouseTurned = view.turned;
    hints.mouseYaw = view.yaw;
    hints.mousePitch = view.pitch;
}

// What the Stats tab's Camera card shows of `view` (the counts on the frame shown), in degrees, with how far off the
// cone and the turns said it was lately (radians: yawOff and pitchOff): live, and on a clip what was shown (its
// camera lines) and Today's (engine/ClipGround).
inline CameraShown cameraOf(const MouseCamera::View& view, float coneOff, float turnsOff) {
    constexpr double kDegrees = 57.29577951308232;
    CameraShown shown;
    if (!view.known) {
        return shown;
    }
    shown.known = true;
    shown.yawKnown = view.yawKnown;
    shown.pitchKnown = view.pitchKnown;
    shown.pitchBy = static_cast<int>(view.pitchBy);
    const double yaw = view.yawKnown ? view.yaw * kDegrees : view.turned * kDegrees;
    shown.yaw = static_cast<float>(view.yawKnown ? std::fmod(std::fmod(yaw, 360.0) + 360.0, 360.0) : yaw);
    shown.pitch = view.pitchKnown ? static_cast<float>(view.pitch * kDegrees) : 0.0f;
    shown.coneOff = static_cast<float>(coneOff * kDegrees);
    shown.turnsOff = static_cast<float>(turnsOff * kDegrees);
    return shown;
}

// What the follower saw on the image captured at `timeMs` (`note`, and the camera it has then) anchors `camera` (a
// MouseCamera or SharedMouseCamera): the minimap's cone, the turns' pitch where it turned enough to say it surely
// (as bench --turns: at least kAnchorTurn, kAnchorAgreeing points agreeing within kAnchorSpread), its own pitch
// while nothing else said one.
inline constexpr float kAnchorTurn = 0.0105f;   // 0.6 degrees.
inline constexpr int kAnchorAgreeing = 100;
inline constexpr float kAnchorSpread = 0.0087f; // 0.5 degrees.
template <class Camera>
void anchorMouse(Camera& camera, double timeMs, const GroundNote& note, const GroundCamera& followed) {
    if (note.headed) {
        camera.heading(timeMs, note.heading);
    }
    if (note.turnFitted && note.turnAgreeing >= kAnchorAgreeing && note.turnSpread < kAnchorSpread &&
        std::fabs(note.turnTurn) >= kAnchorTurn) {
        camera.pitched(timeMs, note.turnPitch);
    }
    if (followed.known && followed.pitchMeasured) {
        camera.guessPitch(timeMs, followed.pitch);
    }
}

} // namespace observex
