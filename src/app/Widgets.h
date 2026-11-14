#pragma once

#include "app/Theme.h"

#include <imgui.h>

#include <cstdint>

namespace observex {

enum class Icon : std::uint8_t {
    None,
    Monitor, // A screen on a stand.
    Record,  // A red dot.
    Stop,    // A red square.
    Gear,    // Settings.
    Picture, // A framed picture: a hill and a sun.
    Close,   // A cross.
    Back,    // A chevron pointing left.
    Forward, // A chevron pointing right.
    Eye,     // An open eye: what is shown over the screen.
    Replay,  // A clock face with an arrow going back around it: save the last seconds.
    Play,    // A triangle pointing right.
    Pause,   // Two bars.
    StepBack,    // A bar, then a triangle pointing left at it: one frame back.
    StepForward, // A triangle pointing right at a bar: one frame on.
    ToStart,     // A bar, then two triangles pointing left at it: the first frame.
    ToEnd,       // Two triangles pointing right at a bar: the last frame.
    Guesses,     // A box's four corners: what is drawn over a clip.
    Plus,        // Add something.
    Folder,      // A folder's outline with its tab: open a saved file (the State tile's Load).
};

// A top-bar button that plainly looks clickable: a rounded border, an icon, then an optional muted caption,
// the main text in medium weight and an optional muted detail, then an optional dropdown arrow. With only
// an icon it is square. Fill and border react to the mouse.
struct BarButton {
    static constexpr float kHeight = 34.0f; // Before DPI scaling.

    Icon icon = Icon::None;
    const char* caption = nullptr; // Muted, before the text, e.g. "Observing".
    const char* text = nullptr;
    const char* detail = nullptr;     // Muted, after the text, e.g. a key or a counter.
    bool arrow = false;               // A dropdown arrow; it points up while `open`.
    bool open = false;                // Drawn highlighted, e.g. while its popup is open or while recording.
    bool enabled = true;              // Disabled buttons are dimmed and cannot be clicked.
    ImU32 highlight = theme::kAccent; // Border colour while `open`.
    float progress = -1.0f;           // 0 to 1: a bar that far along its bottom edge, in `highlight`; below 0: none.

    float width(const theme::Fonts& fonts) const;

    // Draws the button at the cursor. Returns true when clicked. `id` must be unique in the window.
    bool draw(const char* id, const theme::Fonts& fonts) const;
};

// A tab in the strip under the top bar, showing or hiding its tile (the user asked, 2026-11-12: Live, State and Stats):
// `text` in medium weight, muted while off, underlined in the accent colour while on. Drawn at the cursor, `height`
// high. Returns true when clicked.
bool drawTab(const char* id, const char* text, bool on, float height, const theme::Fonts& fonts);

} // namespace observex
