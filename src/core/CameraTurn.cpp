#include "core/CameraTurn.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace observex {

namespace {

// The share of a glide done at `u` (0 to 1) of its time, minimum jerk: it starts and ends at rest, with no jolt.
double glided(double u) {
    u = std::clamp(u, 0.0, 1.0);
    return u * u * u * (10.0 + u * (-15.0 + 6.0 * u));
}

} // namespace

void CameraTurner::begin(double now, int dx, int dy, bool cursorAtMiddle, double countsPerSecond) {
    phase_ = Phase::Pressing;
    phaseAt_ = now;
    middleBefore_ = cursorAtMiddle;
    taken_ = false;
    turnX_ = dx;
    turnY_ = dy;
    sentX_ = 0;
    sentY_ = 0;
    glideSeconds_ = std::max(kMinSeconds, std::max(std::abs(dx), std::abs(dy)) / std::max(countsPerSecond, 1.0));
}

bool CameraTurner::letGo(double now) {
    if (!holds()) {
        return false;
    }
    phase_ = Phase::Settling;
    phaseAt_ = now;
    return true;
}

CameraTurner::Step CameraTurner::step(double now, bool cursorAtMiddle) {
    Step out;
    out.right = holds();
    switch (phase_) {
    case Phase::Idle:
        break;
    case Phase::Pressing:
        // The game turns the camera only once it put the cursor in its window's middle (core/MouseCamera).
        if (cursorAtMiddle && now > phaseAt_ && (!middleBefore_ || now - phaseAt_ >= kSameMiddleWait)) {
            phase_ = Phase::Turning;
            phaseAt_ = now;
            taken_ = true;
        } else if (now - phaseAt_ >= kWarpWait) {
            letGo(now);
            out.right = false;
            out.event = Event::NotTaken;
        }
        break;
    case Phase::Turning: {
        const double u = glideSeconds_ > 0.0 ? (now - phaseAt_) / glideSeconds_ : 1.0;
        const double done = glided(u);
        const int x = static_cast<int>(std::lround(turnX_ * done));
        const int y = static_cast<int>(std::lround(turnY_ * done));
        out.dx = x - sentX_;
        out.dy = y - sentY_;
        sentX_ = x;
        sentY_ = y;
        if (u >= 1.0) {
            phase_ = Phase::Holding;
            phaseAt_ = now;
        }
        break;
    }
    case Phase::Holding:
        if (now - phaseAt_ >= kHoldAfter) {
            letGo(now); // The game puts the cursor back where it was.
            out.right = false;
        }
        break;
    case Phase::Settling:
        if (now - phaseAt_ >= kSettle) {
            phase_ = Phase::Idle;
            out.event = Event::Settled;
        }
        break;
    }
    return out;
}

} // namespace observex
