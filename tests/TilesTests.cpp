#include "core/Tiles.h"

#include <doctest/doctest.h>

#include <array>

using namespace observex;

namespace {

constexpr TileRect kArea = {0.0f, 100.0f, 1000.0f, 900.0f};

bool same(const TileRect& a, const TileRect& b) {
    return a.left == b.left && a.top == b.top && a.right == b.right && a.bottom == b.bottom;
}

} // namespace

TEST_CASE("one tile shown takes the whole view") {
    for (const auto [live, stats, state] : {std::array{true, false, false}, std::array{false, true, false},
                                            std::array{false, false, true}}) {
        const Tiles tiles = placeTiles(live, stats, state, kArea, 0.75f, 0.5f, 6.0f);
        const TileRect& shown = live ? tiles.live : stats ? tiles.stats : tiles.state;
        CHECK(same(shown, kArea));
        CHECK(tiles.live.shown() + tiles.stats.shown() + tiles.state.shown() == 1);
    }
}

TEST_CASE("all three: Live on the left, Stats above State on the right") {
    const Tiles tiles = placeTiles(true, true, true, kArea, 0.75f, 0.5f, 6.0f);
    CHECK(same(tiles.live, {0.0f, 100.0f, 750.0f, 900.0f}));
    CHECK(same(tiles.stats, {756.0f, 100.0f, 1000.0f, 500.0f}));
    CHECK(same(tiles.state, {756.0f, 506.0f, 1000.0f, 900.0f}));
}

TEST_CASE("Live with one other: that one takes the right column whole") {
    const Tiles withState = placeTiles(true, false, true, kArea, 0.75f, 0.5f, 6.0f);
    CHECK(same(withState.live, {0.0f, 100.0f, 750.0f, 900.0f}));
    CHECK(same(withState.state, {756.0f, 100.0f, 1000.0f, 900.0f}));
    CHECK_FALSE(withState.stats.shown());
    const Tiles withStats = placeTiles(true, true, false, kArea, 0.6f, 0.5f, 6.0f);
    CHECK(same(withStats.stats, {606.0f, 100.0f, 1000.0f, 900.0f}));
}

TEST_CASE("without Live: Stats above State across the whole width") {
    const Tiles tiles = placeTiles(false, true, true, kArea, 0.75f, 0.25f, 6.0f);
    CHECK_FALSE(tiles.live.shown());
    CHECK(same(tiles.stats, {0.0f, 100.0f, 1000.0f, 300.0f}));
    CHECK(same(tiles.state, {0.0f, 306.0f, 1000.0f, 900.0f}));
}

TEST_CASE("all four: Live, then Stats above State, then Log, a narrower column at the right") {
    TileRect rest;
    const TileRect log = placeLog(kArea, true, 0.8f, 6.0f, rest);
    CHECK(same(log, {806.0f, 100.0f, 1000.0f, 900.0f}));
    CHECK(same(rest, {0.0f, 100.0f, 800.0f, 900.0f}));
    const Tiles tiles = placeTiles(true, true, true, rest, 0.6f, 0.5f, 6.0f);
    CHECK(same(tiles.live, {0.0f, 100.0f, 480.0f, 900.0f}));
    CHECK(same(tiles.stats, {486.0f, 100.0f, 800.0f, 500.0f}));
    CHECK(same(tiles.state, {486.0f, 506.0f, 800.0f, 900.0f}));
    CHECK(log.right - log.left < tiles.stats.right - tiles.stats.left);
}

TEST_CASE("Log alone takes the whole view") {
    TileRect rest = kArea;
    CHECK(same(placeLog(kArea, false, 0.8f, 6.0f, rest), kArea));
    CHECK_FALSE(rest.shown());
}

TEST_CASE("none shown: nothing placed") {
    const Tiles tiles = placeTiles(false, false, false, kArea, 0.75f, 0.5f, 6.0f);
    CHECK_FALSE((tiles.live.shown() || tiles.stats.shown() || tiles.state.shown()));
}
