#include "heuristics/SnapRules.h"
#include "heuristics/Tracker.h"

#include <doctest/doctest.h>

#include <array>
#include <cmath>
#include <cstdint>
#include <span>
#include <utility>
#include <vector>

using namespace observex;

namespace {

constexpr int kWidth = 320;
constexpr int kHeight = 200;

// Grass (noise) seen with the camera turned `turn` pixels, and a striped thing `thingWidth` x `thingHeight` at `thingX`,
// `thingY` (none if `thingX` is negative).
GreyImage scene(int turn, int thingX, int thingY, int thingWidth = 30, int thingHeight = 28) {
    std::vector<std::uint8_t> pixels(static_cast<std::size_t>(kWidth * kHeight));
    for (int y = 0; y < kHeight; ++y) {
        for (int x = 0; x < kWidth; ++x) {
            const auto world = static_cast<std::uint32_t>(x - turn + 1000);
            const std::uint32_t hash = (world * 73856093u ^ static_cast<std::uint32_t>(y) * 19349663u) * 2654435761u;
            pixels[static_cast<std::size_t>(y * kWidth + x)] = static_cast<std::uint8_t>(70 + (hash >> 24) % 60);
        }
    }
    for (int y = 0; y < thingHeight && thingX >= 0; ++y) {
        for (int x = 0; x < thingWidth; ++x) {
            pixels[static_cast<std::size_t>((thingY + y) * kWidth + thingX + x)] =
                static_cast<std::uint8_t>((x / 4 + y / 5) % 2 == 0 ? 20 + 4 * y : 230 - 3 * x);
        }
    }
    GreyImage image;
    image.fromGrey(pixels.data(), kWidth, kHeight, kWidth);
    return image;
}

// The thing 7 as the tracker hands it out after finding it at `x`, `y` moving `speedX` pixels a millisecond.
Tracked shown(float x, float y, float speedX, float width = 30.0f, float height = 28.0f) {
    Tracked thing;
    thing.x = x + speedX * static_cast<float>(Tracker::kLeadMs);
    thing.y = y;
    thing.width = width;
    thing.height = height;
    thing.sure = 0.9f;
    thing.id = 7;
    thing.speedX = speedX;
    return thing;
}

constexpr auto kLead = static_cast<float>(SnapRules::kLeadMs);

void looked(SnapRules& rules, double lookMs, const GreyImage& image, std::span<const Tracked> shown) {
    const GreyView view = image.view();
    rules.looked(lookMs, &view, shown);
}

// `image` with every pixel outside `areas` (on each level, as engine/GreyMaker reads them back) turned over.
struct Parts {
    std::array<std::vector<std::uint8_t>, GreyView::kLevels> pixels;
    GreyView view;
};
Parts partsOf(const GreyView& image, std::span<const GreyArea> areas) {
    Parts parts;
    for (int l = 0; l < GreyView::kLevels; ++l) {
        const int width = image.width(l);
        const int height = image.height(l);
        std::vector<std::uint8_t>& pixels = parts.pixels[static_cast<std::size_t>(l)];
        pixels.resize(static_cast<std::size_t>(width * height));
        for (int y = 0; y < height; ++y) {
            for (int x = 0; x < width; ++x) {
                pixels[static_cast<std::size_t>(y * width + x)] = static_cast<std::uint8_t>(255 - image.row(l, y)[x]);
            }
        }
        for (const GreyArea& area : areas) {
            const GreyArea part = area.onLevel(l, width, height);
            for (int y = part.y0; y < part.y1; ++y) {
                for (int x = part.x0; x < part.x1; ++x) {
                    pixels[static_cast<std::size_t>(y * width + x)] = image.row(l, y)[x];
                }
            }
        }
        parts.view.levels[static_cast<std::size_t>(l)] = {pixels.data(), width, height, width};
    }
    return parts;
}

// A wider scene with several things 30 x 28 at `xs` (each striped its own way; none where x is negative), `y` down.
GreyImage crowd(int turn, std::span<const int> xs, int y) {
    constexpr int width = 640;
    std::vector<std::uint8_t> pixels(static_cast<std::size_t>(width * kHeight));
    for (int row = 0; row < kHeight; ++row) {
        for (int x = 0; x < width; ++x) {
            const auto world = static_cast<std::uint32_t>(x - turn + 1000);
            const std::uint32_t hash = (world * 73856093u ^ static_cast<std::uint32_t>(row) * 19349663u) * 2654435761u;
            pixels[static_cast<std::size_t>(row * width + x)] = static_cast<std::uint8_t>(70 + (hash >> 24) % 60);
        }
    }
    for (std::size_t i = 0; i < xs.size(); ++i) {
        const int period = 3 + 2 * static_cast<int>(i);
        for (int row = 0; row < 28 && xs[i] >= 0; ++row) {
            for (int x = 0; x < 30; ++x) {
                pixels[static_cast<std::size_t>((y + row) * width + xs[i] + x)] =
                    static_cast<std::uint8_t>((x / period + row / (7 - static_cast<int>(i))) % 2 == 0 ? 20 + 4 * row
                                                                                                   : 230 - 3 * x);
            }
        }
    }
    GreyImage image;
    image.fromGrey(pixels.data(), width, kHeight, width);
    return image;
}

} // namespace

