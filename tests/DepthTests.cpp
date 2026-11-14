#include "core/ClipGuesses.h"
#include "core/ClipLines.h"
#include "core/Depth.h"
#include "heuristics/DepthRules.h"

#include <doctest/doctest.h>

#include <array>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

using namespace observex;

namespace {

// A float as a half float (rounded towards zero; enough for the test maps' values).
std::uint16_t toHalf(float value) {
    if (value == 0.0f) {
        return 0;
    }
    int exponent = 0;
    const float mantissa = std::frexp(value, &exponent); // value = mantissa * 2^exponent, mantissa in [0.5, 1).
    const auto bits = static_cast<std::uint16_t>((exponent + 14) << 10);
    return static_cast<std::uint16_t>(bits | static_cast<std::uint16_t>((mantissa * 2.0f - 1.0f) * 1024.0f));
}

// A map like the ground's: inverse depth growing evenly down the screen, `width` x `height`.
std::vector<std::uint16_t> groundMap(int width, int height) {
    std::vector<std::uint16_t> map(static_cast<std::size_t>(width) * static_cast<std::size_t>(height));
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            map[static_cast<std::size_t>(y * width + x)] = toHalf(0.5f + 4.0f * static_cast<float>(y) / height);
        }
    }
    return map;
}

DepthInput inputOf(const std::vector<std::uint16_t>& map, int width, int height, const GreyArea* ignore = nullptr,
                   int ignoreCount = 0) {
    DepthInput input;
    input.map = map.data();
    input.width = width;
    input.height = height;
    input.screenWidth = width * 2; // A screen twice the map's size.
    input.screenHeight = height * 2;
    input.ignore = ignore;
    input.ignoreCount = ignoreCount;
    return input;
}

} // namespace

TEST_CASE("half floats read as the depth network wrote them") {
    CHECK(halfToFloat(0x0000) == 0.0f);
    CHECK(halfToFloat(0x3C00) == 1.0f);
    CHECK(halfToFloat(0xC000) == -2.0f);
    CHECK(halfToFloat(0x3555) == doctest::Approx(0.33325195));
    CHECK(halfToFloat(0x7BFF) == 65504.0f);              // The largest.
    CHECK(halfToFloat(0x0001) == std::ldexp(1.0f, -24)); // The smallest, below the normal ones.
    CHECK(std::isinf(halfToFloat(0x7C00)));
    CHECK(toHalf(2.5f) == 0x4100);
}

TEST_CASE("depth shades nearer brighter, and the same map always the same") {
    constexpr int kWidth = 96;
    constexpr int kHeight = 64;
    const std::vector<std::uint16_t> map = groundMap(kWidth, kHeight);
    DepthRules rules;
    std::vector<std::uint8_t> levels(map.size());
    std::array<std::uint32_t, Depthing::kPaletteSize> palette{};
    REQUIRE(rules.shade(inputOf(map, kWidth, kHeight), levels.data(), palette.data()));
    // Down the screen is nearer: the levels grow (the ground alone has no local term).
    CHECK(levels[static_cast<std::size_t>(5 * kWidth + 48)] < levels[static_cast<std::size_t>(30 * kWidth + 48)]);
    CHECK(levels[static_cast<std::size_t>(30 * kWidth + 48)] < levels[static_cast<std::size_t>(58 * kWidth + 48)]);
    // Inferno: black far, pale yellow near.
    CHECK(palette[0] == 0xFF000004u);
    CHECK(palette[255] == 0xFFFCFFA4u);

    std::vector<std::uint8_t> again(map.size());
    REQUIRE(rules.shade(inputOf(map, kWidth, kHeight), again.data(), palette.data()));
    CHECK(again == levels);
    DepthRules fresh;
    REQUIRE(fresh.shade(inputOf(map, kWidth, kHeight), again.data(), palette.data()));
    CHECK(again == levels);
}

