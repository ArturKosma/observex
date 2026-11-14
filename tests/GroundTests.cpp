#include "core/Ground.h"
#include "heuristics/DepthTilt.h"
#include "heuristics/GroundMotion.h"
#include "heuristics/MinimapCone.h"
#include "heuristics/Snapper.h" // GreyImage

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <optional>
#include <vector>

using namespace observex;

namespace {

constexpr float kWidth = 2560.0f;
constexpr float kHeight = 1440.0f;

// Where ground point (`right`, `away`) of `from`'s view is in the view of `to`, the camera having turned `yaw` and its
// feet slid (`slideRight`, `slideAway`) of `from`'s view, as GroundFit::motion takes it.
void moveGround(float right, float away, float yaw, float slideRight, float slideAway, float& movedRight,
                float& movedAway) {
    const float c = std::cos(yaw);
    const float s = std::sin(yaw);
    movedRight = c * (right - slideRight) - s * (away - slideAway);
    movedAway = s * (right - slideRight) + c * (away - slideAway);
}

// The matches of ground points on a grid seen by `from`, then by `to` after the camera turned and slid.
std::vector<GroundMatch> matchesOf(const GroundCamera& from, const GroundCamera& to, float yaw, float slideRight = 0.0f,
                                   float slideAway = 0.0f) {
    std::vector<GroundMatch> matches;
    for (int row = 0; row < 6; ++row) {
        for (int column = 0; column < 10; ++column) {
            const float x = kWidth * (static_cast<float>(column) + 0.5f) / 10.0f;
            const float y = kHeight * (0.2f + 0.7f * static_cast<float>(row) / 5.0f);
            const std::optional<GroundPoint> ground = screenToGround(from, x, y);
            if (!ground) {
                continue;
            }
            float right = 0.0f;
            float away = 0.0f;
            moveGround(ground->x, ground->y, yaw, slideRight, slideAway, right, away);
            const GroundPoint seen = groundToScreen(to, right, away);
            if (seen.ahead) {
                matches.push_back({x, y, seen.x, seen.y});
            }
        }
    }
    return matches;
}

} // namespace

TEST_CASE("the point the camera circles is in the screen's middle, the far ground at the horizon") {
    const GroundCamera camera = groundCamera(0.5f, 10.0f, kWidth, kHeight);
    const GroundPoint middle = worldToScreen(camera, 0.0f, 0.0f, kGroundTarget);
    CHECK(middle.ahead);
    CHECK(middle.x == doctest::Approx(kWidth / 2));
    CHECK(middle.y == doctest::Approx(kHeight / 2));
    const GroundPoint far = groundToScreen(camera, 0.0f, 1e5f);
    CHECK(far.y == doctest::Approx(horizonRow(camera)).epsilon(0.001));
    CHECK(camera.focal == doctest::Approx(kGroundFocal));
    CHECK(groundCamera(0.5f, 10.0f, 1280.0f, 720.0f).focal == doctest::Approx(kGroundFocal / 2));
}

TEST_CASE("a ground pixel goes to the ground and back to the same pixel; none above the horizon") {
    const GroundCamera camera = groundCamera(0.4f, 12.0f, kWidth, kHeight);
    for (const auto& [x, y] : {std::pair{100.0f, 1400.0f}, {1280.0f, 800.0f}, {2400.0f, 500.0f}}) {
        const std::optional<GroundPoint> ground = screenToGround(camera, x, y);
        REQUIRE(ground);
        const GroundPoint back = groundToScreen(camera, ground->x, ground->y);
        CHECK(back.x == doctest::Approx(x).epsilon(1e-4));
        CHECK(back.y == doctest::Approx(y).epsilon(1e-4));
    }
    CHECK_FALSE(screenToGround(camera, 1280.0f, horizonRow(camera) - 10.0f));
    // Behind the camera's eye is not on the screen.
    CHECK_FALSE(groundToScreen(camera, 0.0f, -50.0f).ahead);
}

TEST_CASE("the distance that shows the own character's box down to a row is the camera's") {
    const GroundCamera camera = groundCamera(0.6f, 9.0f, kWidth, kHeight);
    const OwnRows rows = ownRows(camera);
    CHECK(rows.top < kHeight / 2);
    CHECK(rows.bottom > kHeight / 2);
    const std::optional<float> distance = distanceForOwnBottom(camera, rows.bottom);
    REQUIRE(distance);
    CHECK(*distance == doctest::Approx(9.0f).epsilon(1e-4));
    CHECK_FALSE(distanceForOwnBottom(camera, kHeight / 2 - 5.0f));
}

