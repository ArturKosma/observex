#pragma once

#include <imgui.h>

namespace observex {

// The look of observex: the Inter font, a dark neutral palette and one accent colour.
namespace theme {

inline constexpr ImU32 kBackground = IM_COL32(14, 14, 17, 255);
inline constexpr ImU32 kSurface = IM_COL32(22, 22, 26, 255); // Popups and placeholders.
inline constexpr ImU32 kSurfaceHover = IM_COL32(31, 31, 37, 255);
inline constexpr ImU32 kSurfaceActive = IM_COL32(40, 40, 47, 255);
inline constexpr ImU32 kBorder = IM_COL32(38, 38, 45, 255);
inline constexpr ImU32 kBorderHover = IM_COL32(62, 62, 73, 255);
inline constexpr ImU32 kText = IM_COL32(236, 236, 241, 255);
inline constexpr ImU32 kTextMuted = IM_COL32(142, 142, 154, 255);
inline constexpr ImU32 kAccent = IM_COL32(139, 123, 255, 255);
inline constexpr ImU32 kRecord = IM_COL32(242, 85, 90, 255); // The record dot and stop square.
inline constexpr ImU32 kYes = IM_COL32(74, 214, 130, 255);   // Review marks answered yes.
inline constexpr ImU32 kOverlay = IM_COL32(14, 14, 17, 210); // Labels drawn over screen images.
inline constexpr ImU32 kHalo = IM_COL32(0, 0, 0, 140); // Behind box outlines, so they show on snow as well as rock.

// `color` with its opacity multiplied by `alpha` (0 to 1), e.g. for things pushed into the background.
constexpr ImU32 faded(ImU32 color, float alpha) {
    const auto opacity = static_cast<ImU32>(static_cast<float>(color >> IM_COL32_A_SHIFT) * alpha + 0.5f);
    return (color & ~IM_COL32_A_MASK) | (opacity << IM_COL32_A_SHIFT);
}

// Same colour as a Win32 COLORREF (0x00BBGGRR), for the window's title bar.
constexpr unsigned long toColorRef(ImU32 color) {
    return color & 0x00FFFFFFu; // ImU32 is 0xAABBGGRR, so dropping alpha gives COLORREF.
}

struct Fonts {
    ImFont* regular = nullptr;
    ImFont* medium = nullptr;
};

// Loads the fonts (embedded in observex.exe) and sets ImGui's colours and sizes for `dpiScale`.
// Call once, after ImGui::CreateContext(). Throws if the fonts are missing.
Fonts apply(float dpiScale);

// A small label on a dark rounded backing, placed so that `pivot` (0 to 1 on each axis, e.g. 0.5, 0 for
// top centre) of the label sits at `anchor`. An optional `detail` goes on a second line in a muted colour.
void drawLabel(ImDrawList* drawList, ImVec2 anchor, ImVec2 pivot, const char* text, const char* detail = nullptr);

// A box over a screen image, e.g. around a monster: a dark halo, then a `thickness` wide outline in `color`. The
// halo fades with `color`'s opacity.
void drawBox(ImDrawList* drawList, ImVec2 min, ImVec2 max, ImU32 color, float thickness);

// The tag naming what a box is, e.g. "Monster 81%": `text` on a `fill` backing, just above the top left corner of
// the box drawBox drew from `boxMin` with `thickness`, or just inside the box if there is no room above `area`'s
// top. Kept inside `area` (the image) left to right.
void drawTag(ImDrawList* drawList, ImFont* font, ImVec2 boxMin, float thickness, ImVec2 areaMin, ImVec2 areaMax,
             const char* text, ImU32 fill, ImU32 textColor);

// The observex mark (a ring and a pupil, as in the app icon) with its centre at `centre`.
void drawLogo(ImDrawList* drawList, ImVec2 centre, float size);

} // namespace theme

} // namespace observex
