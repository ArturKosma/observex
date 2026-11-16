#pragma once

#include "core/Detection.h"

#include <span>

namespace observex {

// A close-up look round the own character (engine/Recognizer's lookCloser; the user's clip 2026-11-16_14-04-25: an
// Imp hitting the own character from right beside it was mostly boxed together with it, or found too unsure to be
// taken for a monster). Platform-free.

// A part of the screen, in pixels.
struct CloseUpArea {
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
};

// The part of a `screenWidth` x `screenHeight` screen 1/`share` of its width and height with its middle at (`middleX`,
// `middleY`), moved to lie inside the screen.
CloseUpArea closeUpArea(float middleX, float middleY, int screenWidth, int screenHeight, int share);

// Joins a close-up look at `area` into the look at the whole screen: of `whole` (its first `count`), those with their
// middle inside `area` shrunk by `margin` of its width and height on each side are dropped, and of `close` those with
// their middle inside it are taken in their place (the close-up sees them larger; nearer its sides it sees them cut).
// The result, surest first, at most `capacity` of them, is written over `whole`; returns how many.
int joinCloseUp(Detection* whole, int count, std::span<const Detection> close, const CloseUpArea& area, float margin,
                int capacity);

} // namespace observex
