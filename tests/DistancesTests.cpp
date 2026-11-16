#include "core/Distances.h"

#include <doctest/doctest.h>

#include <cmath>
#include <memory>

using namespace observex;

namespace {

// The camera the user's clips mostly show: 2560 x 1440, looking down 25 degrees from 7 own heights, turned a little.
GroundCamera clipCamera() {
    GroundCamera camera = groundCamera(0.436f, 7.0f, 2560.0f, 1440.0f);
    camera.yaw = 0.4f;
    camera.feetX = 3.0f;
    camera.feetY = -2.0f;
    return camera;
}

// A terrain round `camera`'s feet whose height is `slope` x the map's y (own heights).
std::unique_ptr<GroundHeights> sloped(const GroundCamera& camera, float slope) {
    auto heights = std::make_unique<GroundHeights>();
    heights->known = true;
    heights->version = 1;
    const float half = 0.5f * GroundHeights::kCell * static_cast<float>(GroundHeights::kCells - 1);
    heights->firstX = camera.feetX - half;
    heights->firstY = camera.feetY - half;
    for (int y = 0; y < GroundHeights::kCells; ++y) {
        for (int x = 0; x < GroundHeights::kCells; ++x) {
            const float mapY = heights->firstY + GroundHeights::kCell * static_cast<float>(y);
            heights->height[static_cast<std::size_t>(y * GroundHeights::kCells + x)] = slope * mapY;
        }
    }
    return heights;
}

// A box `tall` pixels tall standing with its bottom middle on screen point `foot`.
Detection boxOn(const GroundPoint& foot, float tall) {
    Detection box;
    box.width = tall * 0.5f;
    box.height = tall;
    box.x = foot.x - box.width * 0.5f;
    box.y = foot.y - tall;
    box.sure = 0.9f;
    return box;
}

} // namespace

TEST_CASE("Distances: a point on the flat ground is found where it was put") {
    const GroundCamera camera = clipCamera();
    for (const auto& [right, away] : {std::pair{0.0f, 5.0f}, {-4.0f, 12.0f}, {6.0f, 2.0f}, {1.0f, 30.0f}}) {
        const GroundPoint shown = groundToScreen(camera, right, away);
        REQUIRE(shown.ahead);
        const std::optional<GroundSpot> spot = groundUnder(camera, nullptr, shown.x, shown.y);
        REQUIRE(spot.has_value());
        CHECK(spot->right == doctest::Approx(right).epsilon(1e-3));
        CHECK(spot->away == doctest::Approx(away).epsilon(1e-3));
        CHECK(spot->up == 0.0f);
        CHECK(alongGround(*spot) == doctest::Approx(std::hypot(right, away)).epsilon(1e-3));
        const std::optional<float> distance = characterDistance(camera, nullptr, boxOn(shown, 120.0f));
        REQUIRE(distance.has_value());
        CHECK(*distance == doctest::Approx(std::hypot(right, away)).epsilon(1e-3));
    }
}

TEST_CASE("Distances: nothing at or above the horizon, nor with the camera unknown") {
    const GroundCamera camera = clipCamera();
    const float horizon = horizonRow(camera);
    CHECK_FALSE(groundUnder(camera, nullptr, 1280.0f, horizon - 5.0f).has_value());
    CHECK_FALSE(groundUnder(camera, nullptr, 1280.0f, horizon).has_value());
    CHECK(groundUnder(camera, nullptr, 1280.0f, horizon + 40.0f).has_value());
    GroundCamera unknown = camera;
    unknown.known = false;
    CHECK_FALSE(groundUnder(unknown, nullptr, 1280.0f, 1000.0f).has_value());
}

TEST_CASE("Distances: none said farther than kFarthestDistance") {
    const GroundCamera camera = clipCamera();
    const GroundPoint near = groundToScreen(camera, 0.0f, kFarthestDistance - 1.0f);
    const GroundPoint far = groundToScreen(camera, 0.0f, kFarthestDistance + 1.0f);
    CHECK(characterDistance(camera, nullptr, boxOn(near, 20.0f)).has_value());
    CHECK_FALSE(characterDistance(camera, nullptr, boxOn(far, 20.0f)).has_value());
    CHECK(groundUnder(camera, nullptr, far.x, far.y).has_value()); // The ground is still there.
}

