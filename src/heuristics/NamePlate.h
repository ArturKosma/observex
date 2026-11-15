#pragma once

#include "core/GameStats.h"
#include "core/UiLayout.h"

#include <cstdint>
#include <span>
#include <vector>

namespace observex {

// The name plate Lineage 2 draws over a character (the user's screenshots, 2026-11-15): nothing while the mouse is not
// over it; its name between two round orbs while it is (a bronze ring, grey inside); the same with the orbs coloured
// while it is the target (blue; red on a monster fighting: clip 2026-11-13_00-50-14's Goblin). The orbs are 19 px
// across at the user's interface scale and sit 14-16 px left and 13 px right of the name's white letters.
struct PlateFound {
    bool seen = false;
    bool selected = false; // Its orbs coloured: it is the target.
    StatsArea at{};        // From the left orb's left edge to the right orb's right edge.
    StatsArea text{};      // The name between the orbs: its rows and columns.
    int orbs = 0;          // Orbs found in the area looked over (for the why).
};

// The part of a width x height screen image to look over for the plate of the character boxed `over`: above it (more
// for a taller one, as a near character's plate stands farther over its head), 320 px either way of its middle.
StatsArea plateArea(StatsArea over, int width, int height);

// The plate of the character boxed `over`, in `part` (colour pixels round it: plateArea's). Orbs inside any of
// `pieces` (the game's interface) are left out. Of several plates, the one nearest over the box's middle. `work` is
// working memory, kept between calls.
PlateFound findPlate(const ColourPart& part, StatsArea over, std::span<const UiPiece> pieces,
                     std::vector<std::uint8_t>& work);

} // namespace observex
