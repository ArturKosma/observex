#include "core/Tiles.h"

#include <cmath>

namespace observex {

Tiles placeTiles(bool live, bool stats, bool state, TileRect area, float split, float stack, float gap) {
    Tiles tiles;
    TileRect column = area; // Where Stats and State go.
    if (live) {
        if (!stats && !state) {
            tiles.live = area;
            return tiles;
        }
        const float x = std::round(area.left + (area.right - area.left) * split);
        tiles.live = {area.left, area.top, x, area.bottom};
        column.left = x + gap;
    }
    if (stats && state) {
        const float y = std::round(column.top + (column.bottom - column.top) * stack);
        tiles.stats = {column.left, column.top, column.right, y};
        tiles.state = {column.left, y + gap, column.right, column.bottom};
    } else if (stats) {
        tiles.stats = column;
    } else if (state) {
        tiles.state = column;
    }
    return tiles;
}

TileRect placeLog(TileRect area, bool others, float left, float gap, TileRect& rest) {
    if (!others) {
        rest = {};
        return area;
    }
    const float x = std::round(area.left + (area.right - area.left) * left);
    rest = {area.left, area.top, x, area.bottom};
    return {x + gap, area.top, area.right, area.bottom};
}

} // namespace observex