TEST_CASE("the own character is the surest of its kind in the screen's middle") {
    std::vector<Tracked> things(3);
    things[0].kind = 3;
    things[0].sure = 0.9f;
    things[0].x = 200.0f; // Off the middle.
    things[0].width = 100.0f;
    things[1].kind = 3;
    things[1].sure = 0.7f;
    things[1].x = 1230.0f;
    things[1].width = 100.0f;
    things[2].kind = 0;
    things[2].sure = 0.95f;
    things[2].x = 1230.0f;
    things[2].width = 100.0f;
    CHECK(ownCharacter(things, 3, kWidth) == &things[1]);
    CHECK(ownCharacter(things, -1, kWidth) == nullptr);
    things[1].sure = 0.4f;
    CHECK(ownCharacter(things, 3, kWidth) == nullptr);
}

TEST_CASE("the ground's motion in camera turns tells the pitch, the own box the distance") {
    const GroundCamera truth = groundCamera(0.7f, 11.0f, kWidth, kHeight);
    const OwnRows rows = ownRows(truth);
    GroundFit fit;
    double timeMs = 0.0;
    fit.own(rows.top, rows.bottom, kWidth, kHeight, timeMs);
    REQUIRE(fit.camera().known);
    CHECK_FALSE(fit.camera().pitchMeasured);
    CHECK(fit.camera().pitch == doctest::Approx(kGroundStartPitch));
    float turned = 0.0f;
    for (int pair = 0; pair < 8; ++pair) {
        const float yaw = 0.04f;
        fit.own(rows.top, rows.bottom, kWidth, kHeight, timeMs + 66.0);
        CHECK(fit.motion(matchesOf(truth, truth, yaw), timeMs, timeMs + 66.0));
        timeMs += 66.0;
        turned += yaw;
    }
    CHECK(fit.camera().pitchMeasured);
    CHECK(fit.camera().pitch == doctest::Approx(0.7f).epsilon(0.01));
    CHECK(fit.camera().distance == doctest::Approx(11.0f).epsilon(0.01));
    CHECK(fit.camera().yaw == doctest::Approx(turned).epsilon(0.01));
    CHECK(fit.camera().yawSpeed == doctest::Approx(0.04f / 66.0f).epsilon(0.02));
    CHECK(fit.lastAgreeing() >= 40);
}

TEST_CASE("walking keeps the pitch and goes on the map; standing still holds it") {
    const GroundCamera truth = groundCamera(0.5f, 13.0f, kWidth, kHeight);
    const OwnRows rows = ownRows(truth);
    GroundFit fit;
    fit.own(rows.top, rows.bottom, kWidth, kHeight, 0.0);
    // Turns first, so it knows the pitch.
    double timeMs = 0.0;
    for (int pair = 0; pair < 8; ++pair, timeMs += 66.0) {
        fit.own(rows.top, rows.bottom, kWidth, kHeight, timeMs + 66.0);
        fit.motion(matchesOf(truth, truth, 0.05f), timeMs, timeMs + 66.0);
    }
    REQUIRE(fit.camera().pitch == doctest::Approx(0.5f).epsilon(0.01));
    const float yaw = fit.camera().yaw;
    const float feetY = fit.camera().feetY;
    for (int pair = 0; pair < 5; ++pair, timeMs += 66.0) {
        fit.own(rows.top, rows.bottom, kWidth, kHeight, timeMs + 66.0);
        CHECK(fit.motion(matchesOf(truth, truth, 0.0f, 0.0f, 0.3f), timeMs, timeMs + 66.0));
    }
    CHECK(fit.camera().pitch == doctest::Approx(0.5f).epsilon(0.02));
    CHECK(fit.camera().yaw == doctest::Approx(yaw).epsilon(0.01));
    // Away from the camera, turned by the turns before onto the map.
    float right = 0.0f;
    float away = 0.0f;
    GroundCamera start = fit.camera();
    start.feetX = 0.0f;
    start.feetY = 0.0f;
    mapToGround(start, fit.camera().feetX, fit.camera().feetY, right, away);
    CHECK(right == doctest::Approx(0.0f).epsilon(0.02));
    CHECK(away == doctest::Approx(1.5f).epsilon(0.02));
    (void)feetY;
    for (int pair = 0; pair < 3; ++pair, timeMs += 66.0) {
        fit.own(rows.top, rows.bottom, kWidth, kHeight, timeMs + 66.0);
        CHECK(fit.motion(matchesOf(truth, truth, 0.0f), timeMs, timeMs + 66.0));
    }
    CHECK(fit.camera().pitch == doctest::Approx(0.5f).epsilon(0.02));
    CHECK(fit.camera().yawSpeed == doctest::Approx(0.0f));
}

