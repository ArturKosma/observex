#include "core/LootWatch.h"

#include <doctest/doctest.h>

#include <string>
#include <vector>

using namespace observex;

namespace {

constexpr int kWidth = 2560;
constexpr int kHeight = 1440;

// The watch, stepped as the app does on every UI frame.
struct Watch {
    LootWatch watch;
    double ms = 0.0;
    std::vector<WatchedThing> things;
    std::uint64_t image = 0; // The newest screen image looked at.
    std::uint64_t looked = 0;
    std::vector<DropSeen> drops;
    std::uint64_t swept = 0;
    std::vector<DropSeen> sweepDrops;
    std::string log;

    LootWatch::Output step(std::uint32_t dead = 0, const char* name = "Imp Elder") {
        LootWatch::Input in;
        in.ms = ms;
        in.width = kWidth;
        in.height = kHeight;
        in.dead = dead;
        in.name = name;
        in.things = things;
        in.close = {looked, drops};
        in.sweep = {swept, sweepDrops};
        LootWatch::Output out = watch.step(in);
        log += out.log.empty() ? "" : out.log + "\n";
        return out;
    }
    // A new close look of the drop finder's with `found` in it.
    void look(std::vector<DropSeen> found) {
        looked = ++image;
        drops = std::move(found);
    }
    // A new sweep look.
    void sweep(std::vector<DropSeen> found) {
        swept = ++image;
        sweepDrops = std::move(found);
    }
};

} // namespace

TEST_CASE("the square round a corpse is four times its longer side, at its middle near the bottom") {
    // A corpse lying down, 100 x 40, its middle at 1000, 620.
    const LootSquare lying = lootSquare({950, 600, 1050, 640}, kWidth, kHeight);
    CHECK(lying.side == 400);
    CHECK(lying.x == 800);
    CHECK(lying.y == 420); // Its middle 20 above the box's bottom: 620.
    // One still standing, 50 x 120: the middle 25 above its feet.
    const LootSquare standing = lootSquare({975, 500, 1025, 620}, kWidth, kHeight);
    CHECK(standing.side == 480);
    CHECK(standing.y + standing.side / 2 == 595);
}

TEST_CASE("the square is at least kLootMinSide, at most the screen's height, and moved inside the screen") {
    CHECK(lootSquare({1000, 700, 1010, 705}, kWidth, kHeight).side == kLootMinSide);
    const LootSquare huge = lootSquare({1000, 300, 1500, 900}, kWidth, kHeight);
    CHECK(huge.side == kHeight);
    CHECK(huge.y == 0);
    const LootSquare corner = lootSquare({2500, 1380, 2560, 1430}, kWidth, kHeight);
    CHECK(corner.x + corner.side == kWidth);
    CHECK(corner.y + corner.side == kHeight);
    CHECK(lootSquare({0, 0, 50, 50}, 0, 0).empty());
}

TEST_CASE("tiles cover the whole image at two sizes, each overlapping the next by a quarter at least") {
    const std::vector<LootSquare> tiles = lootTiles(kWidth, kHeight);
    REQUIRE(tiles.size() == 7 * 4 + 3);
    for (const LootSquare& tile : tiles) {
        CHECK(tile.x >= 0);
        CHECK(tile.y >= 0);
        CHECK(tile.x + tile.side <= kWidth);
        CHECK(tile.y + tile.side <= kHeight);
    }
    CHECK(tiles.front() == LootSquare{0, 0, kTileSide});
    CHECK(tiles[6].x + kTileSide == kWidth);
    CHECK(tiles[1].x <= kTileSide * 3 / 4);
    CHECK(tiles.back() == LootSquare{kWidth - kHeight, 0, kHeight});
    // A square image no larger than a tile is one tile; a small wide one two, its height each.
    CHECK(lootTiles(300, 300).size() == 1);
    CHECK(lootTiles(400, 300).size() == 2);
}

