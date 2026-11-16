#include "core/CameraFocus.h"

#include "core/Ground.h"
#include "core/MouseCamera.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace observex {

namespace {

constexpr double kDegrees = 57.29577951308232;

int signOf(double value) {
    return (value > 0.0) - (value < 0.0);
}

int counts(double value) {
    return static_cast<int>(std::lround(value));
}

float middleOf(const CameraFocuser::Box& box) {
    return (box.x0 + box.x1) * 0.5f;
}

// The camera as the input says (or guesses) it, and the target's feet (its box's bottom middle) on the ground, in own
// heights right and away of the own character's feet.
struct Scene {
    GroundCamera camera;
    bool onGround = false; // False: at or above the horizon (far off).
    float right = 0.0f;
    float away = 0.0f;
};

Scene sceneOf(const CameraFocuser::Input& in) {
    Scene scene;
    const float pitch = in.pitchKnown ? in.pitch : CameraFocuser::kGuessPitch;
    scene.camera = groundCamera(pitch, CameraFocuser::kGuessDistance, in.width, in.height);
    if (in.distanceKnown && in.distance > 0.0f) {
        scene.camera.distance = in.distance;
    } else if (in.own) {
        if (const std::optional<float> distance = distanceForOwnBottom(scene.camera, in.ownBox.y1)) {
            scene.camera.distance = *distance;
        }
    }
    if (const std::optional<GroundPoint> feet = screenToGround(scene.camera, middleOf(in.targetBox), in.targetBox.y1)) {
        scene.onGround = true;
        scene.right = feet->x;
        scene.away = feet->y;
    }
    return scene;
}

// Where the target's feet show once the camera turned `turn` counts (+ right: the ground turns the other way round
// the own character's feet) and looks down at `pitch`.
GroundPoint turned(const Scene& scene, int turn, float pitch) {
    const double angle = turn * MouseCamera::kCountRadians;
    const double c = std::cos(angle);
    const double s = std::sin(angle);
    GroundCamera camera = scene.camera;
    camera.pitch = pitch;
    return groundToScreen(camera, static_cast<float>(scene.right * c - scene.away * s),
                          static_cast<float>(scene.right * s + scene.away * c));
}

std::string share(float x, float width) {
    char text[16];
    std::snprintf(text, sizeof(text), "%.2f", width > 0.0f ? x / width : 0.0f);
    return text;
}

const char* wayOf(int turn) {
    return turn > 0 ? "right" : "left";
}

} // namespace

void CameraFocuser::start() {
    *this = CameraFocuser();
}

float CameraFocuser::goalOf(const Input& in) {
    // The side the target is on, kept until it is kSideSwitch past the middle the other way (no flip-flopping while it
    // walks across in front of or behind the character).
    const float middle = in.width * 0.5f;
    const float x = middleOf(in.targetBox);
    if (side_ == 0) {
        side_ = x < middle ? -1 : 1;
    } else if (side_ < 0 && x > middle + kSideSwitch * in.width) {
        side_ = 1;
    } else if (side_ > 0 && x < middle - kSideSwitch * in.width) {
        side_ = -1;
    }
    return middle + static_cast<float>(side_) * kGoal * in.width;
}

std::string CameraFocuser::who(const Input& in) const {
    return in.targetName.empty() ? std::string("the target") : std::string(in.targetName);
}