TEST_CASE("a thing the look found is snapped onto a newer image and handed out ahead along its speed") {
    SnapRules rules;
    const Tracked look[] = {shown(100.0f, 80.0f, 2.0f)};
    rules.looked(0.0, nullptr, look); // The look's image is gone: nothing to snap by yet.
    Tracked out[4];
    REQUIRE(rules.snap(6.0, scene(13, 113, 80).view(), out, 4) == 1);
    CHECK(out[0].held);
    CHECK(out[0].x == doctest::Approx(100.0f + 2.0f * (6.0f + kLead)));

    looked(rules, 0.0, scene(0, 100, 80), look); // Its image at hand: learned.
    REQUIRE(rules.snap(6.0, scene(13, 113, 80).view(), out, 4) == 1);
    CHECK_FALSE(out[0].held);
    CHECK(out[0].id == 7);
    CHECK(out[0].sure == doctest::Approx(0.9f));
    CHECK(std::abs(out[0].x - (113.0f + 2.0f * kLead)) < 0.5f); // The tracker's speed until snapped twice.
    CHECK(std::abs(out[0].y - 80.0f) < 0.5f);
}

TEST_CASE("its speed comes from where it was snapped, and a thing not found is held where that takes it") {
    SnapRules rules;
    const Tracked look[] = {shown(100.0f, 80.0f, 2.0f)};
    looked(rules, 0.0, scene(0, 100, 80), look);
    Tracked out[4];
    REQUIRE(rules.snap(6.0, scene(13, 113, 80).view(), out, 4) == 1);
    REQUIRE(rules.snap(12.0, scene(31, 131, 80).view(), out, 4) == 1); // A turn speeding up: 3 pixels a ms.
    CHECK_FALSE(out[0].held);
    CHECK(out[0].speedX == doctest::Approx(3.0f).epsilon(0.05));
    CHECK(std::abs(out[0].x - (131.0f + 3.0f * kLead)) < 1.0f);

    // Gone behind something: where its speed takes it.
    REQUIRE(rules.snap(18.0, scene(49, -1, 0).view(), out, 4) == 1);
    CHECK(out[0].held);
    CHECK(std::abs(out[0].x - (131.0f + 3.0f * (6.0f + kLead))) < 1.0f);
}

TEST_CASE("a late look keeps the newer snapped place, and the things it no longer shows go") {
    SnapRules rules;
    const Tracked first[] = {shown(100.0f, 80.0f, 2.0f)};
    looked(rules, 0.0, scene(0, 100, 80), first);
    Tracked out[4];
    REQUIRE(rules.snap(12.0, scene(24, 124, 80).view(), out, 4) == 1);
    // The look at the image of 6 ms comes after the one of 12 ms was snapped: the box does not go back.
    const Tracked second[] = {shown(112.0f, 80.0f, 2.0f)};
    looked(rules, 6.0, scene(12, 112, 80), second);
    REQUIRE(rules.snap(12.0, scene(24, 124, 80).view(), out, 4) == 1);
    CHECK(std::abs(out[0].x - (124.0f + 2.0f * kLead)) < 0.5f);

    looked(rules, 12.0, scene(24, 124, 80), {});
    CHECK(rules.snap(18.0, scene(36, 136, 80).view(), out, 4) == 0);
}

