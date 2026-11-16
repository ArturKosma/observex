#include "core/CameraFocus.h"

#include "core/Ground.h"
#include "core/MouseCamera.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

using namespace observex;

namespace {

constexpr double kDt = 1.0 / 144.0; // A UI frame.
constexpr float kWidth = 2560.0f;
constexpr float kHeight = 1440.0f;
constexpr double kCount = MouseCamera::kCountRadians;

// A game whose camera circles the own character as Lineage 2's does (core/Ground's camera), turned and tilted by the
// focuser's counts only while the right button is held and the game took it for turning (it puts the cursor in the
// middle `warpDelay` after the press, or never); the focuser sees it `lag` late, as the screen, the recogniser and the
// tracker show it.
struct Game {
    double yaw = 0.0;   // Radians, + turned right.
    double pitch = 0.40; // Radians down.
    float distance = 8.0f;
    double east = 2.0;  // The target's feet from the own character's feet, own heights (north: away at yaw 0).
    double north = 6.0;
    double walkEast = 0.0; // Own heights a second.
    double walkNorth = 0.0;
    float feetLift = 0.0f; // The target's box ends this many pixels above its feet (a box not down to them).
    int turnSign = 1;      // -1: the game turns the camera the other way than the counts say.
    bool takesPress = true;
    double warpDelay = 0.03;
    double lag = 0.04;
    bool knowsPitch = true;
    bool mouseFree = true;

    bool right = false;
    double pressedAt = -1.0;
    bool cameraMode = false;
    int presses = 0;
    int lostCounts = 0; // Counts sent while the game was not turning the camera.
    int counts = 0;     // All counts sent.
    std::vector<std::string> log;

    struct Past {
        double t, yaw, pitch, east, north;
    };
    std::vector<Past> past;
    double now = 0.0;

    const Past& seenAt(double t) const {
        const Past* seen = &past.front();
        for (const Past& p : past) {
            if (p.t <= t) {
                seen = &p;
            }
        }
        return *seen;
    }

    // The target's box (feet and head, a monster one own height tall) and the own character's, as `p` shows them.
    void boxes(const Past& p, CameraFocuser::Input& in) const {
        const GroundCamera camera = groundCamera(static_cast<float>(p.pitch), distance, kWidth, kHeight);
        const double c = std::cos(p.yaw);
        const double s = std::sin(p.yaw);
        const auto across = static_cast<float>(p.east * c - p.north * s);
        const auto away = static_cast<float>(p.east * s + p.north * c);
        const GroundPoint feet = groundToScreen(camera, across, away);
        const GroundPoint head = worldToScreen(camera, across, away, 1.0f);
        in.target = feet.ahead && head.ahead;
        in.targetId = 7;
        const float bottom = feet.y - feetLift;
        const float half = (feet.y - head.y) * 0.3f;
        in.targetBox = {feet.x - half, head.y, feet.x + half, bottom};
        const OwnRows own = ownRows(camera);
        in.own = true;
        in.ownBox = {1250.0f, own.top, 1310.0f, own.bottom};
        in.pitchKnown = knowsPitch;
        in.pitch = static_cast<float>(p.pitch);
        in.distanceKnown = true;
        in.distance = distance;
    }

    // Where the target shows now (the camera as turned so far).
    float shownX() const {
        CameraFocuser::Input in;
        boxes({now, yaw, pitch, east, north}, in);
        return (in.targetBox.x0 + in.targetBox.x1) * 0.5f / kWidth;
    }

