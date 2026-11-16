#include "core/MouseCamera.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace observex {

namespace {

constexpr float kTwoPi = 6.28318530718f;
constexpr unsigned kRightDown = 0x0004; // Raw Input's RI_MOUSE_RIGHT_BUTTON_DOWN.
constexpr unsigned kRightHeld = 2;

// How far off the pitch may be, so how far past a limit it must be pushed to be pinned by it (units): said by the
// follower (the ground, the depth network: up to 6 degrees off), the turns (a degree) or a limit before.
constexpr int kGuessedUnits = 30 * MouseCamera::kUnitsPerCount;
constexpr int kMeasuredUnits = MouseCamera::kPinUnits;
// Measured pitches kept, the middle of them taken.
constexpr std::size_t kPitchesKept = 31;

float middleOf(std::vector<float> values) {
    const auto half = values.begin() + static_cast<std::ptrdiff_t>(values.size() / 2);
    std::nth_element(values.begin(), half, values.end());
    return *half;
}

} // namespace

void MouseCamera::add(const ClipMouse& report) {
    if ((report.buttons & kRightDown) != 0) {
        pressed_ = true;
        cameraMode_ = false;
    }
    if ((report.held & kRightHeld) == 0) {
        pressed_ = false;
        cameraMode_ = false;
    }
    if (pressed_ && !cameraMode_) {
        // The game put the cursor in the middle: on it, or (the report's own move after it) near it, come there by a
        // jump farther than that move goes (a cursor dragged across the middle does not jump).
        const int reach = kWarpSlack + kWarpPixelsPerCount * (std::abs(report.dx) + std::abs(report.dy));
        const bool on = report.cursorX == middleX_ && report.cursorY == middleY_;
        const bool near = std::abs(report.cursorX - middleX_) <= reach && std::abs(report.cursorY - middleY_) <= reach;
        const bool jumped =
            cursorSeen_ && std::max(std::abs(report.cursorX - cursorX_), std::abs(report.cursorY - cursorY_)) > reach;
        cameraMode_ = on || (near && jumped);
    }
    cursorSeen_ = true;
    cursorX_ = report.cursorX;
    cursorY_ = report.cursorY;
    const bool turning = cameraMode_ && (report.dx != 0 || report.dy != 0);
    State state;
    state.timeMs = report.timeMs;
    const double before = states_.empty() ? report.timeMs - settings_.spreadMs : states_.back().timeMs;
    state.startMs = std::max(before, report.timeMs - settings_.spreadMs);
    if (turning) {
        turn_ += static_cast<long long>(report.dx) * kUnitsPerCount;
        const int wanted = pitch_ + report.dy * kUnitsPerCount;
        if (pitchKnown_) {
            const int clamped = std::clamp(wanted, settings_.bottomUnits, settings_.topUnits);
            if (clamped != wanted) {
                pinPush_ += std::abs(wanted - clamped);
                if (pinPush_ >= pinNeed_) {
                    pitchPinned_ = true;
                    pitchBy_ = PitchBy::Limit;
                    pinNeed_ = kMeasuredUnits;
                    pitches_.clear();
                }
            } else if (clamped != settings_.topUnits && clamped != settings_.bottomUnits) {
                pinPush_ = 0;
            }
            pitch_ = clamped;
        } else {
            pitch_ = wanted;
        }
        if (report.timeMs - lastTurningMs_ > kConeRestMs) {
            closeRest();
        }
        lastTurningMs_ = report.timeMs;
    }
    state.turn = turn_;
    state.pitch = pitch_;
    state.turning = turning;
    wheel_ += report.wheel;
    state.wheel = wheel_;
    states_.push_back(state);
    while (states_.size() > 2 && states_[1].timeMs < report.timeMs - settings_.keepMs) {
        base_ = states_.front();
        states_.pop_front();
    }
}

void MouseCamera::stateAt(double timeMs, long long& turn, int& pitch) const {
    const auto after = std::upper_bound(states_.begin(), states_.end(), timeMs,
                                        [](double time, const State& state) { return time < state.timeMs; });
    if (after == states_.end()) {
        turn = states_.back().turn;
        pitch = states_.back().pitch;
        return;
    }
    const State& from = after == states_.begin() ? base_ : *(after - 1);
    turn = from.turn;
    pitch = from.pitch;
    if (timeMs > after->startMs && after->timeMs > after->startMs) {
        const double share = std::min((timeMs - after->startMs) / (after->timeMs - after->startMs), 1.0);
        turn += std::llround(share * static_cast<double>(after->turn - from.turn));
        pitch += static_cast<int>(std::lround(share * static_cast<double>(after->pitch - from.pitch)));
    }
}

double MouseCamera::lastTurningBefore(double timeMs) const {
    if (lastTurningMs_ <= timeMs) {
        return lastTurningMs_;
    }
    auto it = std::upper_bound(states_.begin(), states_.end(), timeMs,
                               [](double time, const State& state) { return time < state.timeMs; });
    while (it != states_.begin()) {
        --it;
        if (it->turning) {
            return it->timeMs;
        }
    }
    return -1e300;
}

long long MouseCamera::wheelBy(double timeMs) const {
    const auto after = std::upper_bound(states_.begin(), states_.end(), timeMs,
                                        [](double time, const State& state) { return time < state.timeMs; });
    return after == states_.begin() ? base_.wheel : (after - 1)->wheel;
}

