#include "core/ClipGuesses.h"
#include "core/ClipLines.h"
#include "core/Ground.h"
#include "heuristics/DepthTerrain.h"

#include <doctest/doctest.h>

#include <cmath>
#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <string>
#include <vector>

using namespace observex;

namespace {

constexpr float kWidth = 2560.0f;
constexpr float kHeight = 1440.0f;

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
    return static_cast<std::uint16_t>((static_cast<std::uint32_t>(exponent) << 10) +
                                      (((bits & 0x7FFFFFu) + 0x1000u) >> 13));
}

// The terrain's height (own heights) at a point of the ground's map.
using Terrain = std::function<float(float mapX, float mapY)>;

// The depth map `camera` sees of `terrain` on a 2560 x 1440 screen, as the network would give it: `scale` times the true
// inverse depth (along where it looks, own heights) plus `offset`; the sky `offset`. The network's size.
struct RenderedDepth {
    int width = 924;
    int height = 518;
    std::vector<std::uint16_t> map;

    RenderedDepth(const GroundCamera& camera, const Terrain& terrain, float scale, float offset) {
        map.resize(static_cast<std::size_t>(width * height));
        const float s = std::sin(camera.pitch);
        const float c = std::cos(camera.pitch);
        const float eye = kGroundTarget + camera.distance * s;
        for (int y = 0; y < height; ++y) {
            for (int x = 0; x < width; ++x) {
                const float ax = ((static_cast<float>(x) + 0.5f) * kWidth / static_cast<float>(width) - camera.middleX) /
                                 camera.focal;
                const float b = ((static_cast<float>(y) + 0.5f) * kHeight / static_cast<float>(height) - camera.middleY) /
                                camera.focal;
                // Above the terrain at `ahead` along the ray?
                const auto above = [&](float ahead) {
                    const float up = eye - ahead * (b * c + s);
                    const float right = ahead * ax;
                    const float away = -camera.distance * c + ahead * (c - b * s);
                    float mapX = 0.0f;
                    float mapY = 0.0f;
                    groundToMap(camera, right, away, mapX, mapY);
                    return up > terrain(mapX, mapY);
                };
                float near = 0.3f;
                float hit = -1.0f;
                for (float ahead = near; ahead < 200.0f; ahead = ahead * 1.04f + 0.02f) {
                    if (!above(ahead)) {
                        float low = near;
                        float high = ahead;
                        for (int i = 0; i < 20; ++i) {
                            const float middle = 0.5f * (low + high);
                            (above(middle) ? low : high) = middle;
                        }
                        hit = 0.5f * (low + high);
                        break;
                    }
                    near = ahead;
                }
                map[static_cast<std::size_t>(y * width + x)] = toHalf(hit > 0.0f ? scale / hit + offset : offset);
            }
        }
    }

    DepthInput input() const {
        DepthInput in;
        in.map = map.data();
        in.width = width;
        in.height = height;
        in.screenWidth = static_cast<int>(kWidth);
        in.screenHeight = static_cast<int>(kHeight);
        return in;
    }
};

GroundCamera cameraAt(float pitch, float distance, float yaw = 0.0f, float feetX = 0.0f, float feetY = 0.0f) {
    GroundCamera camera = groundCamera(pitch, distance, kWidth, kHeight);
    camera.pitchMeasured = true;
    camera.yaw = yaw;
    camera.feetX = feetX;
    camera.feetY = feetY;
    return camera;
}

// A round hill `top` own heights high at map point (x, y), `radius` wide (a Gaussian's).
Terrain hill(float x, float y, float top, float radius) {
    return [=](float mapX, float mapY) {
        const float d2 = (mapX - x) * (mapX - x) + (mapY - y) * (mapY - y);
        return top * std::exp(-d2 / (2.0f * radius * radius));
    };
}

} // namespace