TEST_CASE("a pitch held (by the depth network) is mostly kept by the fit; free, a turn's motion takes it back") {
    const GroundCamera truth = groundCamera(0.5f, 13.0f, kWidth, kHeight);
    const OwnRows rows = ownRows(truth);
    for (const bool held : {true, false}) {
        CAPTURE(held);
        GroundFit fit;
        fit.own(rows.top, rows.bottom, kWidth, kHeight, 0.0);
        double timeMs = 0.0;
        for (int pair = 0; pair < 8; ++pair, timeMs += 66.0) {
            fit.own(rows.top, rows.bottom, kWidth, kHeight, timeMs + 66.0);
            fit.motion(matchesOf(truth, truth, 0.05f), timeMs, timeMs + 66.0);
        }
        REQUIRE(fit.camera().pitch == doctest::Approx(0.5f).epsilon(0.01));
        fit.tilt(0.06f); // As a depth map says: 3.4 degrees off.
        const GroundCamera& camera = fit.camera();
        const GroundFit::Start start{camera.pitch, camera.pitch, camera.distance, camera.distance,
                                     false,        0.0f,         false,           held};
        fit.own(rows.top, rows.bottom, kWidth, kHeight, timeMs + 66.0);
        CHECK(fit.motion(matchesOf(truth, truth, 0.05f), timeMs, timeMs + 66.0, start));
        // Held, a turn's motion (which says the pitch) takes it back only about a quarter of the way (1 degree).
        if (held) {
            CHECK(fit.camera().pitch > 0.535f);
        } else {
            CHECK(fit.camera().pitch == doctest::Approx(0.5f).epsilon(0.01));
        }
    }
}

TEST_CASE("matches off the ground count for little") {
    const GroundCamera truth = groundCamera(0.6f, 10.0f, kWidth, kHeight);
    const OwnRows rows = ownRows(truth);
    GroundFit fit;
    double timeMs = 0.0;
    for (int pair = 0; pair < 8; ++pair, timeMs += 66.0) {
        fit.own(rows.top, rows.bottom, kWidth, kHeight, timeMs);
        fit.own(rows.top, rows.bottom, kWidth, kHeight, timeMs + 66.0);
        std::vector<GroundMatch> matches = matchesOf(truth, truth, 0.04f);
        for (std::size_t i = 0; i < matches.size(); i += 4) { // A quarter stand still: the interface.
            matches[i].x1 = matches[i].x0;
            matches[i].y1 = matches[i].y0;
        }
        fit.motion(matches, timeMs, timeMs + 66.0);
    }
    CHECK(fit.camera().pitch == doctest::Approx(0.6f).epsilon(0.02));
}

TEST_CASE("the camera is forgotten a while after the own character was last seen") {
    const GroundCamera truth = groundCamera(0.6f, 10.0f, kWidth, kHeight);
    const OwnRows rows = ownRows(truth);
    GroundFit fit;
    fit.own(rows.top, rows.bottom, kWidth, kHeight, 0.0);
    fit.unseen(GroundFit::kKeepMs - 1.0);
    CHECK(fit.camera().known);
    fit.unseen(GroundFit::kKeepMs + 1.0);
    CHECK_FALSE(fit.camera().known);
    // No own box near the images: no fit.
    fit.own(rows.top, rows.bottom, kWidth, kHeight, 5000.0);
    CHECK_FALSE(fit.motion(matchesOf(truth, truth, 0.04f), 9000.0, 9066.0));
}