TEST_CASE("a thing the look holds stays where it was snapped, not where the tracker's speed takes it") {
    SnapRules rules;
    const Tracked first[] = {shown(100.0f, 80.0f, 0.0f)};
    looked(rules, 0.0, scene(0, 100, 80), first);
    Tracked out[4];
    REQUIRE(rules.snap(6.0, scene(0, 100, 80).view(), out, 4) == 1);
    // The tracker lost it (in a jump) and holds it where the speed it had takes it, far off: it is still here.
    Tracked held = shown(300.0f, 80.0f, 5.0f);
    held.held = true;
    const Tracked second[] = {held};
    looked(rules, 10.0, scene(0, 100, 80), second);
    REQUIRE(rules.snap(16.0, scene(0, 100, 80).view(), out, 4) == 1);
    CHECK_FALSE(out[0].held);
    CHECK(std::abs(out[0].x - 100.0f) < 0.5f);
}

TEST_CASE("a thing the look holds is not handed out twice when it is snapped onto another one's box") {
    SnapRules rules;
    const Tracked a = shown(100.0f, 80.0f, 0.0f);
    Tracked b = a;
    b.id = 8;
    b.sure = 0.8f;
    const Tracked both[] = {a, b};
    looked(rules, 0.0, scene(0, 100, 80), both);
    Tracked out[4];
    // Both found by the look: the tracker tells them apart, so both stay.
    CHECK(rules.snap(6.0, scene(0, 100, 80).view(), out, 4) == 2);
    b.held = true; // Lost by the tracker, and found again as 7.
    const Tracked later[] = {a, b};
    looked(rules, 10.0, scene(0, 100, 80), later);
    REQUIRE(rules.snap(16.0, scene(0, 100, 80).view(), out, 4) == 1);
    CHECK(out[0].id == 7);

    b.sure = 0.95f; // Also when the held one is surer, so handed out first.
    const Tracked surer[] = {b, a};
    looked(rules, 20.0, scene(0, 100, 80), surer);
    REQUIRE(rules.snap(26.0, scene(0, 100, 80).view(), out, 4) == 1);
    CHECK(out[0].id == 7);
    CHECK_FALSE(out[0].held);
}

TEST_CASE("a thing found neither by the looks nor on the images for kGoneMs is not handed out") {
    SnapRules rules;
    const Tracked look[] = {shown(100.0f, 80.0f, 0.0f)};
    looked(rules, 0.0, scene(0, 100, 80), look);
    Tracked out[4];
    const GreyImage empty = scene(0, -1, 0);
    REQUIRE(rules.snap(SnapRules::kGoneMs - 1.0, empty.view(), out, 4) == 1);
    CHECK(out[0].held);
    CHECK(rules.snap(SnapRules::kGoneMs + 1.0, empty.view(), out, 4) == 0);
    looked(rules, SnapRules::kGoneMs + 2.0, scene(0, 100, 80), look); // Found by a look again.
    REQUIRE(rules.snap(SnapRules::kGoneMs + 3.0, scene(0, 100, 80).view(), out, 4) == 1);
    CHECK_FALSE(out[0].held);
}

TEST_CASE("after a long gap a thing is looked for at most Tracker::kMaxAheadMs along its speed") {
    SnapRules rules;
    const Tracked look[] = {shown(100.0f, 80.0f, 5.0f)};
    looked(rules, 0.0, scene(0, 100, 80), look);
    GreyArea areas[4];
    REQUIRE(rules.snapAreas(50.0, false, 0.0, {}, areas, 4) == 1);
    // All 250 pixels of its speed would read 600 across; 20 ms of it about 300.
    const float went = 5.0f * static_cast<float>(Tracker::kMaxAheadMs);
    CHECK(areas[0].x1 - areas[0].x0 <= static_cast<int>(2.0f * (SnapRules::kReach + went + 48.0f) + 32.0f));
    // The game stood still meanwhile: it is 16 pixels on, and found there.
    Tracked out[4];
    REQUIRE(rules.snap(50.0, scene(16, 116, 80).view(), out, 4) == 1);
    CHECK_FALSE(out[0].held);
    CHECK(std::abs(out[0].x - (116.0f + 5.0f * kLead)) < 0.5f); // Led by the tracker's speed: no line of its own yet.
}