TEST_CASE("a small thing a little nearer than the ground round it shows clearly, the ground behind it darker") {
    constexpr int kWidth = 96;
    constexpr int kHeight = 64;
    std::vector<std::uint16_t> map = groundMap(kWidth, kHeight);
    const std::vector<std::uint16_t> plain = map;
    // A distant monster: 3 x 5 map pixels, 10% nearer than the ground at its feet.
    const float feet = 0.5f + 4.0f * 24.0f / kHeight;
    for (int y = 20; y < 25; ++y) {
        for (int x = 40; x < 43; ++x) {
            map[static_cast<std::size_t>(y * kWidth + x)] = toHalf(feet * 1.1f);
        }
    }
    DepthRules rules;
    std::vector<std::uint8_t> levels(map.size());
    std::vector<std::uint8_t> ground(map.size());
    std::array<std::uint32_t, Depthing::kPaletteSize> palette{};
    REQUIRE(rules.shade(inputOf(map, kWidth, kHeight), levels.data(), palette.data()));
    REQUIRE(rules.shade(inputOf(plain, kWidth, kHeight), ground.data(), palette.data()));
    const auto at = [&](const std::vector<std::uint8_t>& of, int x, int y) {
        return static_cast<int>(of[static_cast<std::size_t>(y * kWidth + x)]);
    };
    CHECK(at(levels, 41, 22) >= at(ground, 41, 22) + 25);      // The monster, against the ground there without it.
    CHECK(at(levels, 41, 22) >= at(levels, 41, 15) + 30);      // Against the ground behind it,
    CHECK(at(levels, 41, 15) < at(ground, 41, 15));            // which is darker than without it.
    CHECK(std::abs(at(levels, 10, 50) - at(ground, 10, 50)) <= 2); // Far from it nothing changes.
}

TEST_CASE("the interface's pixels are filled smoothly from the world round them, and left out of the levels") {
    constexpr int kWidth = 96;
    constexpr int kHeight = 64;
    std::vector<std::uint16_t> map = groundMap(kWidth, kHeight);
    const std::vector<std::uint16_t> plain = map;
    // A window over the world (map pixels 30-60 x 10-40), where the network guessed something near and flat.
    for (int y = 10; y < 40; ++y) {
        for (int x = 30; x < 60; ++x) {
            map[static_cast<std::size_t>(y * kWidth + x)] = toHalf(9.0f);
        }
    }
    const GreyArea window{60, 20, 120, 80}; // In the screen's pixels (twice the map's).
    DepthRules rules;
    std::vector<std::uint8_t> levels(map.size());
    std::vector<std::uint8_t> ground(map.size());
    std::array<std::uint32_t, Depthing::kPaletteSize> palette{};
    REQUIRE(rules.shade(inputOf(map, kWidth, kHeight, &window, 1), levels.data(), palette.data()));
    REQUIRE(rules.shade(inputOf(plain, kWidth, kHeight, &window, 1), ground.data(), palette.data()));
    // What the network guessed under the interface changes nothing, there or anywhere else.
    CHECK(levels == ground);
    // Inside: between the world above and below it, growing down as the ground does, nowhere as near as the window.
    for (int y = 12; y < 38; y += 5) {
        const int level = levels[static_cast<std::size_t>(y * kWidth + 45)];
        CHECK(level < 230);
        CHECK(level <= levels[static_cast<std::size_t>((y + 5) * kWidth + 45)] + 2);
    }
}