TEST_CASE("keepApart keeps the surest of boxes on one place and drops one cut at a square's edge") {
    std::vector<Detection> found(4);
    found[0] = {100, 100, 40, 20, 0.6f};
    found[1] = {102, 101, 40, 20, 0.9f}; // The same drop from another square, surer.
    found[2] = {100, 100, 18, 20, 0.7f}; // Its left part, cut at an edge: inside it.
    found[3] = {300, 100, 40, 20, 0.5f}; // Another drop.
    const std::vector<int> kept = keepApart(found);
    REQUIRE(kept.size() == 2);
    CHECK(kept[0] == 1);
    CHECK(kept[1] == 3);
}

TEST_CASE("a dead target starts the look round its box, which follows it while it is on the screen") {
    Watch w;
    w.things = {{7, {950, 600, 1050, 640}}};
    CHECK(w.step().square.empty()); // Nothing died yet.
    LootWatch::Output out = w.step(7);
    CHECK(out.square == lootSquare({950, 600, 1050, 640}, kWidth, kHeight));
    CHECK(w.log == "Looking for drops round Imp Elder (box 7)\n");
    w.things[0].box = {850, 600, 950, 640}; // The camera turned.
    w.ms += 100.0;
    out = w.step();
    CHECK(out.square.x == 700);
}

TEST_CASE("drops found are counted and logged once, and the square follows them once the corpse is gone") {
    Watch w;
    w.things = {{7, {950, 600, 1050, 640}}};
    w.step(7);
    w.look({{{1100, 650, 1130, 665}, 0.9f}, {{900, 580, 940, 620}, 0.8f}});
    w.ms += 50.0;
    LootWatch::Output out = w.step();
    CHECK(out.drops == 2);
    w.look({{{1100, 650, 1130, 665}, 0.9f}}); // One missed on a look: nothing said.
    w.ms += 50.0;
    w.step();
    CHECK(w.log == "Looking for drops round Imp Elder (box 7)\nDrops round Imp Elder: 2\n");

    // The corpse vanished; the camera moved the drops 200 left.
    w.things.clear();
    w.look({{{900, 650, 930, 665}, 0.9f}, {{700, 580, 740, 620}, 0.8f}});
    w.ms += 50.0;
    out = w.step();
    CHECK(out.square == lootSquareAt(815.0f, 622.5f, 100.0f, kWidth, kHeight));

    // Picked up: nothing for kLostMs, and it ends.
    w.look({});
    w.ms += 50.0;
    CHECK_FALSE(w.step().square.empty());
    w.ms += LootWatch::kLostMs + 1.0;
    w.look({});
    CHECK(w.step().square.empty());
    CHECK(w.log.ends_with("Drops round Imp Elder: none left\n"));
}

TEST_CASE("no drops: it ends once the corpse is gone for kLostMs") {
    Watch w;
    w.things = {{7, {950, 600, 1050, 640}}};
    w.looked = w.image = 41; // An older spot's look, before this death: not counted.
    w.drops = {{{1100, 650, 1130, 665}, 0.9f}};
    w.step(7);
    w.ms += 50.0;
    CHECK(w.step().drops == 0);
    w.look({});
    w.ms += 3000.0;
    CHECK_FALSE(w.step().square.empty()); // The corpse still lies there.
    w.things.clear();
    w.ms += LootWatch::kLostMs + 1.0;
    CHECK(w.step().square.empty());
    CHECK(w.log.ends_with("No drops round Imp Elder\n"));
}

TEST_CASE("the same target's box marked again is the same kill; another target's death is a new one") {
    Watch w;
    w.things = {{7, {950, 600, 1050, 640}}};
    w.step(7);
    w.look({{{1100, 650, 1130, 665}, 0.9f}});
    w.ms += 50.0;
    w.step();
    w.things = {{9, {960, 600, 1060, 640}}}; // The tracker lost it and found it again.
    w.ms += 50.0;
    LootWatch::Output out = w.step(9);
    CHECK(out.square == lootSquare({960, 600, 1060, 640}, kWidth, kHeight));
    CHECK(w.log.find("box 9") == std::string::npos);
    w.ms += 50.0;
    w.step(12, "Goblin");
    CHECK(w.log.ends_with("Looking for drops round Goblin (box 12)\n"));
}