TEST_CASE("Distances: the terrain not known is the flat ground") {
    const GroundCamera camera = clipCamera();
    auto heights = sloped(camera, 0.3f);
    heights->known = false;
    const GroundPoint shown = groundToScreen(camera, 2.0f, 9.0f);
    const std::optional<GroundSpot> spot = groundUnder(camera, heights.get(), shown.x, shown.y);
    REQUIRE(spot.has_value());
    CHECK(spot->away == doctest::Approx(9.0f).epsilon(1e-3));
}

TEST_CASE("Distances: a thing on a slope is found on it, not on the flat ground") {
    const GroundCamera camera = clipCamera();
    for (const float slope : {0.25f, -0.2f}) {
        const auto heights = sloped(camera, slope);
        for (const auto& [right, away] : {std::pair{0.0f, 6.0f}, {-3.0f, 14.0f}, {5.0f, 20.0f}}) {
            float mapX = 0.0f;
            float mapY = 0.0f;
            groundToMap(camera, right, away, mapX, mapY);
            const float up = groundHeight(*heights, camera, mapX, mapY);
            const GroundPoint shown = worldToScreen(camera, right, away, up);
            REQUIRE(shown.ahead);
            const std::optional<GroundSpot> spot = groundUnder(camera, heights.get(), shown.x, shown.y);
            REQUIRE(spot.has_value());
            CHECK(spot->right == doctest::Approx(right).epsilon(2e-3));
            CHECK(spot->away == doctest::Approx(away).epsilon(2e-3));
            CHECK(spot->up == doctest::Approx(up).epsilon(2e-3));
            // The flat ground puts it elsewhere: farther uphill, nearer downhill.
            const std::optional<GroundSpot> flat = groundUnder(camera, nullptr, shown.x, shown.y);
            const float off = flat ? alongGround(*flat) - alongGround(*spot) : 1e9f;
            CHECK(std::fabs(off) > 0.1f);
        }
    }
}

TEST_CASE("Distances: terrain over the camera's eye behind it is not where things stand") {
    // The terrain under and behind the camera is never seen, only filled: here a wall over the eye behind the feet.
    const GroundCamera camera = clipCamera();
    auto heights = sloped(camera, 0.0f);
    for (int y = 0; y < GroundHeights::kCells; ++y) {
        for (int x = 0; x < GroundHeights::kCells; ++x) {
            float right = 0.0f;
            float away = 0.0f;
            mapToGround(camera, heights->firstX + GroundHeights::kCell * static_cast<float>(x),
                        heights->firstY + GroundHeights::kCell * static_cast<float>(y), right, away);
            heights->height[static_cast<std::size_t>(y * GroundHeights::kCells + x)] = away < -3.0f ? 10.0f : 0.0f;
        }
    }
    const GroundPoint shown = groundToScreen(camera, 1.0f, 8.0f);
    const std::optional<GroundSpot> spot = groundUnder(camera, heights.get(), shown.x, shown.y);
    REQUIRE(spot.has_value());
    CHECK(spot->away == doctest::Approx(8.0f).epsilon(2e-3));
    CHECK(spot->right == doctest::Approx(1.0f).epsilon(2e-3));
}

TEST_CASE("Distances: a drop stands on its middle, a character on its bottom") {
    const GroundCamera camera = clipCamera();
    const GroundPoint shown = groundToScreen(camera, 1.0f, 8.0f);
    Detection drop;
    drop.width = 12.0f;
    drop.height = 8.0f;
    drop.x = shown.x - 6.0f;
    drop.y = shown.y - 4.0f;
    const std::optional<float> distance = dropDistance(camera, nullptr, drop);
    REQUIRE(distance.has_value());
    CHECK(*distance == doctest::Approx(std::hypot(1.0f, 8.0f)).epsilon(1e-3));
    const std::optional<float> asCharacter = characterDistance(camera, nullptr, drop);
    REQUIRE(asCharacter.has_value());
    CHECK(*asCharacter < *distance); // Its bottom is nearer the camera.
}
