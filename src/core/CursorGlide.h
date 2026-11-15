#pragma once

#include <algorithm>
#include <cmath>

namespace observex {

// Eases the cursor onto a point, one step a UI frame, read back from the real cursor each time (so the pointer's
// acceleration in Windows does not matter): faster the farther (kEase x the distance a second, kLeastSpeed to
// kTopSpeed), its speed changing at most kAcceleration a second and slow enough to brake at that by the point, so it
// never stops at speed. Select Target onto a monster (core/TargetSelect) and the Skills tab's Refresh onto a skill
// (core/SkillRefresh).
class CursorGlide {
public:
    static constexpr double kTopSpeed = 7000.0;      // Pixels a second.
    static constexpr double kEase = 12.0;            // Speed from distance: 12 x the pixels left a second...
    static constexpr double kLeastSpeed = 500.0;     // ...at least this...
    static constexpr double kAcceleration = 40000.0; // ...reached at most this fast (pixels a second, a second).

    struct Move {
        int dx = 0;
        int dy = 0;
    };

    // The move this step from the cursor towards a point `toX`, `toY` pixels away, `dt` seconds after the step before.
    Move step(double toX, double toY, double dt) {
        const double distance = std::hypot(toX, toY);
        if (distance <= 1.0) {
            speed_ = 0.0;
            return {};
        }
        const double braking = std::sqrt(2.0 * kAcceleration * distance);
        const double wanted = std::clamp(std::min(distance * kEase, braking), kLeastSpeed, kTopSpeed);
        const double change = kAcceleration * dt;
        speed_ = std::clamp(wanted, speed_ - change, speed_ + change);
        const double moved = std::min(speed_ * dt, distance);
        return {static_cast<int>(std::lround(toX / distance * moved)),
                static_cast<int>(std::lround(toY / distance * moved))};
    }
    // Starts from rest (a new point, or the cursor held still).
    void stop() { speed_ = 0.0; }

private:
    double speed_ = 0.0;
};

} // namespace observex
