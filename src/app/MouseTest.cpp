#include "app/MouseTest.h"

#include "engine/PicoController.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace observex {
namespace {

// How far the cursor may stray from our steering before we take it as the user moving the mouse.
constexpr double kStillTol = 30.0; // While we hold still (countdown, resting): any move this far is the user.
constexpr double kPerpTol = 55.0;  // While moving: a sideways move this far (across our command) is the user.
constexpr double kBackTol = 35.0;  // While moving: a move this far against our command is the user.

constexpr double kArrivePx = 6.0;    // This close to a monster counts as arrived.
constexpr double kMinSpeed = 350.0;  // Sweep speed floor (px/s), so the last pixels still close.
constexpr double kMaxSpeed = 2600.0; // Sweep speed cap (px/s).
constexpr double kAccel = 12000.0;   // How fast the speed ramps (px/s^2), for a smooth start and stop.
constexpr float kRestSeconds = 2.0f;
constexpr float kCountdownSeconds = 3.0f;

const MouseTarget* findById(const std::vector<MouseTarget>& monsters, std::uint32_t id) {
    for (const MouseTarget& m : monsters) {
        if (m.id == id) {
            return &m;
        }
    }
    return nullptr;
}

} // namespace

void MouseTest::begin(PicoController& controller) {
    if (active()) {
        return;
    }
    controller_ = &controller;
    phase_ = Phase::Countdown;
    countdownLeft_ = kCountdownSeconds;
    targetId_ = 0;
    lastId_ = 0;
    restLeft_ = 0.0f;
    speed_ = 0.0;
    havePrev_ = false;
    lastCmdX_ = 0;
    lastCmdY_ = 0;
    status_ = "Counting down\xE2\x80\xA6";
}

void MouseTest::stop() {
    phase_ = Phase::Idle;
    targetId_ = 0;
    speed_ = 0.0;
    lastCmdX_ = 0;
    lastCmdY_ = 0;
}

void MouseTest::pickTarget(const std::vector<MouseTarget>& monsters, int cursorX, int cursorY) {
    targetId_ = 0;
    const MouseTarget* nearest = nullptr;
    double nearestD = 0.0;
    const MouseTarget* nearestOther = nullptr; // Not the one we just rested on.
    double nearestOtherD = 0.0;
    for (const MouseTarget& m : monsters) {
        const double d = std::hypot(static_cast<double>(m.x - cursorX), static_cast<double>(m.y - cursorY));
        if (nearest == nullptr || d < nearestD) {
            nearest = &m;
            nearestD = d;
        }
        if (m.id != lastId_ && (nearestOther == nullptr || d < nearestOtherD)) {
            nearestOther = &m;
            nearestOtherD = d;
        }
    }
    const MouseTarget* pick = nearestOther != nullptr ? nearestOther : nearest;
    if (pick != nullptr) {
        targetId_ = pick->id;
        targetX_ = pick->x;
        targetY_ = pick->y;
        speed_ = 0.0; // A fresh leg ramps up from a stop.
    }
}

void MouseTest::tick(const std::vector<MouseTarget>& monsters, int cursorX, int cursorY, double dtMs) {
    if (phase_ == Phase::Idle) {
        return;
    }
    const double dt = std::max(0.0, dtMs) / 1000.0;

    // Did the user take over? Compare the cursor's move since last frame with the move we commanded then.
    if (havePrev_) {
        const double mx = static_cast<double>(cursorX - prevCursorX_);
        const double my = static_cast<double>(cursorY - prevCursorY_);
        const double cmdLen = std::hypot(static_cast<double>(lastCmdX_), static_cast<double>(lastCmdY_));
        bool user = false;
        if (cmdLen < 2.0) {
            user = std::hypot(mx, my) > kStillTol; // We held still but the cursor moved: the user.
        } else {
            const double ux = lastCmdX_ / cmdLen;
            const double uy = lastCmdY_ / cmdLen;
            const double along = mx * ux + my * uy;          // Along our command (acceleration only scales this).
            const double perp = std::abs(-mx * uy + my * ux); // Across it: our moves have none, the user's do.
            user = perp > kPerpTol || along < -kBackTol;
        }
        if (user) {
            status_ = "Stopped: you moved the mouse.";
            stop();
            return;
        }
    }

    int cmdX = 0;
    int cmdY = 0;

    if (phase_ == Phase::Countdown) {
        countdownLeft_ -= static_cast<float>(dt);
        if (countdownLeft_ > 0.0f) {
            char buf[48];
            std::snprintf(buf, sizeof(buf), "Starting in %.1f s\xE2\x80\xA6", static_cast<double>(countdownLeft_));
            status_ = buf;
        } else {
            countdownLeft_ = 0.0f;
            phase_ = Phase::Moving;
            pickTarget(monsters, cursorX, cursorY);
        }
    } else if (phase_ == Phase::Resting) {
        restLeft_ -= static_cast<float>(dt);
        if (restLeft_ > 0.0f) {
            status_ = "On a monster\xE2\x80\xA6";
        } else {
            phase_ = Phase::Moving;
            targetId_ = 0; // Choose the next monster below.
        }
    }

    if (phase_ == Phase::Moving) {
        // Keep aiming at the same monster as it moves; if it is gone, choose another.
        if (targetId_ != 0) {
            if (const MouseTarget* m = findById(monsters, targetId_); m != nullptr) {
                targetX_ = m->x;
                targetY_ = m->y;
            } else {
                targetId_ = 0;
            }
        }
        if (targetId_ == 0) {
            pickTarget(monsters, cursorX, cursorY);
        }
        if (targetId_ == 0) {
            status_ = "Waiting for a monster\xE2\x80\xA6";
        } else {
            const double toX = targetX_ - cursorX;
            const double toY = targetY_ - cursorY;
            const double dist = std::hypot(toX, toY);
            if (dist <= kArrivePx) {
                phase_ = Phase::Resting;
                restLeft_ = kRestSeconds;
                lastId_ = targetId_;
                speed_ = 0.0;
                status_ = "On a monster\xE2\x80\xA6";
            } else {
                const double desired = std::clamp(dist * 7.0, kMinSpeed, kMaxSpeed); // Ease out near the monster.
                const double change = kAccel * dt;                                   // Ramp, for a smooth start.
                speed_ = std::clamp(desired, speed_ - change, speed_ + change);
                const double stepLen = std::min(speed_ * dt, dist);
                cmdX = static_cast<int>(std::lround(toX / dist * stepLen));
                cmdY = static_cast<int>(std::lround(toY / dist * stepLen));
                status_ = "Moving to a monster\xE2\x80\xA6";
            }
        }
    }

    if ((cmdX != 0 || cmdY != 0) && controller_ != nullptr) {
        if (!controller_->moveMouse(cmdX, cmdY)) {
            status_ = "Stopped: the controller link was lost.";
            stop();
            return;
        }
    }
    lastCmdX_ = cmdX;
    lastCmdY_ = cmdY;
    prevCursorX_ = cursorX;
    prevCursorY_ = cursorY;
    havePrev_ = true;
}

} // namespace observex
