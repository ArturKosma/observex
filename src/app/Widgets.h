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

    float width(const theme::Fonts& fonts) const;

    // Draws the button at the cursor. Returns true when clicked. `id` must be unique in the window.
    bool draw(const char* id, const theme::Fonts& fonts) const;
};

} // namespace observex
