#include "core/MouseCamera.h"

#include <doctest/doctest.h>

#include <cmath>
#include <random>

using namespace observex;

namespace {

constexpr unsigned kRightDown = 0x0004;
constexpr unsigned kRightUp = 0x0008;

ClipMouse report(double timeMs, int dx, int dy, unsigned held, unsigned buttons = 0, int x = 1280, int y = 720) {
    ClipMouse mouse;
    mouse.timeMs = timeMs;
    mouse.dx = dx;
    mouse.dy = dy;
    mouse.held = held;
    mouse.buttons = buttons;
    mouse.cursorX = x;
    mouse.cursorY = y;
    return mouse;
}

// Presses the right button at `timeMs` away from the middle, and the game takes it (the cursor in the middle) 8 ms on.
double startTurning(MouseCamera& camera, double timeMs) {
    camera.add(report(timeMs, 0, 0, 2, kRightDown, 900, 600));
    camera.add(report(timeMs + 8.0, 0, 0, 2));
    return timeMs + 8.0;
}

} // namespace

TEST_CASE("mouse camera: only counts in the game's camera mode turn it") {
    MouseCamera camera;
    camera.middle(1280, 720);
    camera.add(report(0.0, 50, 0, 0)); // Not held.
    camera.add(report(8.0, 0, 0, 2, kRightDown, 900, 600));
    camera.add(report(16.0, 30, 0, 2, 0, 930, 600)); // Held, but the game has not put the cursor in the middle.
    CHECK(camera.at(1000.0).turnUnits == 0);
    camera.add(report(24.0, 7, 0, 2, 0, 1280, 720)); // Now it has: this one turns.
    camera.add(report(32.0, 3, 0, 2, 0, 1283, 720));
    CHECK(camera.at(1000.0).turnUnits == 10 * MouseCamera::kUnitsPerCount);
    camera.add(report(40.0, 5, 0, 0, kRightUp, 1288, 720)); // Let go: not counted.
    camera.add(report(48.0, 5, 0, 0, 0, 1293, 720));
    CHECK(camera.at(1000.0).turnUnits == 10 * MouseCamera::kUnitsPerCount);
    // A press the game never takes (the cursor never goes to the middle): its counts turn nothing.
    camera.add(report(56.0, 0, 0, 2, kRightDown, 1293, 720));
    camera.add(report(64.0, 40, 0, 2, 0, 1333, 720));
    CHECK(camera.at(1000.0).turnUnits == 10 * MouseCamera::kUnitsPerCount);
}

TEST_CASE("mouse camera: a frame shows the reports up to the lag before it, each spread over the time before it") {
    MouseCamera camera;
    camera.middle(1280, 720);
    const double t = startTurning(camera, 0.0); // 8
    camera.add(report(t + 8.0, 10, 0, 2));       // At 16, its counts taken from 8 (the report before) on.
    const double lag = MouseCamera::kLagMs;
    CHECK(camera.at(t + lag).turnUnits == 0);
    CHECK(camera.at(t + 4.0 + lag).turnUnits == 5 * MouseCamera::kUnitsPerCount);
    CHECK(camera.at(t + 8.0 + lag).turnUnits == 10 * MouseCamera::kUnitsPerCount);
    CHECK(camera.at(t + 1000.0).turned == doctest::Approx(10 * MouseCamera::kCountRadians));
    // After a pause the spread is at most kSpreadMs.
    camera.add(report(t + 1008.0, -4, 0, 2));
    CHECK(camera.at(t + 1008.0 - MouseCamera::kSpreadMs + lag).turnUnits == 10 * MouseCamera::kUnitsPerCount);
    CHECK(camera.at(t + 1008.0 + lag).turnUnits == 6 * MouseCamera::kUnitsPerCount);
}

TEST_CASE("mouse camera: whole counts, no drift however long it turns") {
    MouseCamera camera;
    camera.middle(1280, 720);
    double t = startTurning(camera, 0.0);
    std::mt19937 random(7);
    std::uniform_int_distribution<int> step(-40, 40);
    long long sum = 0;
    for (int i = 0; i < 100000; ++i) {
        t += 8.0;
        const int dx = step(random);
        sum += dx;
        camera.add(report(t, dx, 0, 2));
    }
    camera.add(report(t + 8.0, static_cast<int>(-sum), 0, 2)); // Back where it started.
    const MouseCamera::View view = camera.at(t + 1000.0);
    CHECK(view.turnUnits == 0);
    CHECK(view.turned == 0.0);
}