TEST_CASE("depth terrain: flat ground comes out flat, the network's scale and offset found") {
    const GroundCamera camera = cameraAt(0.40f, 7.0f);
    const RenderedDepth depth(camera, [](float, float) { return 0.0f; }, 3.0f, 0.2f);
    DepthTerrain terrain;
    GroundDepthNote note;
    REQUIRE(terrain.take(depth.input(), 0.0, camera, {}, note));
    CHECK(note.shaped);
    CHECK(note.shapeScale == doctest::Approx(3.0f).epsilon(0.02));
    CHECK(note.shapeOffset == doctest::Approx(0.2f).epsilon(0.05));
    const GroundHeights& heights = terrain.heights();
    CHECK(heights.known);
    CHECK(heights.version > 0);
    for (const float away : {1.0f, 4.0f, 10.0f, 18.0f}) {
        for (const float right : {-6.0f, 0.0f, 6.0f}) {
            CHECK(std::fabs(groundHeight(heights, camera, right, away)) < 0.08f);
        }
    }
}

TEST_CASE("depth terrain: a hill ahead stands up where it is") {
    const GroundCamera camera = cameraAt(0.35f, 7.0f);
    const Terrain terrain = hill(1.0f, 14.0f, 2.0f, 2.5f);
    const RenderedDepth depth(camera, terrain, 4.0f, -0.1f);
    DepthTerrain shape;
    GroundDepthNote note;
    REQUIRE(shape.take(depth.input(), 0.0, camera, {}, note));
    const GroundHeights& heights = shape.heights();
    // Its front up to near the top, and its sides. The top itself is seen so flat that few samples land on it, and the
    // back is hidden: both are filled from round them (lower).
    CHECK(groundHeight(heights, camera, 1.0f, 13.0f) == doctest::Approx(terrain(1.0f, 13.0f)).epsilon(0.15));
    CHECK(groundHeight(heights, camera, 1.0f, 11.5f) == doctest::Approx(terrain(1.0f, 11.5f)).epsilon(0.2));
    CHECK(groundHeight(heights, camera, 1.0f, 14.0f) > 1.0f);
    CHECK(groundHeight(heights, camera, 4.0f, 14.0f) == doctest::Approx(terrain(4.0f, 14.0f)).epsilon(0.25));
    // Flat round it.
    CHECK(std::fabs(groundHeight(heights, camera, -8.0f, 6.0f)) < 0.1f);
    CHECK(std::fabs(groundHeight(heights, camera, 0.0f, 3.0f)) < 0.1f);
}

TEST_CASE("depth terrain: a tree trunk stays a trunk, the ground under it flat") {
    const GroundCamera camera = cameraAt(0.30f, 7.0f);
    // A trunk 6 own heights tall and 1 wide, 10 ahead and 1 to the right, on flat ground.
    const Terrain trunk = [](float mapX, float mapY) { return std::hypot(mapX - 1.0f, mapY - 10.0f) < 0.5f ? 6.0f : 0.0f; };
    DepthTerrain shape;
    GroundDepthNote note;
    REQUIRE(shape.take(RenderedDepth(camera, trunk, 3.0f, 0.1f).input(), 0.0, camera, {}, note));
    for (const float away : {9.0f, 10.0f, 11.0f, 13.0f}) {
        for (const float right : {0.0f, 1.0f, 2.0f}) {
            CHECK(std::fabs(groundHeight(shape.heights(), camera, right, away)) < 0.3f);
        }
    }
}

TEST_CASE("depth terrain: a turned and moved camera puts the hill at its place on the map") {
    const GroundCamera turned = cameraAt(0.45f, 6.0f, 0.6f, 2.0f, -1.0f);
    float hillX = 0.0f; // The hill 2 right of the feet and 9 away, as the turned camera sees it.
    float hillY = 0.0f;
    groundToMap(turned, 2.0f, 9.0f, hillX, hillY);
    float right = 0.0f;
    float away = 0.0f;
    mapToGround(turned, hillX, hillY, right, away);
    CHECK(right == doctest::Approx(2.0f).epsilon(0.001));
    CHECK(away == doctest::Approx(9.0f).epsilon(0.001));
    const Terrain terrain = hill(hillX, hillY, 1.5f, 2.0f);
    const RenderedDepth depth(turned, terrain, 2.5f, 0.05f);
    DepthTerrain shape;
    GroundDepthNote note;
    REQUIRE(shape.take(depth.input(), 0.0, turned, {}, note));
    CHECK(groundHeight(shape.heights(), turned, hillX, hillY) == doctest::Approx(1.5f).epsilon(0.2));
    CHECK(std::fabs(groundHeight(shape.heights(), turned, 8.0f, -8.0f)) < 0.1f); // Behind the camera: flat.
}

