#include "core/Ground.h"
#include "heuristics/Snapper.h" // GreyImage
#include "heuristics/TurnPitch.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

using namespace observex;

namespace {

constexpr float kDegree = 0.017453292f;

// A camera of the ground (as core/Ground's) on a screen `width` x `height`, turned `yaw` and its own character's feet
// at (`feetX`, `feetY`) on the ground's map.
GroundCamera cameraAt(float pitch, float yaw, float feetX, float feetY, float width = 2560.0f, float height = 1440.0f) {
    GroundCamera camera = groundCamera(pitch, 6.0f, width, height);
    camera.yaw = yaw;
    camera.feetX = feetX;
    camera.feetY = feetY;
    return camera;
}

// A point of the world (the ground's map's x, y and up, own heights) as `camera` sees it, as rays: false if not on
// the screen.
bool seenAs(const GroundCamera& camera, float x, float y, float up, double& rayX, double& rayY) {
    float right = 0.0f;
    float away = 0.0f;
    mapToGround(camera, x, y, right, away);
    const GroundPoint point = worldToScreen(camera, right, away, up);
    if (!point.ahead || point.x < 0.0f || point.y < 0.0f || point.x > 2.0f * camera.middleX ||
        point.y > 2.0f * camera.middleY) {
        return false;
    }
    rayX = (point.x - camera.middleX) / camera.focal;
    rayY = (point.y - camera.middleY) / camera.focal;
    return true;
}

// A fixed pseudo-random number in [0, 1) for each (i, j, seed).
float hashOf(int i, int j, int seed) {
    std::uint32_t h = static_cast<std::uint32_t>(i) * 0x8da6b343u ^ static_cast<std::uint32_t>(j) * 0xd8163841u ^
                      static_cast<std::uint32_t>(seed) * 0xcb1ab31fu;
    h ^= h >> 13;
    h *= 0x5bd1e995u;
    h ^= h >> 15;
    return static_cast<float>(h & 0xffffffu) / 16777216.0f;
}

// Smooth noise in [0, 1): random values `spacing` apart, between them smoothly.
float noiseAt(float x, float y, float spacing, int seed) {
    const float u = x / spacing;
    const float v = y / spacing;
    const int i = static_cast<int>(std::floor(u));
    const int j = static_cast<int>(std::floor(v));
    const float fu = u - static_cast<float>(i);
    const float fv = v - static_cast<float>(j);
    const float su = fu * fu * (3.0f - 2.0f * fu);
    const float sv = fv * fv * (3.0f - 2.0f * fv);
    const float top = hashOf(i, j, seed) + su * (hashOf(i + 1, j, seed) - hashOf(i, j, seed));
    const float bottom = hashOf(i, j + 1, seed) + su * (hashOf(i + 1, j + 1, seed) - hashOf(i, j + 1, seed));
    return top + sv * (bottom - top);
}

// A world as `camera` sees it, in grey: textured ground (fine detail fading out where a quarter-size pixel spans too
// much of it), and above the horizon a textured sky infinitely far.
std::vector<std::uint8_t> render(const GroundCamera& camera, int width, int height) {
    std::vector<std::uint8_t> pixels(static_cast<std::size_t>(width) * static_cast<std::size_t>(height));
    const float s = std::sin(camera.pitch);
    const float c = std::cos(camera.pitch);
    const float cy = std::cos(camera.yaw);
    const float sy = std::sin(camera.yaw);
    // The ground point (map x, y) seen through ray (dx, dy); false at or above the horizon.
    const auto ground = [&](float dx, float dy, float& x, float& y) {
        const float below = c * dy + s;
        if (below <= 1e-4f) {
            return false;
        }
        const float t = (kGroundTarget + s * camera.distance) / below;
        const float right = t * dx;
        const float away = -s * t * dy + c * (t - camera.distance);
        x = camera.feetX + cy * right + sy * away;
        y = camera.feetY - sy * right + cy * away;
        return true;
    };
    for (int row = 0; row < height; ++row) {
        const float dy = (static_cast<float>(row) + 0.5f - camera.middleY) / camera.focal;
        for (int column = 0; column < width; ++column) {
            const float dx = (static_cast<float>(column) + 0.5f - camera.middleX) / camera.focal;
            float grey = 0.0f;
            float x = 0.0f;
            float y = 0.0f;
            float nextX = 0.0f;
            float nextY = 0.0f;
            if (ground(dx, dy, x, y) && ground(dx, dy + 1.0f / camera.focal, nextX, nextY)) {
                const float quarter = 4.0f * std::hypot(nextX - x, nextY - y); // Ground a quarter-size pixel spans.
                const auto weight = [quarter](float spacing) {
                    return std::clamp(2.0f - 4.0f * quarter / spacing, 0.0f, 1.0f);
                };
                grey = 128.0f + 110.0f * weight(1.0f) * (noiseAt(x, y, 1.0f, 1) - 0.5f) +
                       90.0f * weight(0.3f) * (noiseAt(x, y, 0.3f, 2) - 0.5f);
            } else {
                // The sky's direction on the map: across (turning) and up.
                const float right = dx;
                const float away = -s * dy + c;
                const float up = -c * dy - s;
                const float azimuth = std::atan2(cy * right + sy * away, -sy * right + cy * away);
                const float elevation = std::atan2(up, std::hypot(right, away));
                grey = 170.0f + 120.0f * (noiseAt(azimuth, elevation, 0.02f, 3) - 0.5f);
            }
            pixels[static_cast<std::size_t>(row) * static_cast<std::size_t>(width) + static_cast<std::size_t>(column)] =
                static_cast<std::uint8_t>(std::clamp(grey, 0.0f, 255.0f));
        }
    }
    return pixels;
}

// Rays of world points seen by both cameras: on the ground round the feet, on things standing on it, far away.
std::vector<TurnPitch::Ray2> raysOf(const GroundCamera& first, const GroundCamera& second, float noise) {
    std::vector<TurnPitch::Ray2> rays;
    int n = 0;
    for (int i = 0; i < 4000 && rays.size() < 600; ++i) {
        const float kind = hashOf(i, 0, 7);
        const float angle = 6.2831853f * hashOf(i, 1, 7);
        const float reach = kind < 0.6f ? 1.0f + 30.0f * hashOf(i, 2, 7) : 40.0f + 300.0f * hashOf(i, 2, 7);
        const float up = kind < 0.4f ? 0.0f : kind < 0.6f ? 2.0f * hashOf(i, 3, 7) : 25.0f * hashOf(i, 3, 7);
        const float x = first.feetX + reach * std::sin(angle);
        const float y = first.feetY + reach * std::cos(angle);
        TurnPitch::Ray2 ray;
        if (!seenAs(first, x, y, up, ray.x1, ray.y1) || !seenAs(second, x, y, up, ray.x2, ray.y2)) {
            continue;
        }
        const double off = static_cast<double>(noise / first.focal);
        ray.x2 += off * (hashOf(n, 4, 7) - 0.5) * 2.0;
        ray.y2 += off * (hashOf(n, 5, 7) - 0.5) * 2.0;
        ++n;
        rays.push_back(ray);
    }
    return rays;
}

} // namespace