TEST_CASE("mouse camera: the pitch stops at the top limit and is pinned by it") {
    MouseCamera camera;
    camera.middle(1280, 720);
    double t = startTurning(camera, 0.0);
    camera.guessPitch(t, 0.5f); // The follower's, some degrees off.
    MouseCamera::View view = camera.at(t + 100.0);
    CHECK(view.pitchKnown);
    CHECK(view.pitchBy == MouseCamera::PitchBy::Follower);
    CHECK(view.pitch == doctest::Approx(0.5f).epsilon(0.001));
    // Up past the limit, more than the guess may be off: exact there.
    for (int i = 0; i < 60; ++i) {
        t += 8.0;
        camera.add(report(t, 0, 20, 2));
    }
    view = camera.at(t + 100.0);
    CHECK(view.pitchUnits == MouseCamera::kTopUnits);
    CHECK(view.pitchPinned);
    CHECK(view.pitchBy == MouseCamera::PitchBy::Limit);
    // Back down by 100 counts: 100 counts under the limit.
    for (int i = 0; i < 10; ++i) {
        t += 8.0;
        camera.add(report(t, 0, -10, 2));
    }
    CHECK(camera.at(t + 100.0).pitchUnits == MouseCamera::kTopUnits - 100 * MouseCamera::kUnitsPerCount);
}

TEST_CASE("mouse camera: a guessed pitch pushed only a little past the limit is not pinned") {
    MouseCamera camera;
    camera.middle(1280, 720);
    double t = startTurning(camera, 0.0);
    camera.guessPitch(t, 1.40f); // 80.2 degrees: 1.8 under the limit.
    for (int i = 0; i < 3; ++i) {
        t += 8.0;
        camera.add(report(t, 0, 5, 2)); // 15 counts up: 3 degrees, so 1.2 past it (it may be 6 off).
    }
    const MouseCamera::View view = camera.at(t + 100.0);
    CHECK(view.pitchUnits == MouseCamera::kTopUnits);
    CHECK_FALSE(view.pitchPinned);
}

TEST_CASE("mouse camera: the turns' pitch anchors it, by the middle of what several say") {
    MouseCamera camera;
    camera.middle(1280, 720);
    double t = startTurning(camera, 0.0);
    camera.pitched(t, 1.54f); // Past the top limit (88 degrees): wrong, not taken.
    camera.pitched(t, 0.30f); // The first few say it.
    camera.pitched(t, 0.31f);
    CHECK_FALSE(camera.at(t + 100.0).pitchKnown);
    camera.pitched(t, 0.30f);
    CHECK(camera.at(t + 100.0).pitch == doctest::Approx(0.30f).epsilon(0.002));
    CHECK(camera.at(t + 100.0).pitchBy == MouseCamera::PitchBy::Turns);
    // Then they say a degree more: taken once enough of them do (one far off among them).
    const float said[] = {0.3175f, 0.3170f, 0.40f, 0.3180f, 0.3172f};
    for (const float pitch : said) {
        t += 8.0;
        camera.add(report(t, 0, 0, 2));
        camera.pitched(t, pitch);
    }
    CHECK(camera.at(t + 100.0).pitch == doctest::Approx(0.3175f).epsilon(0.002));
}

TEST_CASE("mouse camera: the minimap's cone anchors the yaw while the camera rests") {
    MouseCamera camera;
    camera.middle(1280, 720);
    double t = startTurning(camera, 0.0);
    t += 200.0;
    camera.add(report(t, 0, 0, 2));
    camera.heading(t, 1.0f);
    MouseCamera::View view = camera.at(t);
    CHECK(view.yawKnown);
    CHECK(view.yaw == doctest::Approx(1.0f));
    // Turned 100 counts: the yaw goes on by them, and a cone seen while turning is not taken.
    for (int i = 0; i < 10; ++i) {
        t += 8.0;
        camera.add(report(t, 10, 0, 2));
        camera.heading(t, 0.0f);
    }
    t += 200.0;
    camera.add(report(t, 0, 0, 2));
    view = camera.at(t);
    CHECK(view.yaw == doctest::Approx(1.0 + 100 * MouseCamera::kCountRadians).epsilon(1e-4));
    // At rest the cone, half a degree off as it reads, is this rest's (no rest before said otherwise); then cones 5
    // degrees off (turned by something else) move it there once most of the rest's say so.
    for (int i = 0; i < 3; ++i) {
        camera.heading(t, static_cast<float>(1.0 + 100 * MouseCamera::kCountRadians + 0.0087));
    }
    CHECK(std::fabs(camera.at(t).yaw - static_cast<float>(1.0 + 100 * MouseCamera::kCountRadians)) < 0.009f);
    const auto turned = static_cast<float>(1.0 + 100 * MouseCamera::kCountRadians + 0.087);
    for (int i = 0; i < 5; ++i) {
        t += 30.0;
        camera.add(report(t, 0, 0, 2));
        camera.heading(t, turned);
    }
    CHECK(camera.at(t).yaw == doctest::Approx(turned).epsilon(1e-3));
}