TEST_CASE("a thing not found is held on along its speed from image to image, at most kMaxAheadMs a step") {
    SnapRules rules;
    const Tracked look[] = {shown(100.0f, 80.0f, 2.0f)};
    looked(rules, 0.0, scene(0, 100, 80), look);
    Tracked out[4];
    const GreyImage gone = scene(0, -1, 0); // Behind something, while the camera turns on.
    // Images 10 ms apart: on 2 pixels a millisecond each time, past kMaxAheadMs in all (until the evening of
    // 2026-11-11 it stood still after 20 ms).
    for (int i = 1; i <= 3; ++i) {
        REQUIRE(rules.snap(10.0 * i, gone.view(), out, 4) == 1);
        CHECK(out[0].held);
        CHECK(std::abs(out[0].x - (100.0f + 2.0f * (10.0f * static_cast<float>(i) + kLead))) < 0.5f);
    }
    // No new image for 28 ms (the game stood still): only kMaxAheadMs of it.
    REQUIRE(rules.snap(58.0, gone.view(), out, 4) == 1);
    CHECK(std::abs(out[0].x - (100.0f + 2.0f * (30.0f + static_cast<float>(Tracker::kMaxAheadMs) + kLead))) < 0.5f);
}

TEST_CASE("a thing the snapping lost and a look found again is looked for at the tracker's speed") {
    SnapRules rules;
    const Tracked still[] = {shown(100.0f, 80.0f, 0.0f)};
    looked(rules, 0.0, scene(0, 100, 80), still);
    Tracked out[4];
    REQUIRE(rules.snap(8.0, scene(0, 100, 80).view(), out, 4) == 1);
    REQUIRE(rules.snap(16.0, scene(0, 100, 80).view(), out, 4) == 1); // Snapped twice: its own line, standing.
    CHECK(out[0].speedX == doctest::Approx(0.0f));
    // A camera turn starts, 8 pixels a millisecond: it is too far from where it was looked for.
    REQUIRE(rules.snap(24.0, scene(64, 164, 80).view(), out, 4) == 1);
    CHECK(out[0].held);
    // The look at that image found it, and the tracker has its speed: on the next image it is found again (until the
    // evening of 2026-11-11 it was looked for at its own speed, standing, and held there).
    const Tracked turning[] = {shown(164.0f, 80.0f, 8.0f)};
    looked(rules, 24.0, scene(64, 164, 80), turning);
    REQUIRE(rules.snap(32.0, scene(128, 228, 80).view(), out, 4) == 1);
    CHECK_FALSE(out[0].held);
    CHECK(std::abs(out[0].x - (228.0f + 8.0f * kLead)) < 0.5f);
}