TEST_CASE("the turns' fit finds the pitch, the turn and the tilt from points seen twice") {
    for (const float pitch : {0.05f, 0.3f, 0.6f, 1.0f}) {
        CAPTURE(pitch);
        const GroundCamera first = cameraAt(pitch, 0.4f, 1.0f, -2.0f);
        // Turned 1.5 degrees, tilted 0.25, the feet gone a little (walking).
        const GroundCamera second = cameraAt(pitch + 0.0044f, 0.4f + 0.026f, 1.05f, -1.9f);
        const std::vector<TurnPitch::Ray2> rays = raysOf(first, second, 0.0f);
        REQUIRE(rays.size() > 200);
        const TurnPitch::Pair pair = TurnPitch::fit(rays, first.focal, pitch + 0.08f, 0.02f, 0.0f);
        REQUIRE(pair.fitted);
        CHECK(std::fabs(pair.pitch - pitch) < 0.01f * kDegree);
        CHECK(std::fabs(pair.turn - 0.026f) < 0.005f * kDegree);
        CHECK(std::fabs(pair.tilt - 0.0044f) < 0.005f * kDegree);
        CHECK(pair.spread < 0.01f * kDegree);
    }
}

TEST_CASE("the turns' fit is as sure as the points are: a third of a pixel off says the pitch to a tenth of a degree") {
    const GroundCamera first = cameraAt(0.35f, 0.0f, 0.0f, 0.0f);
    const GroundCamera second = cameraAt(0.35f - 0.002f, 0.02f, 0.0f, 0.0f);
    const std::vector<TurnPitch::Ray2> rays = raysOf(first, second, 0.6f); // Up to 0.6 pixels off each way.
    const TurnPitch::Pair pair = TurnPitch::fit(rays, first.focal, 0.25f, 0.01f, 0.0f);
    REQUIRE(pair.fitted);
    CHECK(std::fabs(pair.pitch - 0.35f) < 0.1f * kDegree);
    CHECK(pair.spread > 0.001f * kDegree);
    CHECK(pair.spread < 0.1f * kDegree);
    CHECK(std::fabs(pair.pitch - 0.35f) < 4.0f * pair.spread);
}