TEST_CASE("an interface piece at the screen's edge leaves no ridge along its own edge") {
    // The hotbar reaching the bottom (map pixels 30-60 x 40 to the bottom): the fill under it cannot go on with the
    // ground's slope to the screen's edge. The world just above it must still shade as the world beside it.
    constexpr int kWidth = 96;
    constexpr int kHeight = 64;
    const std::vector<std::uint16_t> map = groundMap(kWidth, kHeight);
    const GreyArea hotbar{60, 80, 120, 128}; // In the screen's pixels (twice the map's).
    DepthRules rules;
    std::vector<std::uint8_t> levels(map.size());
    std::array<std::uint32_t, Depthing::kPaletteSize> palette{};
    REQUIRE(rules.shade(inputOf(map, kWidth, kHeight, &hotbar, 1), levels.data(), palette.data()));
    const auto at = [&](int x, int y) { return static_cast<int>(levels[static_cast<std::size_t>(y * kWidth + x)]); };
    for (int y = 30; y < 38; ++y) { // Above it (the margin starts at 38).
        CHECK(std::abs(at(45, y) - at(10, y)) <= 1);
    }
    for (int y = 40; y < kHeight; y += 4) { // Beside it, up to the screen's edge.
        CHECK(std::abs(at(27, y) - at(10, y)) <= 1);
    }
}

TEST_CASE("a bar across the whole bottom and a window in its corner fill steadily, the world round them even") {
    // The game's: its menu bar from the left edge to the right (map rows 54 to the bottom with the margin: no row there
    // has any of the world), and the chat window above it in the corner (map pixels 0-22 x 28 to the bottom).
    constexpr int kWidth = 96;
    constexpr int kHeight = 64;
    const std::vector<std::uint16_t> map = groundMap(kWidth, kHeight);
    const std::array<GreyArea, 2> pieces{{{0, 112, 192, 128}, {0, 60, 40, 128}}}; // In the screen's pixels.
    DepthRules rules;
    std::vector<std::uint8_t> levels(map.size());
    std::array<std::uint32_t, Depthing::kPaletteSize> palette{};
    REQUIRE(rules.shade(inputOf(map, kWidth, kHeight, pieces.data(), 2), levels.data(), palette.data()));
    const auto at = [&](int x, int y) { return static_cast<int>(levels[static_cast<std::size_t>(y * kWidth + x)]); };
    for (int y = 30; y < 54; ++y) { // Beside the window as far from it, up to the bar.
        CHECK(std::abs(at(30, y) - at(70, y)) <= 1);
    }
    for (int y = 41; y < 53; ++y) { // Down to the bar the levels grow evenly: no ridge along it.
        CHECK(std::abs(at(60, y + 1) - 2 * at(60, y) + at(60, y - 1)) <= 2);
    }
    for (int y = 54; y < kHeight; ++y) { // Under the bar the ground goes on nearer, as under the window.
        CHECK(at(60, y) >= at(60, 53));
        CHECK(std::abs(at(10, y) - at(60, y)) <= 1);
    }
}

TEST_CASE("depth lines read back, and say which map was seen at each time") {
    std::string text;
    appendDepth(text, 1200, 33.5, 1198);
    appendDepth(text, 1203, 83.25, 1201);
    appendDepth(text, 1207, 150.0, 0);
    CHECK(text == "depth 1200 33.5 1198\ndepth 1203 83.25 1201\ndepth 1207 150 0\n");
    unsigned long long image = 0;
    double timeMs = 0.0;
    unsigned long long map = 0;
    REQUIRE(readDepth("depth 1203 83.25 1201", image, timeMs, map));
    CHECK(image == 1203);
    CHECK(timeMs == 83.25);
    CHECK(map == 1201);
    CHECK_FALSE(readDepth("depths 1203 83.25 1201", image, timeMs, map));
    CHECK_FALSE(readDepth("depth 1203", image, timeMs, map));

    const ClipText clip = readClipText("frame 1 1200 33.5\n" + text);
    REQUIRE(clip.depths.size() == 3);
    CHECK(recordedDepth(clip.depths, 10.0) == 0);   // Before the first.
    CHECK(recordedDepth(clip.depths, 33.5) == 1198);
    CHECK(recordedDepth(clip.depths, 100.0) == 1201);
    CHECK(recordedDepth(clip.depths, 150.0) == 0);  // Nothing drawn from then on.
}
