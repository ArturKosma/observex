#pragma once

#include "app/Theme.h"
#include "core/Detection.h"
#include "core/Ground.h"
#include "core/UiLayout.h"

#include <imgui.h>

#include <span>
#include <string>
#include <vector>

namespace observex {

// The characters observex shows over a screen image, as the live view draws them and the clip screen redraws them
// from a clip (the same look, so a clip shows exactly what was seen): each thing's box and its tag ("Character 81%";
// with `showKinds` and a model with kinds, its kind's name and colour as the review shows them, e.g. "Monster 81%" in
// green, or "Character?" in silver for one it cannot tell). `kinds` are the model's kind ids by number ("monster").
// The image of `imageWidth` pixels is drawn from `min` to `max`. Surest last, so it is on top. With `distances` (the
// same count as `things`; the live view's, core/Distances: game units, under 0 none) each tag also says how far the
// thing is from the own character ("Monster 81% · 610", to the nearest 10; "610?" while the camera's pitch is only
// presumed, `distancesSure` false).
void drawThings(ImDrawList* drawList, ImFont* font, ImVec2 min, ImVec2 max, float imageWidth,
                std::span<const Tracked> things, const std::vector<std::string>& kinds, bool showKinds,
                float dpiScale, std::span<const float> distances = {}, bool distancesSure = true);

// The pieces of the game's interface the UI finder found (the Show flag UI), over the screen image drawn as above:
// each one filled see-through, outlined and tagged with its name ("Chat", or "Window" for one found by its frame).
void drawUiPieces(ImDrawList* drawList, ImFont* font, ImVec2 min, ImVec2 max, float imageWidth,
                  std::span<const UiPiece> pieces, float dpiScale);

// Drops found (the Show flag Drops): each one's four corners just outside it and its tag "Drop 87%", in gold. The
// square the drop finder looks at closely is not drawn (the user asked, 2026-11-15: its corners, 192 px and more,
// looked like a drop's outline). Over the screen image drawn as above. `distances` as drawThings' ("Drop 87% · 340").
void drawLoot(ImDrawList* drawList, ImFont* font, ImVec2 min, ImVec2 max, float imageWidth,
              std::span<const Detection> drops, float dpiScale, std::span<const float> distances = {},
              bool distancesSure = true);

// The ground the camera sees (the Show flag Ground; the user asked, 2026-11-12: a green see-through plane that turns
// with the camera and reacts to zooms): a grid fixed to the ground around the own character's feet, its cells filled
// fainter the farther they are, a ring round the feet, and the horizon; fainter still while the pitch is only
// presumed (GroundCamera::pitchMeasured). Since 2026-11-16 (the user: "the grid points should wrap around terrain")
// laid on `heights` (the depth maps' terrain, core/Ground's groundGrid; null: flat), each cell's sides bent in
// kGridSteps pieces; later that morning hidden where the terrain's newest map saw something in front (a trunk, a
// crest, a character: groundHidden), the lines cut every few pixels. Nothing while the camera is unknown. Over the screen image drawn as above. UI thread only.
void drawGround(ImDrawList* drawList, ImVec2 min, ImVec2 max, float imageWidth, const GroundCamera& camera,
                const GroundHeights* heights, float dpiScale);

} // namespace observex