TEST_CASE("depth terrain: walking up, the heights stay on the ground and the feet on it") {
    // A slope rising away along the map's y, flat round the start; the camera then stands on it.
    const Terrain slope = [](float, float mapY) { return mapY > 6.0f ? 0.25f * (mapY - 6.0f) : 0.0f; };
    const GroundCamera first = cameraAt(0.40f, 7.0f);
    DepthTerrain shape;
    GroundDepthNote note;
    REQUIRE(shape.take(RenderedDepth(first, slope, 3.0f, 0.1f).input(), 0.0, first, {}, note));
    CHECK(groundHeight(shape.heights(), first, 0.0f, 14.0f) == doctest::Approx(2.0f).epsilon(0.15));
    // Walked to y = 10 (1 own height up): the terrain there is the feet's.
    const GroundCamera second = cameraAt(0.40f, 7.0f, 0.0f, 0.0f, 10.0f);
    CHECK(groundHeight(shape.heights(), second, 0.0f, 10.0f) == doctest::Approx(0.0f));
    CHECK(groundHeight(shape.heights(), second, 0.0f, 0.0f) == doctest::Approx(-1.0f).epsilon(0.15));
}

TEST_CASE("depth terrain: looking straight down tells nothing, clearing forgets") {
    DepthTerrain shape;
    GroundDepthNote note;
    const GroundCamera down = cameraAt(1.50f, 7.0f);
    CHECK_FALSE(shape.take(RenderedDepth(down, [](float, float) { return 0.0f; }, 3.0f, 0.1f).input(), 0.0, down, {},
                           note));
    CHECK_FALSE(shape.heights().known);
    const GroundCamera camera = cameraAt(0.40f, 7.0f);
    REQUIRE(shape.take(RenderedDepth(camera, hill(0.0f, 12.0f, 1.0f, 2.0f), 3.0f, 0.1f).input(), 0.0, camera, {},
                       note));
    const std::uint64_t version = shape.heights().version;
    shape.clear();
    CHECK_FALSE(shape.heights().known);
    CHECK(shape.heights().version > version);
    CHECK(groundHeight(shape.heights(), camera, 0.0f, 12.0f) == 0.0f);
}

TEST_CASE("heights lines read back, and say which terrain the grid lay on at each time") {
    std::string text;
    appendHeights(text, 1200, 33.5, 1);
    appendHeights(text, 1203, 83.25, 2);
    appendHeights(text, 1207, 150.0, 0);
    const ClipText clip = readClipText(text);
    REQUIRE(clip.heights.size() == 3);
    CHECK(clip.heights[1].image == 1203);
    CHECK(clip.heights[1].timeMs == 83.25);
    CHECK(recordedHeights(clip.heights, 10.0) == 0);
    CHECK(recordedHeights(clip.heights, 33.5) == 1);
    CHECK(recordedHeights(clip.heights, 100.0) == 2);
    CHECK(recordedHeights(clip.heights, 200.0) == 0);
    unsigned long long image = 0;
    double timeMs = 0.0;
    unsigned long long terrain = 0;
    CHECK_FALSE(readHeights("height 1203 83.25 2", image, timeMs, terrain));
    CHECK_FALSE(readHeights("heights 1203", image, timeMs, terrain));
}

TEST_CASE("front codes: distances ahead kept to a few percent, too far nothing") {
    for (const float ahead : {0.3f, 1.0f, 5.0f, 12.5f, 37.0f, 250.0f}) {
        CHECK(frontAhead(frontCode(ahead)) == doctest::Approx(ahead).epsilon(0.02));
    }
    CHECK(frontCode(0.1f) == 1);
    CHECK(frontCode(1000.0f) == 0);
    CHECK(frontAhead(0) > 1e20f);
}

