#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace observex {

// A picture of a hotbar slot to read its key from: side x side pixels from the miniature's top left (its light frame
// is row 0 and column 0), `step` bytes a pixel, blue, green and red first (BGRA 4, a profile's BGR 3), rows `pitch`
// bytes apart.
struct KeyPicture {
    const std::uint8_t* pixels = nullptr;
    std::size_t pitch = 0;
    int step = 4;
    int side = 0;
};

// One part of a label as read: a glyph known (its text), or letters not known ("?"), over picture columns from..to.
struct KeyPart {
    std::string text;
    bool known = false;
    int from = 0;
    int to = 0;
    float score = 0.0f; // A known glyph's correlation.
};

// The keys written on the hotbar's slots (the user asked, 2026-11-15: each skill's shortcut, saved in the profile for
// the Pico to press, following the skill when it is moved to another slot). Written at a miniature's top left in cream
// letters with a dark rim, in a proportional anti-aliased font whose letters may touch, e.g. "F2", "F12", "A+K1" (any
// key the user binds), so they are read by glyphs learned from slots whose key is known (models/ui/keys.txt), not by
// Windows' OCR (it reads them as "FIO", 2026-11-15).
// - Letter pixels: grey at least kLight, not bluish (the light blue of a pressed flash, the white of a picture under
//   them are not letters only by brightness, but most pictures are not cream). Dark: grey at most kDark.
// - The letters are in rows 1 to 0.22 of the side; the label starts in the first fifth of the width.
// - Glyph by glyph from the left: each glyph (its letter pixels and the dark pixels round them; the rest of its box is
//   not compared) is tried a column before to kGap after where the next letters start, a row either way; it is there
//   where the correlation of its grey with the picture's is at least kMatch, its letters at least kContrast lighter
//   than their rim, and no more than kExtra of its letter pixels' count of rimmed letter pixels there that it does not
//   have (a "1" inside a "4"). Of two within 0.02 the wider wins ("F12" taught as one beats "F" then "1").
// - Letters no glyph matches are one '?' if they look like a letter (at least kMinRows rows tall from near the top):
//   the corner of a picture under the label is left out.
// Glyphs are by the size they were learned at (a slot's side; another UI scale has other glyphs).
class KeyFont {
public:
    // The glyphs in `text` (keys.txt's whole text); false with *error (if given) if a line is wrong (then none kept).
    bool parse(std::string_view text, std::string* error);
    // The glyphs in `file`; a file not there is a font with none (true).
    bool load(const std::filesystem::path& file, std::string* error);
    std::string write() const;
    // Written to `file` whole: a .tmp file first, then renamed over it (a reader never sees half of it).
    bool save(const std::filesystem::path& file, std::string* error) const;

    // The key written on `picture`: empty if no letters are written there; a '?' for each letter not known.
    std::string read(const KeyPicture& picture, std::vector<KeyPart>* parts = nullptr) const;
    // Learns that the key written on `picture` is `typed` (the user typed it, or the bench was told): the letters not
    // known get the typed text between the known ones (a letter each when as many, else one glyph over them all); when
    // the known ones do not agree with `typed`, one glyph over the whole label. Returns how many glyphs it learned (0:
    // no letters there).
    int teach(const KeyPicture& picture, std::string_view typed);

    int glyphs() const { return static_cast<int>(glyphs_.size()); }

private:
    struct Glyph {
        std::string text; // May be empty: something under labels that is not a letter.
        int side = 0;     // The slot's side it was learned at.
        int width = 0;
        int height = 0;
        std::vector<std::uint8_t> grey;
        std::vector<char> kind; // 'L' its letter, 'D' the dark rim round it, '.' not compared.
        // Made from those (prepare()).
        std::vector<float> centred; // Grey minus the mean of the compared pixels; 0 where not compared.
        float spread = 0.0f;        // Root of their squares' sum.
        int lead = 0;               // Its first and last columns with letter pixels.
        int trail = 0;
        int letters = 0;
    };
    // A picture's pixels sorted: grey, letter, dark (side x side).
    struct Seen {
        int side = 0;
        int top = 1;     // Letter rows top to just before bottom; a glyph is height rows from top.
        int bottom = 1;
        std::vector<int> grey;
        std::vector<std::uint8_t> letter;
        std::vector<std::uint8_t> dark;
        std::vector<int> columns; // Letter pixels in the letter rows, per column (0 at the edges).
    };

    static Seen see(const KeyPicture& picture);
    static bool prepare(Glyph& glyph);
    // The glyph of `seen`'s letters in columns a to z, saying `text`; false (none) if there are no letters there.
    static bool cut(const Seen& seen, int a, int z, std::string text, Glyph* glyph);
    // How `glyph` correlates with `seen` with its left column at x and dy rows from the letters' top; below 0 where it
    // cannot be (off the picture, too little contrast, or letters there it does not have).
    static float score(const Glyph& glyph, const Seen& seen, int x, int dy);
    std::string read(const Seen& seen, std::vector<KeyPart>* parts) const;

    std::vector<Glyph> glyphs_;
};

// The glyphs' file in the UI folder (models/ui), read by the stats reader (heuristics/StatsReader) with stats.txt.
inline constexpr char kKeysFile[] = "keys.txt";

} // namespace observex