TEST_CASE("it stops looking kMaxMs after the death") {
    Watch w;
    w.things = {{7, {950, 600, 1050, 640}}};
    w.step(7);
    w.ms += LootWatch::kMaxMs + 1.0;
    CHECK(w.step().square.empty());
    CHECK(w.log.ends_with("Stopped looking for drops round Imp Elder after 60 s\n"));
}

TEST_CASE("the square round drops found with no corpse makes the largest a tenth of it and holds their spread") {
    CHECK(lootSquareRound(std::vector<DropSeen>{{{1000, 700, 1040, 720}, 0.9f}}, kWidth, kHeight) ==
          lootSquareAt(1020.0f, 710.0f, kDropsSpot * 40.0f, kWidth, kHeight));
    CHECK(lootSquareRound(std::vector<DropSeen>{{{1000, 700, 1010, 705}, 0.9f}}, kWidth, kHeight).side == kLootMinSide);
    // Two far apart: their spread fills the middle 80%.
    const LootSquare wide = lootSquareRound(
        std::vector<DropSeen>{{{1000, 700, 1020, 710}, 0.9f}, {{1620, 700, 1640, 710}, 0.8f}}, kWidth, kHeight);
    CHECK(wide.side == 800);
    CHECK(wide.x == 920);
    CHECK(lootSquareRound({}, kWidth, kHeight).empty());
}

TEST_CASE("drops the sweep found start a close look round them, said once a close look found them too") {
    Watch w;
    CHECK(w.step().square.empty()); // Only the sweep.
    w.sweep({{{1000, 700, 1040, 720}, 0.8f}});
    LootWatch::Output out = w.step();
    CHECK(out.square == lootSquareRound(w.sweepDrops, kWidth, kHeight));
    CHECK_FALSE(out.kill);
    CHECK(w.log.empty()); // One look alone may be wrong.
    w.look({{{990, 705, 1030, 725}, 0.9f}}); // The camera moved 10 left.
    w.ms += 50.0;
    out = w.step();
    CHECK(out.drops == 1);
    CHECK(out.square == lootSquareRound(w.drops, kWidth, kHeight));
    CHECK(w.log == "Drops seen: 1\n");
    // Another drop elsewhere on the sweep changes nothing while this spot lasts.
    w.sweep({{{200, 300, 240, 320}, 0.9f}});
    w.ms += 50.0;
    CHECK(w.step().square == lootSquareRound(w.drops, kWidth, kHeight));
    // Picked up.
    w.look({});
    w.ms += LootWatch::kLostMs + 1.0;
    CHECK(w.step().square.empty());
    CHECK(w.log == "Drops seen: 1\nDrops gone\n");
}

TEST_CASE("a spot the sweep started that no close look confirms ends without a word") {
    Watch w;
    w.sweep({{{1000, 700, 1040, 720}, 0.6f}});
    w.step();
    w.look({});
    w.ms += 50.0;
    CHECK_FALSE(w.step().square.empty());
    w.ms += LootWatch::kLostMs + 1.0;
    w.look({});
    CHECK(w.step().square.empty());
    CHECK(w.log.empty());
    // The same sweep look is not counted again; a new one starts a new spot.
    w.ms += 50.0;
    CHECK(w.step().square.empty());
    w.sweep({{{1000, 700, 1040, 720}, 0.6f}});
    CHECK_FALSE(w.step().square.empty());
}

TEST_CASE("a kill takes over from drops the sweep found") {
    Watch w;
    w.sweep({{{200, 300, 240, 320}, 0.9f}});
    w.step();
    w.look({{{200, 300, 240, 320}, 0.9f}});
    w.ms += 50.0;
    w.step();
    w.things = {{7, {950, 600, 1050, 640}}};
    w.ms += 50.0;
    const LootWatch::Output out = w.step(7);
    CHECK(out.square == lootSquare({950, 600, 1050, 640}, kWidth, kHeight));
    CHECK(out.drops == 0); // The close look before the death is the other spot's.
    CHECK(out.kill);
    CHECK(w.log == "Drops seen: 1\nLooking for drops round Imp Elder (box 7)\n");
}
