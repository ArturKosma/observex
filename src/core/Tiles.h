#pragma once

namespace observex {

// Where the tiles go in the view under the tabs Live, Stats and State (the user asked, 2026-11-12): each tab shows or
// hides its tile, and the shown ones share the view. One shown takes all of it. Live with others: Live on the left
// (`split` of the width, 3/4 at first), the others in a column on the right, Stats above State (`stack` of the column's
// height, half at first). Without Live, Stats above State across the whole width. `gap` is left between tiles, for the
// line that parts them.

struct TileRect {
    float left = 0.0f;
    float top = 0.0f;
    float right = 0.0f;
    float bottom = 0.0f;

    bool shown() const { return right > left && bottom > top; }
};

struct Tiles {
    TileRect live; // Not shown(): hidden.
    TileRect stats;
    TileRect state;
};

Tiles placeTiles(bool live, bool stats, bool state, TileRect area, float split, float stack, float gap);

// The Log tile (the tabs' last, the user asked, 2026-11-13: with all four shown, "a narrower column to the right of
// stats and state"): a column at the right of `area`, the whole height; the other tiles go into `rest` (placeTiles), the
// `left` share of the width (4/5 at first), then `gap`. With no others shown it takes all of `area` (`rest` not shown).
TileRect placeLog(TileRect area, bool others, float left, float gap, TileRect& rest);

} // namespace observex
