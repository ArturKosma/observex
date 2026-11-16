#pragma once

#include "core/GameStats.h"
#include "core/UiLayout.h"
#include "heuristics/TextOcr.h"

#include <cstdint>
#include <string>
#include <vector>

namespace observex {

// The lowest lines of the game's Messages window (its system messages), as one image shows them, and which came in
// since the image before: what heuristics/LocationReader (answers to /loc, white) and heuristics/PickupReader (items
// and adena picked up, yellow: "You have obtained 69 Adena.") read. The stats reader looks once an image, then both
// read the lines it found.
//
// A line: rows with letter pixels (white, the system's: its dimmest letter pixels 160 or so in every channel, its
// middle 220; or yellow), two blank rows ending it; the one touching the part's top is left out (cut). The window is
// see-through, so a line's pixels are never the same twice (the world behind its letters' edges moves); a line is the
// same as one before when they share at least kSame of their letter pixels (where they lie in the line).
//
// New lines (fresh): the window scrolls up a line's height for each message that comes, so the lines before are now
// `shift` rows higher. The smallest shift under which every line now that lies among the lines before is the same as
// the one there wins, and the lines below those are new. This keeps two alike messages apart ("You have obtained 69
// Adena." then "... 72 Adena." share most letters): a line is new by where it is, not only by its letters. When no
// shift fits (the window moved, scrolled back, a bright world behind it), a line is new when no line before shares its
// letters. Lines shown when it first sees the window are not new (an old message must not be read again). Use one
// from one thread.
class MessageLines {
public:
    static constexpr int kRows = 130;  // The window's lowest rows read (about seven lines of 18 pixels),
    static constexpr int kBottom = 22; // above this many at its bottom (the scroll bar's arrow, the edge),
    static constexpr int kLeft = 24;   // right of this many at its left (the scroll bar).
    static constexpr float kSame = 0.7f; // A line's letter pixels shared with one before (of both's): the same line.

    struct Line {
        int top = 0; // Rows of the part, to just before bottom.
        int bottom = 0;
        int right = 0;      // Its last text column (of the part) plus one.
        bool white = false; // Mostly white letters (the system's), else mostly yellow (items, adena).
        bool fresh = false; // Not on the image before: it came in since.
    };

    // The part of the screen it reads of the Messages window `piece`.
    static StatsArea area(const UiPiece& piece, int width, int height);

    // Finds the lines on `part` (as area() asked) of the Messages window; null: not seen (no lines, and the image
    // before stays the one it was last seen on). `part`'s pixels must last until the next look (read() reads them).
    void look(const ColourPart* part);
    // This image's lines, from the top down (none while not seen).
    const std::vector<Line>& lines() const { return found_; }
    // What Windows' OCR reads on rows top..bottom (of the part), columns kLeft..right, drawn `scale` times as large,
    // smooth or as blocks; `yellow` reads yellow letters (else white). Empty while not seen.
    std::string read(int top, int bottom, int right, bool yellow, int scale, bool smooth, TextOcr& ocr,
                     std::string* error);
    // The bench's, for a clip that begins with a message shown: the lines on the first image seen are new too.
    void takeFirstLines() { firstFresh_ = true; }

private:
    // The lines on `part` from the top down into found_ (their letter pixels into rows_).
    void find(const ColourPart& part);
    // How many of `a`'s (of found_, its pixels in rows_) and `b`'s (of before_, in beforeRows_) letter pixels both have,
    // of those either has.
    float alike(const Line& a, const Line& b) const;
    // Marks found_'s new lines (fresh) against before_.
    void markFresh();

    ColourPart part_{};
    bool seen_ = false;       // part_ is this image's.
    bool haveBefore_ = false; // before_ holds an image's lines.
    bool firstFresh_ = false;
    std::vector<Line> found_;
    std::vector<Line> before_;              // The lines on the image before (the last one seen),
    std::vector<std::uint64_t> rows_;       // each row's letter pixels (words_ words a row), this image's
    std::vector<std::uint64_t> beforeRows_; // and the image before's.
    int words_ = 0;
    int beforeWords_ = 0;
    std::vector<int> shifts_;
    std::vector<std::uint8_t> picture_;
};

} // namespace observex
