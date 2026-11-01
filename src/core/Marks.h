#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace observex {

// A rectangle in image pixels: left, top, width and height.
struct Box {
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;

    bool contains(int px, int py) const { return px >= x && px < x + width && py >= y && py < y + height; }
    bool operator==(const Box&) const = default;
};

// How much two boxes cover the same area: 0 = not at all, 1 = the same box (intersection over union).
float overlap(const Box& a, const Box& b);

enum class Answer : std::uint8_t {
    None, // Not answered yet.
    Yes,  // It belongs to the category, e.g. it is a monster.
    No,   // It does not.
};

// Something marked on an image for one category (e.g. a monster): a guess, the user's answer, or both.
struct Mark {
    Box box;
    Answer answer = Answer::None;
    float guess = -1.0f;    // How sure the finder was that it belongs to the category (0 to 1); -1 if the user drew it.
    bool carried = false;   // The answer was copied from the image before, not given on this one.
    bool presumed = false;  // Answered yes because the finder was sure enough (presumeSure), not by the user.
};

// The marks of each image, by image file name (e.g. "000001.jpg"), in name order.
using ImageMarks = std::map<std::string, std::vector<Mark>>;

// Reads marks from text. Each image starts with an "image <file name>" line, and each of its marks follows on a
// line of its own: "yes", "no" or a guess (0 to 1), then x, y, width and height, e.g. "yes 1012 540 80 120".
// An image line with no marks after it is kept, with no marks. Blank lines, lines starting with '#' and lines
// it does not understand are skipped.
ImageMarks parseMarks(std::string_view text);

// The text parseMarks reads: `comment` first (each line gets a "# "), then the images in name order. Each mark is
// written with its answer, or with its guess if it has no answer; marks with neither are left out.
std::string formatMarks(const ImageMarks& marks, std::string_view comment = {});

// Gives each unanswered mark in `marks` the answer of the answered mark in `previous` that overlaps it most, if
// they overlap at least `minOverlap`, and flags it as carried. Returns how many marks got an answer.
// This is how one answer covers the same thing on the next image, as long as it has not moved much.
int carryAnswers(const std::vector<Mark>& previous, std::vector<Mark>& marks, float minOverlap = 0.5f);

// Answers yes each unanswered mark the finder was at least `minSure` sure of, and flags it as presumed. Returns how
// many marks got an answer. Run it after carryAnswers, so an answer from the image before wins (a "no" stays no).
int presumeSure(std::vector<Mark>& marks, float minSure);

} // namespace observex