TEST_CASE("mouse camera: the Camera card in degrees, the minimap's yaw from 0 to 360") {
    constexpr double kDegrees = 57.29577951308232;
    MouseCamera::View view;
    CHECK_FALSE(cameraOf(view, 0.5f, 0.5f).known); // No report yet: no card.
    view.known = true;
    view.turned = -2.0 * 3.14159265358979323846 - 0.5; // Only turned: how far, not wrapped.
    CameraShown card = cameraOf(view, 0.0f, 0.0f);
    CHECK(card.known);
    CHECK_FALSE(card.yawKnown);
    CHECK_FALSE(card.pitchKnown);
    CHECK(card.yaw == doctest::Approx(-360.0 - 0.5 * kDegrees));
    CHECK(card.pitch == 0.0f);
    view.yawKnown = true;
    view.yaw = -0.5f; // West of north: 331.4 degrees.
    view.pitchKnown = true;
    view.pitch = 0.3f;
    view.pitchBy = MouseCamera::PitchBy::Turns;
    card = cameraOf(view, 0.01f, -0.02f);
    CHECK(card.yaw == doctest::Approx(360.0 - 0.5 * kDegrees));
    CHECK(card.pitch == doctest::Approx(0.3 * kDegrees));
    CHECK(card.pitchBy == 2);
    CHECK(card.coneOff == doctest::Approx(0.01 * kDegrees));
    CHECK(card.turnsOff == doctest::Approx(-0.02 * kDegrees));
    view.yaw = 3.14159265f; // South.
    CHECK(cameraOf(view, 0.0f, 0.0f).yaw == doctest::Approx(180.0).epsilon(1e-5));
}

TEST_CASE("mouse camera: the cursor put in the middle and moved on by the same report still starts turning") {
    MouseCamera camera;
    camera.middle(1280, 720);
    // The press at (1447, 708); the game's warp and a move of (-2, 4) come as one report at (1279, 723), as on clip
    // 2026-11-15_23-34-30, and the cursor never sits on the middle exactly after.
    camera.add(report(0.0, 0, 0, 2, kRightDown, 1447, 708));
    camera.add(report(3.0, 3, 0, 2, 0, 1450, 708)); // Not yet taken: still where it was.
    camera.add(report(14.0, -2, 4, 2, 0, 1279, 723));
    camera.add(report(22.0, 10, 0, 2, 0, 1300, 723));
    CHECK(camera.at(1000.0).turnUnits == 8 * MouseCamera::kUnitsPerCount);
    // A cursor dragged across the middle (the game never took the press) does not jump there: nothing turns.
    MouseCamera dragged;
    dragged.middle(1280, 720);
    dragged.add(report(0.0, 0, 0, 2, kRightDown, 1200, 720));
    for (int i = 1; i <= 20; ++i) {
        dragged.add(report(8.0 * i, 5, 0, 2, 0, 1200 + 8 * i, 721)); // Through (1280, 721): 1 pixel off the middle.
    }
    CHECK(dragged.at(1000.0).turnUnits == 0);
}

TEST_CASE("mouse camera: the wheel's notches the game has taken by a frame") {
    constexpr double kLag = MouseCamera::kWheelLagMs;
    MouseCamera camera;
    camera.middle(1280, 720);
    CHECK_FALSE(camera.at(0.0).known); // Nothing came yet.
    camera.add(report(10.0, 3, 0, 0));
    CHECK(camera.at(20.0).known);
    CHECK(camera.at(20.0).wheelNotches == 0.0); // Watched, never turned.
    ClipMouse notch = report(30.0, 0, 0, 0);
    notch.wheel = 120; // One notch away from the user: out.
    camera.add(notch);
    camera.add(report(40.0, 2, 0, 0));
    CHECK(camera.at(1000.0).wheelNotches == doctest::Approx(1.0));
    CHECK(camera.at(30.0 + kLag - 1.0).wheelNotches == 0.0); // The game shows it only kWheelLagMs on.
    CHECK(camera.at(30.0 + kLag).wheelNotches == doctest::Approx(1.0));
    notch.timeMs = 50.0;
    notch.wheel = -240; // Two in.
    camera.add(notch);
    CHECK(camera.at(1000.0).wheelNotches == doctest::Approx(-1.0));
    CHECK(camera.at(45.0 + kLag).wheelNotches == doctest::Approx(1.0));
    GroundHints hints;
    mouseHints(camera.at(1000.0), hints);
    CHECK(hints.wheelWatched);
    CHECK(hints.wheelNotches == doctest::Approx(-1.0));
    // Reports no longer kept still count (summed into the oldest kept).
    MouseCamera::Settings settings;
    settings.keepMs = 100.0;
    MouseCamera brief(settings);
    ClipMouse spun = report(0.0, 0, 0, 0);
    spun.wheel = 120;
    for (int i = 0; i < 10; ++i) {
        spun.timeMs = 50.0 * i;
        brief.add(spun);
    }
    CHECK(brief.at(1000.0).wheelNotches == doctest::Approx(10.0));
}