TEST_CASE("after the camera jumped, the things lost at once are looked for wide and found where they say alike") {
    // Four things standing; the game stands still, then the scene is 150 pixels on, and 160 by the next image, where
    // the fourth is hidden. One on whole images, one on images where all but the parts it said turned over.
    SnapRules whole;
    SnapRules parts;
    std::vector<Tracked> look;
    const std::array<int, 4> at = {20, 120, 220, 320};
    for (std::size_t i = 0; i < at.size(); ++i) {
        Tracked thing = shown(static_cast<float>(at[i]), 80.0f, 0.0f);
        thing.id = 7 + static_cast<std::uint32_t>(i);
        look.push_back(thing);
    }
    const GreyImage first = crowd(0, at, 80);
    looked(whole, 0.0, first, look);
    looked(parts, 0.0, first, look);
    const auto both = [&](double timeMs, const GreyImage& image, std::array<Tracked, 4>& out) {
        GreyArea areas[4];
        const int count = parts.snapAreas(timeMs, false, 0.0, {}, areas, 4);
        const Parts read = partsOf(image.view(), {areas, static_cast<std::size_t>(count)});
        std::array<Tracked, 4> fromParts;
        const int written = whole.snap(timeMs, image.view(), out.data(), 4);
        REQUIRE(parts.snap(timeMs, read.view, fromParts.data(), 4) == written);
        for (int i = 0; i < written; ++i) {
            CHECK(fromParts[static_cast<std::size_t>(i)].x == out[static_cast<std::size_t>(i)].x);
            CHECK(fromParts[static_cast<std::size_t>(i)].held == out[static_cast<std::size_t>(i)].held);
        }
        return written;
    };
    std::array<Tracked, 4> out;
    REQUIRE(both(8.0, first, out) == 4);

    const std::array<int, 4> jumped = {170, 270, 370, 470};
    REQUIRE(both(16.0, crowd(150, jumped, 80), out) == 4);
    for (const Tracked& thing : out) {
        CHECK(thing.held); // Past where any was looked for.
    }

    const std::array<int, 4> next = {180, 280, 380, -1};
    REQUIRE(both(24.0, crowd(160, next, 80), out) == 4);
    for (const Tracked& thing : out) {
        CAPTURE(thing.id);
        if (thing.id == 10) {
            CHECK(thing.held); // Not found: held where the jump takes it.
            CHECK(std::abs(thing.x - 480.0f) < 1.0f);
        } else {
            CHECK_FALSE(thing.held);
            CHECK(std::abs(thing.x - static_cast<float>(next[thing.id - 7])) < 0.5f); // Its line starts again.
        }
    }
}

TEST_CASE("snapping reads only the parts of the images it says it does") {
    for (const auto& [width, height] : {std::pair{30, 28}, std::pair{110, 96}}) {
        CAPTURE(width);
        // One on whole images, one on images where all but the parts it said turned over.
        SnapRules whole;
        SnapRules parts;
        const auto w = static_cast<float>(width);
        const auto h = static_cast<float>(height);
        const Tracked look[] = {shown(60.0f, 50.0f, 2.0f, w, h)};
        const GreyImage first = scene(0, 60, 50, width, height);
        const GreyImage second = scene(13, 73, 50, width, height);
        looked(whole, 0.0, first, look);
        // As live: the look's parts and the next image's are read back before the look is handed over.
        GreyArea areas[4];
        int count = parts.learnAreas(look, areas, 4);
        REQUIRE(count == 1);
        const Parts firstParts = partsOf(first.view(), {areas, static_cast<std::size_t>(count)});
        count = parts.snapAreas(6.0, true, 0.0, look, areas, 4);
        REQUIRE(count == 1);
        const Parts secondParts = partsOf(second.view(), {areas, static_cast<std::size_t>(count)});
        parts.looked(0.0, &firstParts.view, look);
        Tracked a[4];
        Tracked b[4];
        REQUIRE(whole.snap(6.0, second.view(), a, 4) == 1);
        REQUIRE(parts.snap(6.0, secondParts.view, b, 4) == 1);
        CHECK_FALSE(a[0].held);
        CHECK(b[0].held == a[0].held);
        CHECK(b[0].x == a[0].x);
        CHECK(b[0].y == a[0].y);

        // The next image, with no look: faster, and a little down.
        const GreyImage third = scene(31, 91, 53, width, height);
        count = parts.snapAreas(12.0, false, 0.0, {}, areas, 4);
        REQUIRE(count == 1);
        const Parts thirdParts = partsOf(third.view(), {areas, static_cast<std::size_t>(count)});
        REQUIRE(whole.snap(12.0, third.view(), a, 4) == 1);
        REQUIRE(parts.snap(12.0, thirdParts.view, b, 4) == 1);
        CHECK_FALSE(a[0].held);
        CHECK(b[0].held == a[0].held);
        CHECK(b[0].x == a[0].x);
        CHECK(b[0].y == a[0].y);
    }
}

