#pragma once

#include "core/Marks.h"

namespace observex {

// Which part of an image a view of the image's shape shows: the review's zoom (the user asked, 2026-11-08: a crowd of
// far-off characters was too small to answer). 100% is the whole image fitted to the view, the default and the
// least; the mouse wheel steps through kZoomSteps, keeping the pixel under the mouse where it is.
struct ImageView {
    float zoom = 1.0f; // How many times larger than fitted: 1 (100%) to kZoomSteps' last.
    float left = 0.0f; // The image pixel at the view's left edge,
    float top = 0.0f;  // and at its top edge.

    bool operator==(const ImageView&) const = default;
};

// The zoom levels the wheel steps through, as in image editors: round percentages, finer near 100%. At 800% a
// character 20 pixels tall on a 2560 x 1440 screen is about 94 pixels tall in a view 1500 pixels wide.
inline constexpr float kZoomSteps[] = {1.0f, 1.25f, 1.5f, 2.0f, 2.5f, 3.0f, 4.0f, 5.0f, 6.0f, 8.0f};

// The zoom `steps` levels in from `zoom` (out if negative), within 100% and the last level.
float steppedZoom(float zoom, int steps);

// `view` at `zoom`, with image pixel (`x`, `y`) where it was in the view; kept inside the image (keptInside).
ImageView zoomedAt(const ImageView& view, float zoom, float x, float y, float width, float height);

// `view` moved so it shows no space outside the image (`width` x `height` pixels).
ImageView keptInside(const ImageView& view, float width, float height);

// `view` centred on `box` if the box is not all in it, else as it is; kept inside the image.
ImageView showing(const ImageView& view, const Box& box, float width, float height);

} // namespace observex
