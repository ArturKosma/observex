#include "heuristics/Snapper.h"

#include <doctest/doctest.h>

#include <cmath>
#include <cstdint>
#include <vector>

using namespace observex;

namespace {

constexpr int kWidth = 320;
constexpr int kHeight = 200;

// Grass (noise) seen with the camera turned `turn` pixels (the world shows that much further right), and a striped
// thing `thingWidth` x `thingHeight` at `thingX`, `thingY` (partly off the image if it reaches over the edge).
std::vector<std::uint8_t> scene(int turn, int thingX, int thingY, int thingWidth = 30, int thingHeight = 28) {
    std::vector<std::uint8_t> pixels(static_cast<std::size_t>(kWidth * kHeight));
    for (int y = 0; y < kHeight; ++y) {
        for (int x = 0; x < kWidth; ++x) {
            const auto world = static_cast<std::uint32_t>(x - turn + 1000);
            const std::uint32_t hash = (world * 73856093u ^ static_cast<std::uint32_t>(y) * 19349663u) * 2654435761u;
            pixels[static_cast<std::size_t>(y * kWidth + x)] = static_cast<std::uint8_t>(70 + (hash >> 24) % 60);
        }
    }
    for (int y = 0; y < thingHeight; ++y) {
        for (int x = 0; x < thingWidth; ++x) {
            const int px = thingX + x;
            const int py = thingY + y;
            if (px >= 0 && px < kWidth && py >= 0 && py < kHeight) {
                pixels[static_cast<std::size_t>(py * kWidth + px)] =
                    static_cast<std::uint8_t>((x / 4 + y / 5) % 2 == 0 ? 20 + (4 * y) % 120 : 230 - (3 * x) % 120);
            }
        }
    }
    return pixels;
}

GreyImage grey(const std::vector<std::uint8_t>& pixels) {
    GreyImage image;
    image.fromGrey(pixels.data(), kWidth, kHeight, kWidth);
    return image;
}

} // namespace

TEST_CASE("a thing is found where it moved to on a newer image, to the pixel") {
    Snapper snapper;
    snapper.learn(grey(scene(0, 100, 80)).view(), 7, Detection{100, 80, 30, 28, 0.9f});
    REQUIRE(snapper.knows(7));
    // The camera turned 37 pixels and the thing walked 5 up; the guess is 17 pixels short.
    Snapper::Found found;
    REQUIRE(snapper.find(grey(scene(37, 137, 75)).view(), 7, 120.0f, 80.0f, 40.0f, found));
    CHECK(std::abs(found.x - 137.0f) < 0.5f);
    CHECK(std::abs(found.y - 75.0f) < 0.5f);
    CHECK(found.match > 0.9f);
}

TEST_CASE("nothing is found where the thing is not, nor for a thing never learned") {
    Snapper snapper;
    snapper.learn(grey(scene(0, 100, 80)).view(), 7, Detection{100, 80, 30, 28, 0.9f});
    Snapper::Found found;
    CHECK_FALSE(snapper.find(grey(scene(0, 250, 150)).view(), 7, 100.0f, 80.0f, 40.0f, found)); // Only grass within reach.
    CHECK_FALSE(snapper.find(grey(scene(0, 100, 80)).view(), 8, 100.0f, 80.0f, 40.0f, found));
    snapper.keepOnly({});
    CHECK_FALSE(snapper.knows(7));
}

TEST_CASE("a thing cut by the screen's edge is learned from what shows of it") {
    Snapper snapper;
    snapper.learn(grey(scene(0, -10, 80)).view(), 7, Detection{-10, 80, 30, 28, 0.9f}); // A third of it off the left edge.
    REQUIRE(snapper.knows(7));
    Snapper::Found found;
    REQUIRE(snapper.find(grey(scene(0, 20, 80)).view(), 7, 0.0f, 80.0f, 40.0f, found));
    CHECK(std::abs(found.x - 20.0f) < 0.5f);
    CHECK(std::abs(found.y - 80.0f) < 0.5f);
}

TEST_CASE("a big thing is found as precisely, though matched only down to a halving") {
    // 110 x 96: matched down to the half-size image (Snapper::kFineSide), to a fraction of a pixel there.
    Snapper snapper;
    snapper.learn(grey(scene(0, 60, 40, 110, 96)).view(), 7, Detection{60, 40, 110, 96, 0.9f});
    Snapper::Found found;
    REQUIRE(snapper.find(grey(scene(23, 83, 47, 110, 96)).view(), 7, 70.0f, 40.0f, 40.0f, found));
    CHECK(std::abs(found.x - 83.0f) < 1.0f);
    CHECK(std::abs(found.y - 47.0f) < 1.0f);
}

TEST_CASE("a flat box is not learned: there is nothing to find it by") {
    std::vector<std::uint8_t> pixels(static_cast<std::size_t>(kWidth * kHeight), 100);
    Snapper snapper;
    snapper.learn(grey(pixels).view(), 7, Detection{100, 80, 30, 28, 0.9f});
    CHECK_FALSE(snapper.knows(7));
}