TEST_CASE("without a turn the pitch is not sure, a tilt still is") {
    const GroundCamera first = cameraAt(0.35f, 0.0f, 0.0f, 0.0f);
    const GroundCamera second = cameraAt(0.37f, 0.0f, 0.0f, 0.0f);
    const std::vector<TurnPitch::Ray2> rays = raysOf(first, second, 0.3f);
    const TurnPitch::Pair pair = TurnPitch::fit(rays, first.focal, 0.3f, 0.0f, 0.0f);
    REQUIRE(pair.fitted);
    CHECK(std::fabs(pair.tilt - 0.02f) < 0.02f * kDegree);
    CHECK(pair.spread > TurnHold::kSaySpread);
}

TEST_CASE("points followed on rendered images say the pitch while the camera turns") {
    // The game's screen, the camera turning 3 degrees between two follows (a slow turn at 15 follows a second): the
    // less it turns the less sure (1.5 degrees on a screen half as wide: only to 0.2 degrees at 17 looking down).
    constexpr int kWidth = 2560;
    constexpr int kHeight = 1440;
    for (const float pitch : {0.3f, 0.8f}) {
        CAPTURE(pitch);
        const GroundCamera first = cameraAt(pitch, 0.2f, 0.0f, 0.0f, kWidth, kHeight);
        const GroundCamera second = cameraAt(pitch + 0.004f, 0.2f + 0.052f, 0.03f, 0.05f, kWidth, kHeight);
        const std::vector<std::uint8_t> one = render(first, kWidth, kHeight);
        const std::vector<std::uint8_t> two = render(second, kWidth, kHeight);
        GreyImage a;
        GreyImage b;
        a.fromGrey(one.data(), kWidth, kHeight, kWidth);
        b.fromGrey(two.data(), kWidth, kHeight, kWidth);
        TurnPitch turns;
        turns.learn(a.view(1), {});
        REQUIRE(turns.learnedCount() > 300);
        // The follower's camera 3 degrees off, the turn and the tilt guessed short.
        const TurnPitch::Pair pair = turns.follow(b.view(2), first, pitch + 3.0f * kDegree, 0.04f, 0.0f);
        REQUIRE(pair.fitted);
        CHECK(pair.points > 300);
        // Looking down a turn spins the picture less round where the pitch shows (as its cosine), and the texture
        // here is smoother than the game's: at 46 degrees it says the pitch only to about 0.13.
        CHECK(pair.spread < (pitch < 0.5f ? 0.08f : 0.16f) * kDegree);
        CHECK(std::fabs(pair.pitch - pitch) < std::max(0.1f * kDegree, 3.0f * pair.spread));
        CHECK(std::fabs(pair.turn - 0.052f) < 0.02f * kDegree);
        CHECK(std::fabs(pair.tilt - 0.004f) < 0.02f * kDegree);
        CHECK(pair.miss < 1.0f);
    }
}