TEST_CASE("a grey area halves outwards, a pixel wider, cut to the level") {
    const GreyArea area{-5, 3, 17, 40};
    const GreyArea level0 = area.onLevel(0, 100, 30);
    CHECK(level0.x0 == 0);
    CHECK(level0.y0 == 2);
    CHECK(level0.x1 == 18);
    CHECK(level0.y1 == 30);
    const GreyArea level2 = area.onLevel(2, 25, 7);
    CHECK(level2.x0 == 0); // -5 / 4 rounded down is -2, less a pixel.
    CHECK(level2.y0 == 0);
    CHECK(level2.x1 == 6); // 17 / 4 rounded up is 5, and a pixel.
    CHECK(level2.y1 == 7);
    const GreyArea off = GreyArea{200, 0, 260, 10}.onLevel(1, 50, 50);
    CHECK(off.x0 >= off.x1);
}

TEST_CASE("a replay takes the searches recorded live, so on other pixels it hands out what live did") {
    // Live: the thing found on a look at image 1, then snapped onto images 2 to 5 while the camera turns 3 pixels a
    // millisecond.
    SnapRules live;
    const std::array look{shown(10.0f, 100.0f, 3.0f)};
    const GreyImage first = scene(0, 10, 100);
    const GreyView firstView = first.view(1);
    live.looked(0.0, &firstView, look);
    std::vector<std::vector<SnapFind>> recorded;
    std::vector<std::vector<Tracked>> handed;
    for (int image = 2; image <= 5; ++image) {
        const int turn = 18 * (image - 1); // 6 ms apart.
        const GreyImage now = scene(turn, 10 + turn, 100);
        std::array<Tracked, 4> out;
        const int count = live.snap(6.0 * (image - 1), now.view(static_cast<std::uint64_t>(image)), out.data(), 4);
        handed.emplace_back(out.begin(), out.begin() + count);
        recorded.emplace_back(live.finds().begin(), live.finds().end());
    }
    REQUIRE(recorded[0].size() == 1);
    CHECK(recorded[0][0].finder == Snapper::kFinder);
    CHECK(recorded[0][0].id == 7);
    CHECK(recorded[0][0].learnedImage == 1);
    CHECK(recorded[0][0].learnedX == doctest::Approx(10.0f));
    CHECK(recorded[0][0].found);
    CHECK(recorded[0][0].match > Snapper::kMinMatch);
    for (const std::vector<Tracked>& things : handed) {
        REQUIRE(things.size() == 1);
        CHECK_FALSE(things[0].held); // Live found it on every image.
    }
    CHECK(handed[3][0].x == doctest::Approx(10.0f + 72.0f + 3.0f * kLead).epsilon(0.01));

    // On grass alone (as the video's pixels may differ from the screen's), with live's searches: what live handed out.
    const GreyImage grass = scene(0, -1, 0);
    const auto replayed = [&](std::uint32_t finder) {
        SnapRules replay;
        const GreyView lookView = grass.view(1);
        replay.looked(0.0, &lookView, look);
        std::vector<Tracked> out;
        for (int image = 2; image <= 5; ++image) {
            std::vector<SnapFind> searches = recorded[static_cast<std::size_t>(image - 2)];
            for (SnapFind& search : searches) {
                search.finder = finder;
            }
            replay.replay(searches);
            std::array<Tracked, 4> things;
            const int count = replay.snap(6.0 * (image - 1), grass.view(static_cast<std::uint64_t>(image)),
                                          things.data(), 4);
            REQUIRE(count == 1);
            out.push_back(things[0]);
        }
        return out;
    };
    const std::vector<Tracked> same = replayed(Snapper::kFinder);
    for (std::size_t i = 0; i < same.size(); ++i) {
        CHECK(same[i].x == handed[i][0].x);
        CHECK(same[i].y == handed[i][0].y);
        CHECK(same[i].held == handed[i][0].held);
        CHECK(same[i].speedX == handed[i][0].speedX);
    }
    // Recorded by another way of finding: not taken; it searches the grass, which holds no such thing.
    const std::vector<Tracked> other = replayed(Snapper::kFinder + 1);
    CHECK(std::abs(other.back().x - handed.back()[0].x) > 5.0f);
}
