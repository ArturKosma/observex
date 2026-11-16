#pragma once

#include "core/GameStats.h"
#include "heuristics/MessageLines.h"
#include "heuristics/TextOcr.h"

#include <cstdint>
#include <string>
#include <string_view>

namespace observex {

// Reads the game's answer to /loc among its system messages (the user asked, 2026-11-15: "it keeps watching the chat
// window ingame to read it and sync"): "Current location: -10,163 / 26,863 / -3,208 (Near the Dark Elf Village)",
// white, written at the bottom of the Messages window when it comes (newer messages push it up).
//
// Only the window's lowest rows are looked at (heuristics/MessageLines: the newest lines, and which came in since the
// image before), and only new white lines among the newest kNewest are read with Windows' OCR (heuristics/TextOcr,
// ~2 ms a line), so it costs next to nothing between messages. A new line that says "location" and three numbers is
// an answer, taken when two drawings of it (smooth x3, blocks x4) read the same numbers, else the most-read of three
// (a third, smooth x4). The answer's numbers may go on in the line under it (a narrow window wraps them). Lines higher
// up are never taken (scrolling the window back must not either). Use one from one thread.
class LocationReader {
public:
    static constexpr int kNewest = 3; // Of the lines looked at, the lowest this many may be an answer.

    // Reads this image's new lines (`lines`, MessageLines::look done on it). A new answer goes into `out` (its count one
    // more, the numbers, `map` as the minimap was on this image, the text); `out` is kept otherwise. Says what it read
    // or why it did not take it into `why` (empty: nothing to say).
    void read(MessageLines& lines, const MinimapRead& map, TextOcr& ocr, LocationRead& out, std::string& why);

    // The three numbers of an answer's text, after its colon: thousands written with ',' (or '.', as OCR may read
    // them), a minus before a number (also read as a dash or '~'). False if `text` does not say "location" or does not
    // hold three numbers in the game's world.
    static bool parse(std::string_view text, int& x, int& y, int& z);

private:
    // An answer's numbers off the line `at` of `lines` (and the one under it if the numbers go on there): false if none.
    bool answer(MessageLines& lines, std::size_t at, TextOcr& textOcr, int& x, int& y, int& z, std::string& text,
                std::string& why);

    std::string error_;
};

} // namespace observex
