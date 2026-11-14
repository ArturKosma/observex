#pragma once

#include "core/Depth.h"
#include "core/Heuristics.h"

#include <span>

namespace observex {

// What the depth network's map says of the camera's pitch (the user asked, 2026-11-12: "the depth will have to run
// all the time anyway for other systems, so make it run all the time and make the grid position according to it.
// precision of the grid has top priority"). The pitch came only from the ground's motion, so after a blurred tilt
// that it under-took a still camera kept it 6 to 7 degrees off (clip 2026-11-12_20-16-13 from 7.35 s on).
//
// On flat ground the true inverse depth of a screen row falls on a straight line up the screen and reaches 0 at the
// horizon. The network's map is inverse depth up to a scale and an offset of each image's own, so on its own it
// cannot say where the horizon is; but on this game's ground what the offset does depends on the pitch alone, the
// same on every clip. Measured (2026-11-12, scratchpad ground/depth_inverse.py of that session: the network over
// every 4th frame of 5 clips, about 1 100 maps, against the grid's pitch where the ground's motion measured it):
// where the line through the near rows reaches 0, taken as a pitch, is the pitch itself up to 15 degrees, 3 to 4
// short of it at 20 to 25, and ever more squeezed beyond: 25 at 40 degrees, 31 at 58, 37 at 80 (seen from straight
// down the network still makes a slope). depthPitch() undoes that. One map's is off by about 2 degrees at ordinary
// pitches and 5 looking steeply down (where the table is steep); on a still view each map says the same to 0.2.
//
// measureDepthLine(): the rows from kBandTop to kBandBottom of the screen's height (the near ground: all of it below
// the horizon while the camera looks down more than 3 degrees; above the hotbar and the menu bar), the columns from
// kBandLeft to kBandRight (clear of the chat and the minimap); each row's median over its world pixels (not the map's
// interface, DepthInput::ignore, nor `avoid`: the characters; kMargin map pixels wider), if at least kRowShare of
// them are; a straight line through those rows, if at least kRowsShare of the band's rows have one.
inline constexpr float kDepthBandTop = 0.444f; // Of the screen's height: rows 640 to 1080 of 1440,
inline constexpr float kDepthBandBottom = 0.75f;
inline constexpr float kDepthBandLeft = 0.273f; // of its width: columns 700 to 1900 of 2560.
inline constexpr float kDepthBandRight = 0.742f;
inline constexpr int kDepthMargin = 2;          // Map pixels round what is left out.
inline constexpr float kDepthRowShare = 0.25f;  // A row's median counts with this share of its pixels the world's,
inline constexpr float kDepthRowsShare = 0.5f;  // and the line with this share of the band's rows.

// The ground's depth down the screen, as measureDepthLine() found it.
struct DepthLine {
    bool known = false;    // Enough rows had enough of the world:
    float zeroRow = 0.0f;  // the screen row the line through them reaches 0 at (pixels; above the screen looking down),
    float straight = 0.0f; // how far the rows are off it on average (root mean square), of their range (open flat
                           // ground: under 0.01; past the horizon or over a hill: more),
    int rows = 0;          // and how many rows it went through.
};
DepthLine measureDepthLine(const DepthInput& map, std::span<const GreyArea> avoid);

// What a line's zero row taken as a pitch, `seen` (radians: atan((middle - zeroRow) / focal)), means (radians), by
// the measured table; and how much one map of it counts: 1 where the table goes one for one, less where it is steep
// (its slope squared, inverted: there each map's error is that much larger).
struct DepthPitch {
    float pitch = 0.0f;
    float weight = 0.0f;
};
DepthPitch depthPitch(float seen);

} // namespace observex
