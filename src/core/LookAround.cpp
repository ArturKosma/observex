#include "core/LookAround.h"

#include "core/MouseCamera.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace observex {

namespace {

constexpr double kDegrees = 57.29577951308232;

int degreesOf(int counts) {
    return static_cast<int>(std::lround(std::abs(counts) * MouseCamera::kCountRadians * kDegrees));
}

} // namespace

void LookAround::start() {
    *this = LookAround();
}

void LookAround::beginTurn(const Input& in, Output& out) {
    if (stage_ == Stage::Pinning) {
        turner_.begin(in.now, 0, kPinCounts - pinSent_, in.cursorAtMiddle, CameraTurner::kCountsPerSecond);
        out.right = true;
        return;
    }
    // The tilt to kPitch, worked out once, at the turn's first press: from the limit when this task pinned it (the
    // camera is at it whether or not the counts saw it), else from the pitch known; none while it is not known.
    if (!tiltPlanned_) {
        tiltPlanned_ = true;
        const float top = static_cast<float>(MouseCamera::kTopUnits * MouseCamera::kUnitRadians);
        const bool known = pinnedHere_ || in.pitchKnown;
        const float pitch = pinnedHere_ ? top : in.pitch;
        if (known && std::fabs(pitch - kPitch) > kPitchSlack * (pinnedHere_ ? 0.0f : 1.0f)) {
            tiltLeft_ = static_cast<int>(std::lround((kPitch - pitch) / MouseCamera::kCountRadians));
        }
    }
    const double perSecond = kDegreesPerSecond / (MouseCamera::kCountRadians * kDegrees);
    turner_.begin(in.now, total_ - sent_, tiltLeft_, in.cursorAtMiddle, perSecond);
    out.right = true;
    if (sent_ > 0) {
        char line[160];
        std::snprintf(line, sizeof(line), "Look Around: going on, %d of %d degrees to go", degreesOf(total_ - sent_),
                      degreesOf(total_));
        out.log = line;
    }
}

LookAround::Output LookAround::step(const Input& in) {
    Output out;
    out.right = turner_.holds();
    char line[300];
    if (stage_ == Stage::Done) {
        out.done = true;
        return out;
    }
    if (stage_ == Stage::Start) {
        startedAt_ = in.now;
        out.clearMemory = true;
        total_ = static_cast<int>(std::lround(std::clamp(in.degrees, 1, 7200) / kDegrees / MouseCamera::kCountRadians));
        stage_ = in.pitchPinned ? Stage::Turning : Stage::Pinning;
        std::snprintf(line, sizeof(line),
                      "Look Around: turning %d degrees right to remember the monsters round (the memory forgotten "
                      "first)%s",
                      degreesOf(total_),
                      stage_ == Stage::Pinning ? "; first tilting down to the game's limit, so the pitch is known exactly"
                                               : "");
        out.log = line;
        return out; // The first press on the next step.
    }
    // Never stuck: kGiveUpSlack past the time its turns should take.
    const double perSecond = kDegreesPerSecond / (MouseCamera::kCountRadians * kDegrees);
    const double budget = kPinCounts / CameraTurner::kCountsPerSecond + total_ / perSecond + kGiveUpSlack;
    if (in.now - startedAt_ > budget) {
        turner_.letGo(in.now);
        out.right = false;
        stage_ = Stage::Done;
        out.done = true;
        std::snprintf(line, sizeof(line), "Look Around: gave up after %.1f s, turned %d of %d degrees",
                      in.now - startedAt_, degreesOf(sent_ + turner_.sentX()), degreesOf(total_));
        out.log = line;
        return out;
    }
    if (!in.mouseFree) { // The user's own button down (or another's): let go at once, go on once free.
        if (turner_.letGo(in.now)) {
            out.right = false;
            if (!saidWanted_) {
                saidWanted_ = true;
                std::snprintf(line, sizeof(line),
                              "Look Around: let go of the right button, the mouse is wanted (turned %d of %d "
                              "degrees); going on once it is free",
                              degreesOf(sent_ + turner_.sentX()), degreesOf(total_));
                out.log = line;
            }
        }
        return out;
    }
    saidWanted_ = false;
    if (!turner_.idle()) {
        const CameraTurner::Step s = turner_.step(in.now, in.cursorAtMiddle);
        out.dx = s.dx;
        out.dy = s.dy;
        out.right = s.right;
        if (s.event == CameraTurner::Event::NotTaken) {
            if (++presses_ >= kPresses) {
                stage_ = Stage::Done;
                out.done = true;
                std::snprintf(line, sizeof(line),
                              "Look Around: gave up, the game never took the right button for turning (%d presses)",
                              presses_);
            } else {
                std::snprintf(line, sizeof(line),
                              "Look Around: the game did not start turning the camera (the cursor never went to its "
                              "middle within 0.5 s); let go of the right button, trying again (press %d of %d)",
                              presses_ + 1, kPresses);
            }
            out.log = line;
        } else if (s.event == CameraTurner::Event::Settled) {
            // What went out of this press counts (all of it, or as far as it got before the mouse was wanted).
            if (stage_ == Stage::Pinning) {
                pinSent_ += turner_.sentY();
                if (pinSent_ >= kPinCounts) {
                    pinnedHere_ = true;
                    stage_ = Stage::Turning;
                    presses_ = 0;
                }
            } else {
                sent_ += turner_.sentX();
                tiltLeft_ -= turner_.sentY();
                if (sent_ >= total_) {
                    stage_ = Stage::Done;
                    out.done = true;
                    std::snprintf(line, sizeof(line), "Look Around: turned %d degrees in %.1f s", degreesOf(total_),
                                  in.now - startedAt_);
                    out.log = line;
                }
            }
        }
        return out;
    }
    beginTurn(in, out);
    return out;
}

} // namespace observex