    CameraFocuser::Output step(CameraFocuser& focuser) {
        past.push_back({now, yaw, pitch, east, north});
        CameraFocuser::Input in;
        in.now = now;
        in.width = kWidth;
        in.height = kHeight;
        in.mouseFree = mouseFree;
        in.cursorAtMiddle = cameraMode; // The game keeps the cursor in the middle while it turns.
        in.targetName = "Goblin";
        boxes(seenAt(now - lag), in);
        const CameraFocuser::Output out = focuser.step(in);
        if (out.right && !right) {
            right = true;
            pressedAt = now;
            ++presses;
        } else if (!out.right && right) {
            right = false;
            cameraMode = false;
        }
        if (out.dx != 0 || out.dy != 0) {
            counts += std::abs(out.dx) + std::abs(out.dy);
            if (right && cameraMode) {
                yaw += turnSign * out.dx * kCount;
                pitch = std::clamp(pitch + out.dy * kCount, -0.5, 82.0 / 57.29577951308232);
            } else {
                lostCounts += std::abs(out.dx) + std::abs(out.dy);
            }
        }
        if (right && !cameraMode && takesPress && now - pressedAt >= warpDelay) {
            cameraMode = true;
        }
        east += walkEast * kDt;
        north += walkNorth * kDt;
        if (!out.log.empty()) {
            log.push_back(out.log);
        }
        now += kDt;
        return out;
    }

    // Steps for `seconds`; the last step's output.
    CameraFocuser::Output run(CameraFocuser& focuser, double seconds) {
        CameraFocuser::Output out;
        for (double end = now + seconds; now < end;) {
            out = step(focuser);
        }
        return out;
    }

    bool said(const char* text) const {
        return std::ranges::any_of(log, [&](const std::string& line) { return line.find(text) != std::string::npos; });
    }
};

} // namespace

TEST_CASE("Focus Camera turns a target beyond the character out to 3/4 of the width and lets go") {
    CameraFocuser focuser;
    Game game; // Right 2, away 6: at 0.60 of the width.
    CHECK(game.shownX() == doctest::Approx(0.598).epsilon(0.01));
    const CameraFocuser::Output out = game.run(focuser, 2.0);
    CHECK(std::fabs(game.shownX() - 0.75f) <= CameraFocuser::kTolerance);
    CHECK(out.inPlace);
    CHECK_FALSE(game.right); // Let go once turned.
    CHECK(game.lostCounts == 0); // No count before the game turned the camera.
    CHECK(game.presses >= 1);
    CHECK(game.presses <= 3);
    CHECK(game.yaw < 0.0); // Beyond the character it moves against the turn: out to the right by turning left.
    CHECK(game.said("Focus Camera: Goblin at 0.60 of the width, wants 0.75: turning left"));
}

TEST_CASE("Focus Camera turns a target between the character and the camera the other way") {
    CameraFocuser focuser;
    Game game;
    game.east = -1.5; // Left of it and nearer the camera: lower left, at 0.32, its feet low (row 1331).
    game.north = -3.0;
    CHECK(game.shownX() == doctest::Approx(0.317).epsilon(0.01));
    game.run(focuser, 2.0);
    CHECK(std::fabs(game.shownX() - 0.25f) <= CameraFocuser::kTolerance);
    CHECK(game.yaw < 0.0); // It moves with the turn: out to the left by turning left.
    CHECK_FALSE(game.right);
    CHECK(game.lostCounts == 0);
}

TEST_CASE("Focus Camera tilts up, within 20-30 degrees, for a far target near the top of the screen") {
    CameraFocuser focuser;
    Game game;
    game.pitch = 0.52; // 29.8 degrees down: the target 20 own heights off shows its feet at row 99.
    game.east = 4.0;
    game.north = 20.0;
    game.run(focuser, 2.0);
    CHECK(game.said("tilting up"));
    CHECK(game.pitch < 0.50);
    CHECK(game.pitch >= CameraFocuser::kPitchLow - 0.001);
    CHECK(std::fabs(game.shownX() - 0.75f) <= CameraFocuser::kTolerance);
}

TEST_CASE("Focus Camera says a target nearer than a quarter can show, turned only as far as it comes nearest") {
    CameraFocuser focuser;
    Game game;
    game.east = -1.0; // 2.7 own heights from the character, between it and the camera: no turn shows it past 0.27.
    game.north = -2.5;
    game.run(focuser, 2.0);
    CHECK(game.said("too close"));
    CHECK(game.shownX() < 0.31f);
    CHECK_FALSE(game.right);
}

TEST_CASE("Focus Camera keeps a walking target at its quarter") {
    CameraFocuser focuser;
    Game game;
    game.east = -3.0;
    game.north = 12.0;
    game.walkNorth = -2.0; // Walks in towards the character.
    game.run(focuser, 1.5);
    for (int i = 0; i < 3; ++i) {
        game.run(focuser, 0.5);
        CHECK(std::fabs(game.shownX() - 0.25f) <= CameraFocuser::kTolerance * 2.0f);
    }
}