TEST_CASE("a camera goes on along its speeds, and lies between two") {
    GroundCamera camera = groundCamera(0.5f, 10.0f, kWidth, kHeight);
    camera.timeMs = 100.0;
    camera.yawSpeed = 0.001f;
    camera.feetSpeedY = 0.01f;
    const GroundCamera later = groundAt(camera, 150.0);
    CHECK(later.yaw == doctest::Approx(0.05f));
    CHECK(later.feetY == doctest::Approx(0.5f));
    CHECK(groundAt(camera, 10'000.0).yaw == doctest::Approx(0.001f * kGroundAheadMs));

    GroundCamera after = camera;
    after.timeMs = 200.0;
    after.yaw = 0.2f;
    after.pitch = 0.7f;
    const GroundCamera middle = groundBetween(camera, after, 150.0);
    CHECK(middle.yaw == doctest::Approx(0.1f));
    CHECK(middle.pitch == doctest::Approx(0.6f));
    CHECK(groundBetween(camera, after, 50.0).yaw == doctest::Approx(0.0f));
    CHECK(groundBetween(camera, after, 300.0).yaw == doctest::Approx(0.2f));
    CHECK(groundBetween(camera, GroundCamera{}, 300.0).pitch == doctest::Approx(0.5f));
}

namespace {

constexpr int kSmallWidth = 640;
constexpr int kSmallHeight = 360;

// Smooth bumpy grey ground (value noise), by its place on the map.
float groundGrey(float mapX, float mapY) {
    const auto lattice = [](int x, int y) {
        const std::uint32_t hash =
            (static_cast<std::uint32_t>(x) * 73856093u ^ static_cast<std::uint32_t>(y) * 19349663u) * 2654435761u;
        return static_cast<float>(hash >> 24) / 255.0f;
    };
    const auto noise = [&lattice](float x, float y) {
        const float fx = std::floor(x);
        const float fy = std::floor(y);
        const int ix = static_cast<int>(fx);
        const int iy = static_cast<int>(fy);
        const float tx = (x - fx) * (x - fx) * (3.0f - 2.0f * (x - fx));
        const float ty = (y - fy) * (y - fy) * (3.0f - 2.0f * (y - fy));
        const float top = lattice(ix, iy) + tx * (lattice(ix + 1, iy) - lattice(ix, iy));
        const float bottom = lattice(ix, iy + 1) + tx * (lattice(ix + 1, iy + 1) - lattice(ix, iy + 1));
        return top + ty * (bottom - top);
    };
    return 60.0f + 90.0f * noise(mapX * 3.0f + 500.0f, mapY * 3.0f + 500.0f) +
           40.0f * noise(mapX * 9.0f + 900.0f, mapY * 9.0f + 900.0f);
}

// The ground seen by `camera` (on the map as its yaw and feet say), on a kSmallWidth x kSmallHeight screen.
std::vector<std::uint8_t> render(const GroundCamera& camera) {
    std::vector<std::uint8_t> pixels(static_cast<std::size_t>(kSmallWidth * kSmallHeight), 0);
    const float c = std::cos(camera.yaw);
    const float s = std::sin(camera.yaw);
    for (int y = 0; y < kSmallHeight; ++y) {
        for (int x = 0; x < kSmallWidth; ++x) {
            const std::optional<GroundPoint> ground =
                screenToGround(camera, static_cast<float>(x) + 0.5f, static_cast<float>(y) + 0.5f);
            if (ground) {
                // mapToGround's inverse.
                const float mapX = camera.feetX + c * ground->x + s * ground->y;
                const float mapY = camera.feetY - s * ground->x + c * ground->y;
                pixels[static_cast<std::size_t>(y * kSmallWidth + x)] =
                    static_cast<std::uint8_t>(std::lround(std::fmin(groundGrey(mapX, mapY), 255.0f)));
            }
        }
    }
    return pixels;
}

} // namespace

TEST_CASE("following rendered ground through a camera turn finds its pitch and how far it turned") {
    GroundCamera truth = groundCamera(0.65f, 10.0f, kSmallWidth, kSmallHeight);
    const OwnRows rows = ownRows(truth);
    GroundFollower follower;
    GreyImage image;
    int fitted = 0;
    for (int step = 0; step < 12; ++step) {
        const double timeMs = step * 66.0;
        truth.yaw = 0.03f * static_cast<float>(step);
        const std::vector<std::uint8_t> pixels = render(truth);
        image.fromGrey(pixels.data(), kSmallWidth, kSmallHeight, kSmallWidth);
        GroundHints hints;
        hints.hasOwn = true;
        hints.ownTop = rows.top;
        hints.ownBottom = rows.bottom;
        follower.begin(timeMs, kSmallWidth, kSmallHeight, hints);
        fitted += follower.end(image.view(static_cast<std::uint64_t>(step + 1))) ? 1 : 0;
    }
    CHECK(fitted == 11);
    CHECK(follower.lastMatches() >= 40);
    CHECK(follower.camera().pitch == doctest::Approx(0.65f).epsilon(0.03));
    CHECK(follower.camera().distance == doctest::Approx(10.0f).epsilon(0.03));
    CHECK(follower.camera().yaw == doctest::Approx(0.33f).epsilon(0.03));
}

namespace {

// A minimap as the game draws it, on a 512 x 512 screen: a textured map, and the camera's light cone
// (MinimapCone::kWidth wide) from the tip at (`tipX`, `tipY`) towards `heading` (radians clockwise from up).
std::vector<std::uint8_t> minimap(float tipX, float tipY, float heading) {
    constexpr int kSize = 512;
    std::vector<std::uint8_t> pixels(static_cast<std::size_t>(kSize * kSize));
    for (int y = 0; y < kSize; ++y) {
        for (int x = 0; x < kSize; ++x) {
            const float dx = static_cast<float>(x) - tipX;
            const float dy = static_cast<float>(y) - tipY;
            float grey = 70.0f + 12.0f * std::sin(0.31f * static_cast<float>(x)) * std::cos(0.23f * static_cast<float>(y));
            if (const float radius = std::hypot(dx, dy); radius > 2.0f && radius < 45.0f) {
                const float off = std::remainder(std::atan2(dx, -dy) - heading, 6.28318531f);
                grey += std::abs(off) < 0.5f * MinimapCone::kWidth ? 90.0f : 0.0f;
            }
            pixels[static_cast<std::size_t>(y * kSize + x)] = static_cast<std::uint8_t>(std::clamp(grey, 0.0f, 255.0f));
        }
    }
    return pixels;
}

} // namespace

TEST_CASE("the minimap's cone is found by its window and says where the camera looks, round through north") {
    const GreyArea window{100, 100, 400, 418}; // Puts the tip at (249, 268); it is 2 pixels right, 1 up.
    MinimapCone cone;
    GreyImage image;
    std::array<GreyArea, MinimapCone::kCandidates> areas{};
    for (int step = 0; step < 8; ++step) {
        const float heading = std::fmod(5.6f + 0.25f * static_cast<float>(step), 6.28318531f);
        const std::vector<std::uint8_t> pixels = minimap(251.0f, 267.0f, heading);
        image.fromGrey(pixels.data(), 512, 512, 512);
        REQUIRE(cone.areas(66.0 * step, {&window, 1}, areas.data(), static_cast<int>(areas.size())) == 1);
        REQUIRE(cone.read(image.view(static_cast<std::uint64_t>(step + 1)).levels[0]));
        CHECK(std::abs(std::remainder(cone.heading() - heading, 6.28318531f)) < 0.02f);
        CHECK(cone.locked());
        CHECK(std::abs(cone.tipX() - 251.0f) <= 1.0f);
        CHECK(std::abs(cone.tipY() - 267.0f) <= 1.0f);
    }
    // Once found, only round the tip is read.
    REQUIRE(cone.areas(600.0, {&window, 1}, areas.data(), static_cast<int>(areas.size())) == 1);
    CHECK(areas[0].x1 - areas[0].x0 < 70);
}

TEST_CASE("a map with no cone is no heading, and is not looked at again at once") {
    const GreyArea window{100, 100, 400, 418};
    std::vector<std::uint8_t> pixels(512 * 512, 90);
    GreyImage image;
    image.fromGrey(pixels.data(), 512, 512, 512);
    MinimapCone cone;
    std::array<GreyArea, MinimapCone::kCandidates> areas{};
    REQUIRE(cone.areas(0.0, {&window, 1}, areas.data(), static_cast<int>(areas.size())) == 1);
    CHECK_FALSE(cone.read(image.view(1).levels[0]));
    CHECK_FALSE(cone.seen());
    CHECK(cone.areas(66.0, {&window, 1}, areas.data(), static_cast<int>(areas.size())) == 0);
    CHECK(cone.areas(MinimapCone::kRetryMs + 1.0, {&window, 1}, areas.data(), static_cast<int>(areas.size())) == 1);
}

namespace {

// The minimap in `window` of a kSmallWidth x kSmallHeight screen, as minimap() draws one, its cone towards `heading`
// from where MinimapCone looks for the tip.
void drawMinimap(std::vector<std::uint8_t>& pixels, const GreyArea& window, float heading) {
    const float tipX = 0.5f * static_cast<float>(window.x0 + window.x1) - 1.0f;
    const float tipY = 0.5f * static_cast<float>(window.y0 + MinimapCone::kTitle + window.y1);
    for (int y = window.y0; y < window.y1; ++y) {
        for (int x = window.x0; x < window.x1; ++x) {
            const float dx = static_cast<float>(x) - tipX;
            const float dy = static_cast<float>(y) - tipY;
            float grey =
                70.0f + 12.0f * std::sin(0.31f * static_cast<float>(x)) * std::cos(0.23f * static_cast<float>(y));
            if (const float radius = std::hypot(dx, dy); radius > 2.0f && radius < 45.0f) {
                const float off = std::remainder(std::atan2(dx, -dy) - heading, 6.28318531f);
                grey += std::abs(off) < 0.5f * MinimapCone::kWidth ? 90.0f : 0.0f;
            }
            pixels[static_cast<std::size_t>(y * kSmallWidth + x)] =
                static_cast<std::uint8_t>(std::clamp(grey, 0.0f, 255.0f));
        }
    }
}

} // namespace

TEST_CASE("between follows the camera turns as the ground does, held to the minimap's cone, and stops with them") {
    const GreyArea window{470, 10, 630, 180};
    GroundCamera truth = groundCamera(0.65f, 10.0f, kSmallWidth, kSmallHeight);
    const OwnRows rows = ownRows(truth);
    GroundHints hints;
    hints.hasOwn = true;
    hints.ownTop = rows.top;
    hints.ownBottom = rows.bottom;
    hints.avoid = &window;
    hints.avoidCount = 1;
    hints.windows = &window;
    hints.windowCount = 1;
    GroundFollower follower;
    GreyImage image;
    std::uint64_t number = 0;
    const auto show = [&](std::optional<float> heading, bool flat = false) {
        std::vector<std::uint8_t> pixels = flat ? std::vector<std::uint8_t>(kSmallWidth * kSmallHeight, 120)
                                                : render(truth);
        if (heading) {
            drawMinimap(pixels, window, *heading);
        }
        image.fromGrey(pixels.data(), kSmallWidth, kSmallHeight, kSmallWidth);
        return image.view(++number);
    };
    const auto off = [](float a, float b) { return std::abs(std::remainder(a - b, 6.28318531f)); };
    // Followed through a turn, the cone turning with the camera.
    for (int step = 0; step < 4; ++step) {
        truth.yaw = 0.03f * static_cast<float>(step);
        follower.begin(66.0 * step, kSmallWidth, kSmallHeight, hints);
        follower.end(show(1.0f + truth.yaw));
    }
    const GroundCamera followed = follower.camera();
    REQUIRE(followed.known);
    REQUIRE(follower.cone().seen());
    REQUIRE(followed.yawSpeed > 0.0003f); // The follow's: 0.03 every 66 ms.

    // It turns on fast, then stops: the camera on the images between turns and stops with the ground, and does not go
    // on along the follow's speed. (On this screen's eighth size, 80 x 45, the turns found come out about 0.75 of the
    // true ones; on the real one they were within half a degree of the far trees' shift, clips 2026-11-11_22-10-08
    // and 2026-11-12_18-58-01.)
    std::array<GreyArea, 8> areas{};
    float stopped = 0.0f;
    for (int step = 1; step <= 7; ++step) {
        const double timeMs = 198.0 + 8.0 * step;
        const float turned = 0.02f * static_cast<float>(std::min(step, 3));
        truth.yaw = 0.09f + turned;
        REQUIRE(follower.headAreas(timeMs, hints, areas.data(), static_cast<int>(areas.size())) == 1);
        CHECK(follower.headFirstLevel() == GroundMotion::kCoarseLevel);
        CHECK(follower.head(show(1.09f + turned)));
        const GroundCamera camera = follower.camera();
        CHECK(camera.known);
        CHECK(camera.timeMs == doctest::Approx(timeMs));
        const float turnedBy = std::remainder(camera.yaw - followed.yaw, 6.28318531f);
        CHECK(turnedBy > 0.6f * turned);
        CHECK(turnedBy < 1.2f * turned);
        if (step == 3) {
            stopped = camera.yaw;
        } else if (step > 3) {
            CHECK(off(camera.yaw, stopped) < 0.001f);
        }
        CHECK(camera.pitch == doctest::Approx(groundAt(followed, timeMs).pitch));
    }
    CHECK(std::abs(follower.camera().yawSpeed) < 0.0002f);

    // Where the ground says nothing (a flat image), where the cone looks, and held there.
    REQUIRE(follower.headAreas(262.0, hints, areas.data(), static_cast<int>(areas.size())) == 1);
    CHECK_FALSE(follower.head(show(1.17f, true)));
    CHECK(off(follower.camera().yaw, follower.cone().heading()) < 0.001f);
    CHECK(off(follower.cone().heading(), 1.17f) < 0.02f);
    CHECK(follower.camera().yawSpeed == 0.0f);

    // With neither, it goes on along the follow's speeds, as before.
    REQUIRE(follower.headAreas(270.0, hints, areas.data(), static_cast<int>(areas.size())) == 1);
    CHECK_FALSE(follower.head(show(std::nullopt, true)));
    CHECK(follower.camera().yaw == doctest::Approx(groundAt(followed, 270.0).yaw));

    // The next follow is the follow's own again.
    truth.yaw = 0.1f;
    follower.begin(274.0, kSmallWidth, kSmallHeight, hints);
    follower.end(show(1.1f));
    CHECK(follower.camera().timeMs == doctest::Approx(274.0));

    // While the cone holds still, the ground turns the camera at most kConeSlack from where it was (looking level the
    // ground's patches turned it where the cone did not).
    follower.begin(340.0, kSmallWidth, kSmallHeight, hints);
    follower.end(show(1.1f));
    const GroundCamera still = follower.camera();
    for (int step = 1; step <= 5; ++step) {
        const double timeMs = 340.0 + 8.0 * step;
        truth.yaw = 0.1f + 0.015f * static_cast<float>(step);
        REQUIRE(follower.headAreas(timeMs, hints, areas.data(), static_cast<int>(areas.size())) == 1);
        CHECK(follower.head(show(1.1f)));
    }
    CHECK(off(follower.camera().yaw - still.yaw, GroundFollower::kConeSlack) < 0.001f);
}

namespace {

// A positive float as a half-precision one (what the depth network gives), rounded.
std::uint16_t toHalf(float value) {
    std::uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    const int exponent = static_cast<int>((bits >> 23) & 0xFFu) - 127 + 15;
    if (exponent <= 0) {
        return 0;
    }
    if (exponent >= 31) {
        return 0x7C00u;
    }
    return static_cast<std::uint16_t>((static_cast<std::uint32_t>(exponent) << 10) + (((bits & 0x7FFFFFu) + 0x1000u) >> 13));
}

// A depth map of flat ground as `camera` sees it on a `screenWidth` x `screenHeight` screen: the true inverse depth
// (along where it looks, own heights), times 5 (the network's scale), 0 past the horizon.
struct FlatDepth {
    int width = 0;
    int height = 0;
    std::vector<std::uint16_t> map;
};
FlatDepth flatDepth(const GroundCamera& camera, int width, int height, float screenWidth, float screenHeight) {
    FlatDepth depth{width, height, std::vector<std::uint16_t>(static_cast<std::size_t>(width * height))};
    const float above = camera.distance * std::sin(camera.pitch) + kGroundTarget; // The eye over the ground.
    for (int y = 0; y < height; ++y) {
        const float row = (static_cast<float>(y) + 0.5f) * screenHeight / static_cast<float>(height);
        const float inverse =
            (std::sin(camera.pitch) + (row - camera.middleY) / camera.focal * std::cos(camera.pitch)) / above;
        const std::uint16_t value = toHalf(std::max(inverse, 0.0f) * 5.0f);
        std::fill_n(depth.map.begin() + static_cast<std::ptrdiff_t>(y * width), width, value);
    }
    (void)screenWidth;
    return depth;
}

DepthInput inputOf(const FlatDepth& depth, float screenWidth, float screenHeight) {
    DepthInput input;
    input.map = depth.map.data();
    input.width = depth.width;
    input.height = depth.height;
    input.screenWidth = static_cast<int>(screenWidth);
    input.screenHeight = static_cast<int>(screenHeight);
    return input;
}

// `area` of the screen (pixels) set to `value` on `depth`'s map.
void paint(FlatDepth& depth, const GreyArea& area, float value, float screenWidth, float screenHeight) {
    const float sx = static_cast<float>(depth.width) / screenWidth;
    const float sy = static_cast<float>(depth.height) / screenHeight;
    for (int y = static_cast<int>(static_cast<float>(area.y0) * sy); y < static_cast<int>(static_cast<float>(area.y1) * sy); ++y) {
        for (int x = static_cast<int>(static_cast<float>(area.x0) * sx); x < static_cast<int>(static_cast<float>(area.x1) * sx); ++x) {
            depth.map[static_cast<std::size_t>(y * depth.width + x)] = toHalf(value);
        }
    }
}

} // namespace

TEST_CASE("the depth of flat ground falls on a line that reaches 0 at the horizon, the interface and characters out") {
    const GroundCamera truth = groundCamera(0.3f, 10.0f, kWidth, kHeight);
    FlatDepth depth = flatDepth(truth, 924, 518, kWidth, kHeight);
    DepthLine line = measureDepthLine(inputOf(depth, kWidth, kHeight), {});
    REQUIRE(line.known);
    CHECK(std::abs(line.zeroRow - horizonRow(truth)) < 3.0f);
    CHECK(line.straight < 0.005f);
    CHECK(line.rows > 140);

    // A window across two thirds of the band's columns, and a character: nearer than the ground there.
    const GreyArea window{700, 700, 1500, 900};
    const GreyArea character{1250, 760, 1330, 1000};
    paint(depth, window, 9.0f, kWidth, kHeight);
    paint(depth, character, 6.0f, kWidth, kHeight);
    DepthInput input = inputOf(depth, kWidth, kHeight);
    const DepthLine spoilt = measureDepthLine(input, {}); // With them in, no line (or a crooked one).
    CHECK((!spoilt.known || spoilt.straight > GroundFollower::kDepthStraight));
    input.ignore = &window;
    input.ignoreCount = 1;
    line = measureDepthLine(input, {&character, 1});
    REQUIRE(line.known);
    CHECK(std::abs(line.zeroRow - horizonRow(truth)) < 3.0f);
    CHECK(line.straight < 0.005f);
}

TEST_CASE("the network's pitch is the pitch up to 15 degrees, then means ever more of it, counting less") {
    constexpr float kDegree = 0.0174532925f;
    CHECK(depthPitch(10.0f * kDegree).pitch == doctest::Approx(10.5f * kDegree).epsilon(0.001));
    CHECK(depthPitch(10.0f * kDegree).weight > 0.5f);
    float before = depthPitch(0.0f).pitch;
    for (float seen = 1.0f; seen <= 45.0f; seen += 1.0f) {
        const DepthPitch said = depthPitch(seen * kDegree);
        CHECK(said.pitch >= before);
        CHECK(said.weight > 0.0f);
        before = said.pitch;
    }
    CHECK(depthPitch(30.0f * kDegree).weight < 0.15f);
    CHECK(depthPitch(37.0f * kDegree).pitch > 75.0f * kDegree);
    CHECK(depthPitch(45.0f * kDegree).pitch == doctest::Approx(depthPitch(38.0f * kDegree).pitch)); // Saturated.
}

TEST_CASE("a depth map pulls the camera's pitch: all the way while it is only guessed, then over the images after") {
    const GroundCamera truth = groundCamera(0.65f, 10.0f, kSmallWidth, kSmallHeight);
    const OwnRows rows = ownRows(truth);
    GroundFollower follower;
    GreyImage image;
    GroundHints hints;
    hints.hasOwn = true;
    hints.ownTop = rows.top;
    hints.ownBottom = rows.bottom;
    const std::vector<std::uint8_t> pixels = render(truth);
    image.fromGrey(pixels.data(), kSmallWidth, kSmallHeight, kSmallWidth);
    follower.begin(100.0, kSmallWidth, kSmallHeight, hints);
    follower.end(image.view(1));
    REQUIRE(follower.camera().known);
    REQUIRE_FALSE(follower.camera().pitchMeasured);
    const float guessed = follower.camera().pitch;
    const auto mapAt = [&](float pitch) {
        GroundCamera seen = follower.camera();
        seen.pitch = pitch;
        return flatDepth(seen, 308, 173, kSmallWidth, kSmallHeight);
    };
    const float screenWidth = static_cast<float>(kSmallWidth);
    const float screenHeight = static_cast<float>(kSmallHeight);

    // Older than the images kept: says nothing.
    const FlatDepth first = mapAt(0.2f);
    GroundDepthNote note = follower.depth(inputOf(first, screenWidth, screenHeight), 50.0);
    CHECK_FALSE(note.measured);
    CHECK(follower.camera().pitch == guessed);

    // Only guessed: all the way at once.
    note = follower.depth(inputOf(first, screenWidth, screenHeight), 100.0);
    REQUIRE(note.measured);
    CHECK(note.seen == doctest::Approx(0.2f).epsilon(0.03));
    CHECK(note.then == doctest::Approx(guessed));
    CHECK(follower.camera().pitch == doctest::Approx(depthPitch(note.seen).pitch));

    // Then not at once: how far off it was, from the pitch on the map's image.
    const float now = follower.camera().pitch;
    const FlatDepth second = mapAt(0.22f);
    note = follower.depth(inputOf(second, screenWidth, screenHeight), 100.0);
    REQUIRE(note.measured);
    const DepthPitch said = depthPitch(note.seen);
    CHECK(note.then == doctest::Approx(now));
    CHECK(note.pulled == doctest::Approx(said.pitch - now));
    CHECK(follower.camera().pitch == now);

    // A crooked map (a wall, the horizon in the band) says nothing, and leaves the pull as it was.
    FlatDepth crooked = mapAt(0.3f);
    for (int y = 0; y < crooked.height; y += 2) {
        std::fill_n(crooked.map.begin() + static_cast<std::ptrdiff_t>(y * crooked.width), crooked.width, toHalf(3.0f));
    }
    note = follower.depth(inputOf(crooked, screenWidth, screenHeight), 100.0);
    CHECK_FALSE(note.measured);
    CHECK(follower.camera().pitch == now);

    // The next image (66 ms on) tilts it the share of the way its time says; the motion fits from the pitch as it was,
    // and the follow keeps it.
    follower.begin(166.0, kSmallWidth, kSmallHeight, hints);
    follower.end(image.view(2));
    const float rate = std::max(said.weight, GroundFollower::kDepthLeast) / GroundFollower::kDepthMs;
    const float share = 1.0f - std::exp(-rate * 66.0f);
    CHECK(share > 0.2f);
    CHECK(follower.camera().pitch == doctest::Approx(now + share * (said.pitch - now)).epsilon(0.02));
}