CameraFocuser::Plan CameraFocuser::plan(const Input& in, float goal) const {
    Plan plan;
    const Scene scene = sceneOf(in);
    const float x = middleOf(in.targetBox);
    const float error = goal - x;
    const float tolerance = kTolerance * in.width;
    if (std::fabs(error) > tolerance) {
        // The ground camera's turn: the fewest counts either way that bring the target's feet to the goal, looked at
        // count by count (the target beyond the character moves against the turn, between it and the camera with it,
        // beside it hardly at all). None does (too close): only as far as it comes nearest, if that is worth it.
        int byCamera = 0;
        float expectedAt = x;
        bool tooClose = false;
        if (!scene.onGround) {
            // Far off, at the horizon: it moves as the background does, against the turn, a lens's pixels a radian.
            byCamera = counts(-error / (scene.camera.focal * MouseCamera::kCountRadians) * kAim);
            expectedAt = x - static_cast<float>(byCamera * scene.camera.focal * MouseCamera::kCountRadians);
        } else {
            int crossing = 0;
            int nearest = 0;
            float nearestOff = std::fabs(error);
            for (const int way : {1, -1}) {
                for (int k = 1; k <= kScanCounts; ++k) {
                    const GroundPoint at = turned(scene, way * k, scene.camera.pitch);
                    if (!at.ahead) {
                        break;
                    }
                    const float off = goal - at.x;
                    if (signOf(off) != signOf(error)) {
                        if (crossing == 0 || k < std::abs(crossing)) {
                            crossing = way * k;
                        }
                        break;
                    }
                    if (std::fabs(off) < nearestOff - 0.5f) {
                        nearestOff = std::fabs(off);
                        nearest = way * k;
                    }
                }
            }
            if (crossing != 0) {
                byCamera = counts(crossing * kAim);
            } else if (std::fabs(error) - nearestOff >= tolerance) {
                byCamera = counts(nearest * kAim);
            } else {
                tooClose = true;
            }
            byCamera = std::clamp(byCamera, -kMaxBurstCounts, kMaxBurstCounts);
            expectedAt = turned(scene, byCamera, scene.camera.pitch).x;
        }
        byCamera = std::clamp(byCamera, -kMaxBurstCounts, kMaxBurstCounts);
        int yaw = byCamera;
        float expected = std::fabs(error) - std::fabs(goal - expectedAt);
        plan.yawByCamera = true;
        if (unsure_) {
            // The camera's turn did not bring it nearer last time: by how far it moved a count then, unless the
            // camera's way agrees; it did not move at all: a small probe the other way ("try both").
            plan.yawByCamera = false;
            if (std::fabs(gain_) >= kMinGain) {
                const int byGain = std::clamp(counts(error / gain_ * kAim), -kMaxBurstCounts, kMaxBurstCounts);
                if (byCamera != 0 && signOf(byCamera) == signOf(byGain)) {
                    plan.yawByCamera = true;
                } else {
                    yaw = byGain;
                    expected = std::fabs(gain_ * static_cast<float>(yaw));
                }
            } else {
                yaw = -signOf(lastX_ != 0 ? lastX_ : -error) * kProbeCounts;
                expected = tolerance;
            }
        } else {
            plan.tooClose = tooClose;
        }
        if (std::abs(yaw) >= kMinCounts) {
            plan.dx = yaw;
            plan.expected = std::max(expected, 0.0f);
        }
    }

    // The pitch: within kPitchLow..kPitchHigh, and inside that the target's box between kTop and kBottom of the
    // height (where it will be after the turn above): too high (far off), up as far as the band lets; too low, down.
    plan.canTilt = in.pitchKnown;
    if (in.pitchKnown) {
        const float now = in.pitch;
        float want = std::clamp(now, kPitchLow, kPitchHigh);
        if (scene.onGround) {
            const float top = in.targetBox.y0;
            const float bottom = in.targetBox.y1;
            const auto rows = [&](float pitch, float& shownTop, float& shownBottom) {
                const GroundPoint at = turned(scene, plan.dx, pitch);
                shownBottom = at.y;
                shownTop = top + (at.y - bottom);
                return at.ahead;
            };
            const auto step = static_cast<float>(MouseCamera::kCountRadians);
            float shownTop = 0.0f;
            float shownBottom = 0.0f;
            if (rows(want, shownTop, shownBottom)) {
                if (shownTop < kTop * in.height) {
                    for (float pitch = want;; pitch -= step) {
                        want = std::max(pitch, kPitchLow);
                        if (want <= kPitchLow || (rows(want, shownTop, shownBottom) && shownTop >= kTop * in.height)) {
                            break;
                        }
                    }
                } else if (shownBottom > kBottom * in.height) {
                    for (float pitch = want;; pitch += step) {
                        want = std::min(pitch, kPitchHigh);
                        if (want >= kPitchHigh ||
                            (rows(want, shownTop, shownBottom) && shownBottom <= kBottom * in.height)) {
                            break;
                        }
                    }
                }
            }
        }
        if (std::fabs(want - now) >= kPitchSlack) {
            plan.dy = std::clamp(counts((want - now) / MouseCamera::kCountRadians), -kMaxBurstCounts, kMaxBurstCounts);
        }
    }
    return plan;
}

void CameraFocuser::judge(const Input& in, Output& out) {
    // How the burst's turn did, on the looks since it settled: nearer the goal by half what was expected (or within the
    // tolerance) is right; else it is unsure (see plan), and after kFails in a row it is given up on for kPause.
    if (!judgeYaw_ || !in.target || in.targetId != target_ || burstX_ == 0) {
        return;
    }
    judgeYaw_ = false;
    const float x = middleOf(in.targetBox);
    const float before = std::fabs(beforeGoal_ - beforeX_);
    const float after = std::fabs(beforeGoal_ - x);
    gain_ = (x - beforeX_) / static_cast<float>(burstX_);
    lastX_ = burstX_;
    if (after <= kTolerance * in.width || before - after >= 0.5f * expected_) {
        fails_ = 0;
        if (byCamera_) {
            unsure_ = false;
        }
        return;
    }
    char line[400];
    if (++fails_ >= kFails) {
        fails_ = 0;
        unsure_ = false;
        pausedUntil_ = in.now + kPause;
        std::snprintf(line, sizeof(line),
                      "Focus Camera: cannot hold %s at %s of the width: %d turns brought it no nearer (too close?); "
                      "trying again in %.1f s",
                      who(in).c_str(), share(beforeGoal_, in.width).c_str(), kFails, kPause);
        out.log = line;
        return;
    }
    unsure_ = true;
    const bool moved = std::fabs(gain_) >= kMinGain;
    std::snprintf(line, sizeof(line),
                  "Focus Camera: %s came no nearer (%s -> %s of the width, wants %s) after turning %s %d counts: "
                  "unsure of the way, %s",
                  who(in).c_str(), share(beforeX_, in.width).c_str(), share(x, in.width).c_str(),
                  share(beforeGoal_, in.width).c_str(), wayOf(burstX_), std::abs(burstX_),
                  moved ? "going by how far it moved" : (std::string("trying ") + wayOf(-burstX_)).c_str());
    out.log = line;
}

