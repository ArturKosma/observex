#include "core/LookAround.h"

#include "core/MouseCamera.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

using namespace observex;

namespace {

constexpr double kDt = 1.0 / 144.0; // A UI frame.
constexpr double kDegrees = 57.29577951308232;
constexpr double kTop = MouseCamera::kTopUnits * MouseCamera::kUnitRadians; // 82 degrees down.

// A game turning its camera by the counts only while the right button is held and it took the press (the cursor put
// in the middle `warpDelay` after it), the pitch stopping at its limit.
struct Game {
    double yaw = 0.0;    // Radians, + turned right.
    double pitch = 0.17; // Radians down.
    bool takesPress = true;
    double warpDelay = 0.03;
    bool mouseFree = true;
    bool pitchPinned = false;
    bool pitchKnown = true;

    bool right = false;
    double pressedAt = -1.0;
    bool cameraMode = false;
    int presses = 0;
    int lostCounts = 0;
    bool cleared = false;
    bool done = false;
    double now = 0.0;
    std::vector<std::string> log;

    LookAround::Output step(LookAround& looker, int degrees) {
        LookAround::Input in;
        in.now = now;
        in.mouseFree = mouseFree;
        in.cursorAtMiddle = cameraMode;
        in.pitchKnown = pitchKnown;
        in.pitchPinned = pitchPinned;
        in.pitch = static_cast<float>(pitch);
        in.degrees = degrees;
        const LookAround::Output out = looker.step(in);
        cleared = cleared || out.clearMemory;
        done = done || out.done;
        if (out.right && !right) {
            right = true;
            pressedAt = now;
            ++presses;
        } else if (!out.right && right) {
            right = false;
            cameraMode = false;
        }
        if (out.dx != 0 || out.dy != 0) {
            if (right && cameraMode) {
                yaw += out.dx * MouseCamera::kCountRadians;
                pitch = std::clamp(pitch + out.dy * MouseCamera::kCountRadians, -0.5, kTop);
                if (pitch >= kTop) {
                    pitchPinned = true; // The counts saw it pushed past the limit.
                }
            } else {
                lostCounts += std::abs(out.dx) + std::abs(out.dy);
            }
        }
        if (right && !cameraMode && takesPress && now - pressedAt >= warpDelay) {
            cameraMode = true;
        }
        if (!out.log.empty()) {
            log.push_back(out.log);
        }
        now += kDt;
        return out;
    }
    void run(LookAround& looker, double seconds, int degrees = 360) {
        for (double end = now + seconds; now < end && !done;) {
            step(looker, degrees);
        }
    }
    bool said(const char* text) const {
        return std::ranges::any_of(log, [&](const std::string& line) { return line.find(text) != std::string::npos; });
    }
};

} // namespace

TEST_CASE("Look Around pins the pitch, then turns all the way round in one press, at 25 degrees down") {
    LookAround looker;
    Game game; // Pitch 10 degrees, never pinned.
    game.run(looker, 12.0);
    CHECK(game.done);
    CHECK(game.cleared); // The memory forgotten first.
    CHECK(game.said("first tilting down to the game's limit"));
    CHECK(game.yaw * kDegrees == doctest::Approx(360.0).epsilon(0.005));
    CHECK(game.pitch * kDegrees == doctest::Approx(25.0).epsilon(0.02));
    CHECK(game.pitchPinned);
    CHECK(game.presses == 2); // The pin's, then the turn's.
    CHECK(game.lostCounts == 0);
    CHECK_FALSE(game.right);
    CHECK(game.said("Look Around: turned 360 degrees in"));
}

TEST_CASE("Look Around turns at about 90 degrees a second, and tilts into 20-30 degrees on the way") {
    LookAround looker;
    Game game;
    game.pitchPinned = true; // Pinned before: no pin.
    game.pitch = 0.17;
    const double from = game.now;
    game.run(looker, 10.0);
    REQUIRE(game.done);
    CHECK(game.presses == 1);
    CHECK(game.now - from == doctest::Approx(4.0 + 0.3).epsilon(0.1)); // 4 s of turn, plus press and settle.
    CHECK(game.yaw * kDegrees == doctest::Approx(360.0).epsilon(0.005));
    CHECK(game.pitch * kDegrees == doctest::Approx(25.0).epsilon(0.02));
}

TEST_CASE("Look Around keeps a pitch already within 20-30 degrees, and does not tilt one not known") {
    {
        LookAround looker;
        Game game;
        game.pitchPinned = true;
        game.pitch = 0.47; // 27 degrees.
        game.run(looker, 10.0);
        CHECK(game.pitch == doctest::Approx(0.47));
    }
    {
        LookAround looker;
        Game game;
        game.pitchPinned = true; // The counts' pitch lost (a clip-like case): pinned, yet not known.
        game.pitchKnown = false;
        game.pitch = 0.17;
        game.run(looker, 10.0);
        CHECK(game.pitch == doctest::Approx(0.17));
        CHECK(game.yaw * kDegrees == doctest::Approx(360.0).epsilon(0.005));
    }
}

TEST_CASE("Look Around lets go when the mouse is wanted and goes on with the rest of the turn") {
    LookAround looker;
    Game game;
    game.pitchPinned = true;
    game.run(looker, 2.0); // About half way.
    REQUIRE(game.right);
    game.mouseFree = false;
    game.step(looker, 360);
    CHECK_FALSE(game.right);
    const double turned = game.yaw;
    game.run(looker, 1.0);
    CHECK(game.yaw == turned); // Nothing sent meanwhile.
    CHECK(game.said("the mouse is wanted"));
    game.mouseFree = true;
    game.run(looker, 10.0);
    CHECK(game.done);
    CHECK(game.presses == 2);
    CHECK(game.said("going on"));
    CHECK(game.yaw * kDegrees == doctest::Approx(360.0).epsilon(0.005));
    CHECK(game.lostCounts == 0);
}

TEST_CASE("Look Around tries a press the game does not take again, and is done after kPresses") {
    LookAround looker;
    Game game;
    game.pitchPinned = true;
    game.takesPress = false;
    game.run(looker, 5.0);
    CHECK(game.done);
    CHECK(game.presses == LookAround::kPresses);
    CHECK(game.said("trying again (press 2 of 3)"));
    CHECK(game.said("gave up, the game never took the right button"));
    CHECK(game.yaw == 0.0);
    CHECK_FALSE(game.right);
}

TEST_CASE("Look Around turns a part turn when set to") {
    LookAround looker;
    Game game;
    game.pitchPinned = true;
    game.pitch = 0.44;
    game.run(looker, 5.0, 90);
    CHECK(game.done);
    CHECK(game.yaw * kDegrees == doctest::Approx(90.0).epsilon(0.01));
    CHECK(game.said("turning 90 degrees right"));
}