TEST_CASE("Focus Camera is unsure when the turn moved the target the wrong way, and tries by how it moved") {
    CameraFocuser focuser;
    Game game;
    game.turnSign = -1; // The game turns the other way than the counts say (inverted): the camera's way is wrong.
    game.run(focuser, 4.0);
    CHECK(game.said("unsure of the way"));
    CHECK(std::fabs(game.shownX() - 0.75f) <= CameraFocuser::kTolerance);
    CHECK_FALSE(game.right);
}

TEST_CASE("Focus Camera says a target too close to reach its quarter, and does not turn on and on") {
    CameraFocuser focuser;
    Game game;
    game.east = 0.4; // In melee, beside it: 0.53 of the width, and no turn shows it at 0.75.
    game.north = 0.2;
    const CameraFocuser::Output out = game.run(focuser, 3.0);
    CHECK(game.said("too close"));
    CHECK(out.inPlace);
    CHECK(game.presses <= 4);
    CHECK_FALSE(game.right);
}

TEST_CASE("Focus Camera lets go at once when the mouse is wanted, and sends nothing meanwhile") {
    CameraFocuser focuser;
    Game game;
    for (int i = 0; i < 200 && !(game.right && game.cameraMode && game.counts > 0); ++i) {
        game.step(focuser);
    }
    REQUIRE(game.right); // Mid-turn.
    game.mouseFree = false;
    const int sent = game.counts;
    const CameraFocuser::Output out = game.step(focuser);
    CHECK_FALSE(out.right);
    CHECK_FALSE(game.right);
    game.run(focuser, 1.0);
    CHECK(game.counts == sent);
    CHECK(game.presses == 1);
    CHECK(game.said("the mouse is wanted"));
    game.mouseFree = true; // Free again: it goes on.
    game.run(focuser, 2.0);
    CHECK(std::fabs(game.shownX() - 0.75f) <= CameraFocuser::kTolerance);
}

TEST_CASE("Focus Camera lets go when the game never takes the right button for turning") {
    CameraFocuser focuser;
    Game game;
    game.takesPress = false;
    game.run(focuser, 0.7);
    CHECK(game.said("did not start turning"));
    CHECK(game.counts == 0);
    CHECK(game.presses == 1);
    CHECK_FALSE(game.right);
    game.takesPress = true; // Tried again later.
    game.run(focuser, 2.0);
    CHECK(std::fabs(game.shownX() - 0.75f) <= CameraFocuser::kTolerance);
}

TEST_CASE("Focus Camera brings the pitch into 20-30 degrees, and turns only while the pitch is not known") {
    {
        CameraFocuser focuser;
        Game game;
        game.pitch = 0.17; // 10 degrees.
        game.run(focuser, 2.0);
        CHECK(game.pitch >= CameraFocuser::kPitchLow - CameraFocuser::kPitchSlack);
        CHECK(game.pitch <= CameraFocuser::kPitchHigh + CameraFocuser::kPitchSlack);
        CHECK(std::fabs(game.shownX() - 0.75f) <= CameraFocuser::kTolerance);
        CHECK(game.said("tilting down"));
    }
    {
        CameraFocuser focuser;
        Game game;
        game.pitch = 0.17;
        game.knowsPitch = false;
        game.run(focuser, 2.0);
        CHECK(game.pitch == doctest::Approx(0.17)); // Not tilted,
        CHECK(std::fabs(game.shownX() - 0.75f) <= CameraFocuser::kTolerance); // but turned all the same.
        CHECK(game.said("pitch is not known yet"));
    }
}

TEST_CASE("Focus Camera with no target is in place at once") {
    CameraFocuser focuser;
    CameraFocuser::Input in;
    in.now = 1.0;
    in.width = kWidth;
    in.height = kHeight;
    in.mouseFree = true;
    const CameraFocuser::Output out = focuser.step(in);
    CHECK(out.inPlace);
    CHECK_FALSE(out.right);
    CHECK(out.dx == 0);
}
