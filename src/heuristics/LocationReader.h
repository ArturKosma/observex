#pragma once

#include "core/GameStats.h"
#include "core/UiLayout.h"
#include "heuristics/TextOcr.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace observex {

// Reads the game's answer to /loc among its system messages (the user asked, 2026-11-15: "it keeps watching the chat
// window ingame to read it and sync"): "Current location: -10,163 / 26,863 / -3,208 (Near the Dark Elf Village)",
// white, written at the bottom of the Messages window when it comes (newer messages push it up).
//
// Only the window's lowest rows are read (kRows: the newest lines), and only lines that were not there on the image
// before are read with Windows' OCR (heuristics/TextOcr, ~2 ms a line; a picture read before is not read again), so it
// costs next to nothing between messages. A line is new when no line on the image before has the same letters (at
// least kSame of their pixels, where they lie in it: the window is see-through and the letters' edges change with
// the world behind them, so the pixels are never the same twice; a line with others' letters shares at most a third,
// 2026-11-15). A new white line among the newest kNewest that says "location" and three
// numbers is an answer, taken when two drawings of it (smooth x3, blocks x4) read the same numbers, else the
// most-read of three (a third, smooth x4). The answer's numbers may go on in the line under it (a narrow window wraps
// them). Lines shown when it first sees the window are not new (an old answer must not move the character): the
// first /loc after observex starts is read. Lines higher up are never taken (scrolling the window back must not
// either).
// Use one from one thread.
class LocationReader {
public:
    static constexpr int kRows = 130;  // The window's lowest rows read (about seven lines of 18 pixels),
    static constexpr int kBottom = 22; // above this many at its bottom (the scroll bar's arrow, the edge),
    static constexpr int kLeft = 24;   // right of this many at its left (the scroll bar).
    static constexpr int kNewest = 3;  // Of the lines read, the lowest this many may be an answer.
    static constexpr float kSame = 0.7f; // A line's letter pixels shared with one before (of both's): the same line.

    // The part of the screen it reads of the Messages window `piece`.
    static StatsArea area(const UiPiece& piece, int width, int height);
    // Reads the newest lines on `part` (as area() asked) of the Messages window `piece` (either null: not seen). A new
    // answer goes into `out` (its count one more, the numbers, `map` as the minimap was on this image, the text);
    // `out` is kept otherwise. Says what it read or why it did not take it into `why` (empty: nothing to say).
    void read(const UiPiece* piece, const ColourPart* part, const MinimapRead& map, TextOcr& ocr, LocationRead& out,
              std::string& why);
    // The bench's, for a clip that begins with an answer shown: the lines on the first image seen are new too.
    void takeFirstLines() { first_ = false; }

    // The three numbers of an answer's text, after its colon: thousands written with ',' (or '.', as OCR may read
    // them), a minus before a number (also read as a dash or '~'). False if `text` does not say "location" or does not
    // hold three numbers in the game's world.
    static bool parse(std::string_view text, int& x, int& y, int& z);

private:
    struct Line {
        int top = 0; // Rows of the part, to just before bottom.
        int bottom = 0;
        int right = 0; // Its last text column (of the part) plus one.
        bool white = false;
    };
    // The lines on `part` from the top down (their letter pixels into rows_); the one touching the part's top is left
    // out (cut).
    void lines(const ColourPart& part, std::vector<Line>& out);
    // What OCR reads on rows top..bottom, columns kLeft..right of `part`, drawn `scale` times as large, smooth or as
    // blocks.
    std::string ocr(const ColourPart& part, int top, int bottom, int right, int scale, bool smooth, TextOcr& textOcr,
                    std::string* error);
    // An answer's numbers off the lines from `at` (and the one under it if the numbers go on there): false if none.
    bool answer(const ColourPart& part, const std::vector<Line>& found, std::size_t at, TextOcr& textOcr, int& x, int& y,
                int& z, std::string& text, std::string& why);

    bool first_ = true;
    // Whether `line` (of found_, its pixels in rows_) is on the image before (before_, beforeRows_).
    bool seenBefore(const Line& line) const;

    std::vector<Line> found_;
    std::vector<Line> before_;               // The lines on the image before,
    std::vector<std::uint64_t> rows_;        // each row's letter pixels (words_ words a row), this image's
    std::vector<std::uint64_t> beforeRows_;  // and the image before's.
    int words_ = 0;
    int beforeWords_ = 0;
    std::vector<std::uint8_t> picture_;
    std::string error_;
};

} // namespace observex