TEST_CASE("the interface left out is not followed") {
    constexpr int kWidth = 1280;
    constexpr int kHeight = 720;
    const GroundCamera camera = cameraAt(0.5f, 0.0f, 0.0f, 0.0f, kWidth, kHeight);
    const std::vector<std::uint8_t> one = render(camera, kWidth, kHeight);
    GreyImage a;
    a.fromGrey(one.data(), kWidth, kHeight, kWidth);
    TurnPitch all;
    all.learn(a.view(1), {});
    const GreyArea half{0, 0, kWidth / 2, kHeight};
    TurnPitch some;
    some.learn(a.view(1), std::span<const GreyArea>(&half, 1));
    CHECK(some.learnedCount() < all.learnedCount() * 6 / 10);
    CHECK(some.learnedCount() > all.learnedCount() * 3 / 10);
}

TEST_CASE("the turns hold the pitch once sure, carry it on by the tilts, and take it back when lost") {
    TurnHold hold;
    TurnPitch::Pair still;
    still.fitted = true;
    still.agreeing = 300;
    still.turn = 0.0f;
    still.tilt = 0.01f;
    still.spread = 0.05f;
    CHECK_FALSE(hold.pair(still, 0.40f, 0.012f)); // Not turning: says nothing.
    CHECK_FALSE(hold.holds());

    TurnPitch::Pair turning;
    turning.fitted = true;
    turning.agreeing = 300;
    turning.turn = 0.03f;
    turning.pitch = 0.30f;
    turning.tilt = 0.0f;
    turning.spread = 0.002f;
    CHECK(hold.pair(turning, 0.33f, 0.0f)); // The follower 1.7 degrees off.
    CHECK(hold.holds());
    CHECK(hold.pitch() == doctest::Approx(0.30f).epsilon(0.002));

    // Tilts carry it on.
    TurnPitch::Pair tilting = still;
    tilting.tilt = 0.05f;
    CHECK_FALSE(hold.pair(tilting, 0.0f, 0.06f));
    CHECK(hold.holds());
    CHECK(hold.pitch() == doctest::Approx(0.35f).epsilon(0.002));

    // One turn far off is not taken; two in a row are, if they agree.
    TurnPitch::Pair other = turning;
    other.pitch = 0.45f;
    TurnPitch::Pair wrong = turning;
    wrong.pitch = -1.45f;
    CHECK_FALSE(hold.pair(wrong, 0.0f, 0.0f));
    CHECK_FALSE(hold.pair(other, 0.0f, 0.0f));
    CHECK(hold.pitch() == doctest::Approx(0.35f).epsilon(0.002));
    CHECK(hold.pair(other, 0.0f, 0.0f));
    CHECK(hold.pitch() == doctest::Approx(0.45f).epsilon(0.002));

    // A follow lost (a fast flick, blurred: few points) no longer holds: the follower's pitch stands until a turn
    // says it again.
    TurnPitch::Pair blurred = turning;
    blurred.agreeing = 44;
    CHECK_FALSE(hold.pair(blurred, 0.0f, 0.0f));
    CHECK_FALSE(hold.holds());
    TurnPitch::Pair after = turning;
    after.pitch = 0.10f;
    CHECK(hold.pair(after, 0.12f, 0.0f));
    CHECK(hold.holds());
    CHECK(hold.pitch() == doctest::Approx(0.10f).epsilon(0.003));
}