CameraFocuser::Output CameraFocuser::step(const Input& in) {
    Output out;
    out.right = turner_.holds();
    if (in.target && in.targetId != target_) { // Another target: afresh (a burst under way goes on, unjudged).
        target_ = in.targetId;
        side_ = 0;
        unsure_ = false;
        gain_ = 0.0f;
        lastX_ = 0;
        fails_ = 0;
        pausedUntil_ = -1.0;
        saidTooClose_ = false;
        judgeYaw_ = false;
    }
    if (!in.mouseFree) { // Select Target, Click Mouse or the user wants the mouse: let go at once and wait.
        if (turner_.letGo(in.now)) {
            out.right = false;
            judgeYaw_ = false;
            out.log = "Focus Camera: let go of the right button, the mouse is wanted";
        }
        return out;
    }
    if (!turner_.idle()) { // A burst under way (core/CameraTurn).
        const CameraTurner::Step s = turner_.step(in.now, in.cursorAtMiddle);
        out.dx = s.dx;
        out.dy = s.dy;
        out.right = s.right;
        if (s.event == CameraTurner::Event::NotTaken) {
            judgeYaw_ = false;
            retryAt_ = in.now + kRetryWait;
            out.log = "Focus Camera: the game did not start turning the camera (the cursor never went to its middle "
                      "within 0.5 s); let go of the right button, trying again";
            return out;
        }
        if (s.event != CameraTurner::Event::Settled) {
            return out;
        }
        judge(in, out);
        if (!out.log.empty()) {
            return out; // The next burst on the next step.
        }
    }

    if (in.now < pausedUntil_) {
        out.inPlace = true; // Given up on for now: nothing to wait for.
        return out;
    }
    if (in.now < retryAt_) {
        return out;
    }
    if (!in.target || in.width <= 0.0f || in.height <= 0.0f) {
        out.inPlace = true; // Nothing to hold.
        return out;
    }
    const float goal = goalOf(in);
    const Plan p = plan(in, goal);
    std::string said;
    if (!p.canTilt && !saidNoPitch_) {
        saidNoPitch_ = true;
        said = "Focus Camera: the camera's pitch is not known yet, turning only";
    }
    if (p.tooClose && p.dx == 0) {
        if (!saidTooClose_) {
            saidTooClose_ = true;
            said = "Focus Camera: " + who(in) + " is too close to show at " + share(goal, in.width) +
                   " of the width (no turn brings it there); waiting";
        }
    } else {
        saidTooClose_ = false;
    }
    if (p.dx == 0 && p.dy == 0) {
        out.inPlace = true;
        out.log = said;
        return out;
    }
    // A burst: the right button down now, the counts once the game turns the camera.
    burstX_ = p.dx;
    turner_.begin(in.now, p.dx, p.dy, in.cursorAtMiddle);
    beforeX_ = middleOf(in.targetBox);
    beforeGoal_ = goal;
    judgeYaw_ = p.dx != 0;
    byCamera_ = p.yawByCamera;
    expected_ = p.expected;
    out.right = true;
    char line[400];
    int at = std::snprintf(line, sizeof(line), "Focus Camera: %s at %s of the width, wants %s", who(in).c_str(),
                           share(beforeX_, in.width).c_str(), share(goal, in.width).c_str());
    if (p.dx != 0 && at > 0 && at < static_cast<int>(sizeof(line))) {
        at += std::snprintf(line + at, sizeof(line) - static_cast<std::size_t>(at), ": turning %s %d counts (%.1f degrees%s)",
                            wayOf(p.dx), std::abs(p.dx), std::abs(p.dx) * MouseCamera::kCountRadians * kDegrees,
                            p.yawByCamera ? "" : ", by how it moved last time");
    }
    if (p.dy != 0 && at > 0 && at < static_cast<int>(sizeof(line))) {
        std::snprintf(line + at, sizeof(line) - static_cast<std::size_t>(at),
                      "%s tilting %s %d counts (pitch %.1f -> %.1f degrees)", p.dx != 0 ? ";" : ":",
                      p.dy > 0 ? "down" : "up", std::abs(p.dy), in.pitch * kDegrees,
                      (in.pitch + p.dy * MouseCamera::kCountRadians) * kDegrees);
    }
    out.log = said.empty() ? std::string(line) : said + "\n" + line;
    return out;
}

} // namespace observex