TEST_CASE("depth terrain: a trunk hides the ground behind it, not in front of it or beside it") {
    const GroundCamera camera = cameraAt(0.30f, 7.0f);
    // A trunk 6 own heights tall and 1 wide, 10 ahead and 1 to the right, on flat ground.
    const Terrain trunk = [](float mapX, float mapY) { return std::hypot(mapX - 1.0f, mapY - 10.0f) < 0.5f ? 6.0f : 0.0f; };
    DepthTerrain shape;
    GroundDepthNote note;
    REQUIRE(shape.take(RenderedDepth(camera, trunk, 3.0f, 0.1f).input(), 0.0, camera, {}, note));
    const GroundHeights& heights = shape.heights();
    const auto hidden = [&](float mapX, float mapY) {
        return groundHidden(heights, mapX, mapY, heightOnMap(heights, mapX, mapY));
    };
    CHECK(hidden(1.0f, 14.0f)); // Straight behind it, as the eye sees it.
    CHECK(hidden(1.2f, 18.0f));
    CHECK_FALSE(hidden(1.0f, 8.0f)); // In front.
    CHECK_FALSE(hidden(-3.0f, 14.0f)); // Beside.
    CHECK_FALSE(hidden(5.0f, 14.0f));
    CHECK_FALSE(hidden(0.0f, 4.0f));
    // On the grid: only what is behind it.
    std::vector<GridPoint> grid(kGridPoints);
    groundGrid(camera, &heights, grid);
    int ok = 0;
    int behind = 0;
    for (const GridPoint& point : grid) {
        ok += point.ok ? 1 : 0;
        behind += point.ok && point.hidden ? 1 : 0;
    }
    CHECK(behind > 5);
    CHECK(behind < ok / 30);
    // Flat ground alone hides nothing.
    DepthTerrain flat;
    REQUIRE(flat.take(RenderedDepth(camera, [](float, float) { return 0.0f; }, 3.0f, 0.1f).input(), 0.0, camera, {},
                      note));
    groundGrid(camera, &flat.heights(), grid);
    int flatHidden = 0;
    for (const GridPoint& point : grid) {
        flatHidden += point.ok && point.hidden ? 1 : 0;
    }
    CHECK(flatHidden == 0);
}

TEST_CASE("depth terrain: a trunk thinner than a piece of the grid cuts it in the middle") {
    const GroundCamera camera = cameraAt(0.30f, 7.0f);
    // 0.2 wide, 10 ahead and 1 right: its shadow on the line 14 ahead is about 1.12 to 1.36 right.
    const Terrain trunk = [](float mapX, float mapY) { return std::hypot(mapX - 1.0f, mapY - 10.0f) < 0.1f ? 6.0f : 0.0f; };
    DepthTerrain shape;
    GroundDepthNote note;
    REQUIRE(shape.take(RenderedDepth(camera, trunk, 3.0f, 0.1f).input(), 0.0, camera, {}, note));
    std::vector<GridPoint> grid(kGridPoints);
    groundGrid(camera, &shape.heights(), grid);
    // The grid starts kGridCells cells before the feet's cell, in steps of a kGridSteps-th of a cell.
    const auto index = [](float mapAt) {
        return static_cast<int>(std::lround((mapAt + static_cast<float>(kGridCells) * kGridCell) * kGridSteps / kGridCell));
    };
    const GridPoint& a = grid[static_cast<std::size_t>(index(14.0f) * kGridSide + index(1.0f))];
    const GridPoint& b = grid[static_cast<std::size_t>(index(14.0f) * kGridSide + index(1.5f))];
    REQUIRE(a.ok);
    REQUIRE(b.ok);
    CHECK_FALSE(a.hidden);
    CHECK_FALSE(b.hidden);
    const int count = gridStepCount(a, b);
    CHECK(count >= 4);
    std::vector<GridStep> steps(static_cast<std::size_t>(kGridMostSteps + 1));
    gridSteps(shape.heights(), a, b, count, steps);
    CHECK(steps[0].x == doctest::Approx(a.x));
    CHECK(steps[static_cast<std::size_t>(count)].y == doctest::Approx(b.y));
    int cut = 0;
    for (int k = 0; k <= count; ++k) {
        cut += steps[static_cast<std::size_t>(k)].hidden ? 1 : 0;
    }
    CHECK(cut >= 1);
    CHECK(cut < count);
    // Without a front nothing is cut, in one step.
    const auto none = std::make_unique<GroundHeights>(shape.heights());
    none->frontCamera.known = false;
    groundGrid(camera, none.get(), grid);
    CHECK(gridStepCount(grid[static_cast<std::size_t>(index(14.0f) * kGridSide + index(1.0f))],
                        grid[static_cast<std::size_t>(index(14.0f) * kGridSide + index(1.5f))]) == 1);
}