MouseCamera::View MouseCamera::at(double timeMs) const {
    View view;
    if (states_.empty()) {
        return view;
    }
    view.known = true;
    stateAt(timeMs - settings_.lagMs, view.turnUnits, view.pitchUnits);
    view.turned = static_cast<double>(view.turnUnits) * kUnitRadians;
    view.yawKnown = yawKnown_;
    view.yaw = std::remainder(static_cast<float>(std::remainder(view.turned, 2.0 * 3.14159265358979323846)) +
                                  yawOffset_,
                              kTwoPi);
    view.pitchKnown = pitchKnown_;
    view.pitchPinned = pitchPinned_;
    view.pitchBy = pitchBy_;
    view.pitch = static_cast<float>(view.pitchUnits * kUnitRadians);
    view.turning = lastTurningBefore(timeMs) >= timeMs - kConeRestMs;
    view.wheelNotches = static_cast<double>(wheelBy(timeMs - settings_.wheelLagMs)) / kWheelPerNotch;
    return view;
}

void MouseCamera::shiftPitch(int units) {
    pitch_ += units;
    base_.pitch += units;
    for (State& state : states_) {
        state.pitch += units;
    }
}

void MouseCamera::closeRest() {
    if (restCones_.size() >= 3) {
        rests_.push_back(middleOf(restCones_));
        if (rests_.size() > static_cast<std::size_t>(kRests)) {
            rests_.pop_front();
        }
    }
    restCones_.clear();
}

void MouseCamera::heading(double timeMs, float heading) {
    if (states_.empty() || lastTurningBefore(timeMs) >= timeMs - kConeRestMs) {
        return;
    }
    const auto turned = static_cast<float>(std::remainder(at(timeMs).turned, 2.0 * 3.14159265358979323846));
    const float offset = std::remainder(heading - turned, kTwoPi);
    if (!yawKnown_) {
        yawKnown_ = true;
        yawOffset_ = offset;
    }
    // Kept unwrapped round the offset, so their middle is.
    yawOff_ = std::remainder(offset - yawOffset_, kTwoPi);
    restCones_.push_back(yawOffset_ + yawOff_);
    if (restCones_.size() < 3) {
        return;
    }
    const float rest = middleOf(restCones_);
    if (restCones_.size() >= 5 && std::fabs(rest - yawOffset_) > kYawJump) {
        rests_.clear(); // Turned by something else: the rests before say nothing now.
    }
    std::vector<float> all(rests_.begin(), rests_.end());
    all.push_back(rest);
    yawOffset_ = middleOf(all);
}

void MouseCamera::pitched(double timeMs, float pitch) {
    // Beyond the limits it is wrong (the first pair of clip 21-46-09 said 88 degrees).
    if (states_.empty() || pitch > static_cast<float>(settings_.topUnits * kUnitRadians) + kPitchBeyond ||
        pitch < static_cast<float>(settings_.bottomUnits * kUnitRadians) - kPitchBeyond) {
        return;
    }
    const View view = at(timeMs);
    const float off = pitch - view.pitch;
    if (!pitchKnown_) { // The middle of the first few (the counts' pitch until then only relative).
        pitches_.push_back(off);
        if (pitches_.size() >= static_cast<std::size_t>(kPitchFirst)) {
            shiftPitch(static_cast<int>(std::lround(middleOf(pitches_) / kUnitRadians)));
            pitches_.clear();
            pitchKnown_ = true;
            pitchBy_ = PitchBy::Turns;
            pinNeed_ = kMeasuredUnits;
        }
        return;
    }
    pitches_.push_back(off);
    if (pitches_.size() > kPitchesKept) {
        pitches_.erase(pitches_.begin());
    }
    pitchOff_ = middleOf(pitches_);
    const bool guessed = pitchBy_ == PitchBy::Follower;
    const std::size_t need =
        static_cast<std::size_t>(pitchPinned_ ? kPitchChecks : guessed ? kPitchFirst : kPitchTakes);
    if (pitches_.size() < need || (pitchPinned_ && std::fabs(pitchOff_) < kPitchOff)) {
        return;
    }
    const int units = static_cast<int>(std::lround(pitchOff_ / kUnitRadians));
    if (std::abs(units) < kUnitsPerCount / 2) { // Within half a count: as the counts have it.
        if (guessed) {
            pitchBy_ = PitchBy::Turns;
            pinNeed_ = kMeasuredUnits;
        }
        return;
    }
    shiftPitch(units);
    for (float& said : pitches_) {
        said -= static_cast<float>(units * kUnitRadians);
    }
    pitchOff_ -= static_cast<float>(units * kUnitRadians);
    if (pitchPinned_) { // The counts lost some: measured again from here.
        pitchPinned_ = false;
        pitches_.clear();
    }
    pitchBy_ = PitchBy::Turns;
    pinNeed_ = kMeasuredUnits;
}

void MouseCamera::guessPitch(double timeMs, float pitch) {
    if (pitchKnown_ || states_.empty()) {
        return;
    }
    shiftPitch(static_cast<int>(std::lround((pitch - at(timeMs).pitch) / kUnitRadians)));
    pitchKnown_ = true;
    pitchBy_ = PitchBy::Follower;
    pinNeed_ = kGuessedUnits;
}

void MouseCamera::clear() {
    const Settings settings = settings_;
    const int x = middleX_;
    const int y = middleY_;
    *this = MouseCamera(settings);
    middle(x, y);
}

} // namespace observex
