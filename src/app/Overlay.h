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
// The image of `imageWidth` pixels is drawn from `min` to `max`. Surest last, so it is on top.
void drawThings(ImDrawList* drawList, ImFont* font, ImVec2 min, ImVec2 max, float imageWidth,
                std::span<const Tracked> things, const std::vector<std::string>& kinds, bool showKinds,
                float dpiScale);

// The pieces of the game's interface the UI finder found (the Show flag UI), over the screen image drawn as above:
// each one filled see-through, outlined and tagged with its name ("Chat", or "Window" for one found by its frame).
void drawUiPieces(ImDrawList* drawList, ImFont* font, ImVec2 min, ImVec2 max, float imageWidth,
                  std::span<const UiPiece> pieces, float dpiScale);

// The ground the camera sees (the Show flag Ground; the user asked, 2026-11-12: a green see-through plane that turns
// with the camera and reacts to zooms): a grid fixed to the ground around the own character's feet, its cells filled
// fainter the farther they are, a ring round the feet, and the horizon; fainter still while the pitch is only
// presumed (GroundCamera::pitchMeasured). Nothing while the camera is unknown. Over the screen image drawn as above.
void drawGround(ImDrawList* drawList, ImVec2 min, ImVec2 max, float imageWidth, const GroundCamera& camera,
                float dpiScale);

} // namespace observex
