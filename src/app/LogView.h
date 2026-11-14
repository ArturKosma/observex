#pragma once

#include "core/Log.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace observex {

// The Log tile (the user asked, 2026-11-13: "a very minimalistic logs window, with time of every message"): a line
// each, its time (muted) then what happened, and a muted date row wherever the day changes. A text too long for the
// tile wraps under itself, right of the times (the tile is a narrow column). While scrolled to the bottom it stays
// with the newest line.
class LogView {
public:
    // Draws `lines` (oldest first) into the current window; those from `happened` on are dimmed (a clip's lines after
    // the frame shown). `version` changes whenever `lines` do (LogBook::added()). `empty` is said when there are none.
    void draw(std::span<const LogLine> lines, std::size_t happened, std::uint64_t version, const char* empty,
              float dpiScale);

private:
    // The rows: each line's index, with kDateRow set for the date row before it.
    static constexpr std::uint32_t kDateRow = 0x80000000u;
    std::vector<std::uint32_t> rows_;
    std::vector<float> widths_; // Each line's text, unwrapped.
    std::vector<float> tops_;   // Where each row starts, then where the last ends: rows_.size() + 1 of them.
    std::uint64_t version_ = ~0ull;
    std::size_t count_ = 0;
    std::string lastWhen_;   // The last line's time: lines added after it are measured alone.
    float wrap_ = -1.0f;     // The text width the rows were measured for,
    float rowHeight_ = 0.0f; // and a one-line row's height.
};

} // namespace observex
