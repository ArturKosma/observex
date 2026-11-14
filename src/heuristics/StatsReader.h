#pragma once

#include "core/GameStats.h"
#include "heuristics/TextOcr.h"

#include <array>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace observex {

// Reads the game's interface on the pieces the UI finder found (core/UiLayout), by models/ui/stats.txt (its header
// explains its lines):
// - Bars and the numbers on them. The number first, by its glyphs (a font of fixed width, cut by
//   tools/ui/make_digits.py), found by correlation over each glyph's mask (the letter and its dark line, so the bar's
//   colour under it does not matter); then how full the bar is, from where the first run of its colour ends, the
//   columns under the number left out. Past that end the bar must be its dark empty colour, else a window is over it.
//   A number that does not agree with its bar is not kept: one of the two was read wrong.
// - The cast bar: how far its light blue fill's white head is.
// - The skills the user added (the profile's, given by setSkills(), each a miniature as it looked ready): found among
//   the miniatures of the hotbar (heuristics/Miniatures) by their pixels, near either the picture or its darkened
//   picture; then how much of the cooldown is left from the bright sector growing back clockwise, and the yellow
//   seconds written on it. Each is looked for where it was last seen (a few hundred pixels), every image, also while
//   it is not there; the hotbar is looked over when one is missing, three times a second. Where it was, a picture
//   alike in its shapes but neither ready nor darkened (correlation) is the flash after its key was pressed (clip
//   2026-11-14_14-50-29: dark, framed light blue, ~0.6 s). What is cast is the skill whose cooldown began, or that was
//   pressed, last.
// - Words, e.g. the target's name: by Windows' OCR (heuristics/TextOcr), once for each new picture of them.
// Only small parts of the image are read: areas() says which. In observex_stats.dll (heuristics/StatsAbi) and built
// into the app as its fallback. Use one from one thread (read() keeps its working memory and what it saw before).
class StatsReader final : public StatsReading {
public:
    // Reads `folder`/stats.txt and the glyphs' pictures there. False with *error (if given) if any is missing or
    // wrong; the reader then reads nothing.
    bool load(const std::filesystem::path& folder, std::string* error);
    // The same from stats.txt's text; the glyphs' pictures are read from `folder`.
    bool parse(std::string_view text, const std::filesystem::path& folder, std::string* error);

    int bars() const { return static_cast<int>(bars_.size()); }

    int areas(std::span<const UiPiece> pieces, int width, int height, StatsArea* out, int max) const override;
    void read(std::span<const UiPiece> pieces, std::span<const ColourPart> parts, GameStats& out) override;
    void setSkills(std::span<const SkillPicture> skills) override;

    // A number on a bar as a share of its whole: "238/256" -> 0.93, "26.64%" -> 0.2664. False if `text` is neither.
    static bool fraction(std::string_view text, float* share);

    // The added skills it knows (setSkills()).
    int skills() const { return static_cast<int>(skills_.size()); }

private:
    enum class Fill : std::uint8_t { Red, Blue, Lilac };
    // A bar's empty part: dark (no channel light; the default), or dark red, dark blue or grey ("empty COLOUR").
    enum class Empty : std::uint8_t { Dark, Red, Blue, Grey };
    struct GlyphPixel {
        std::int8_t dx = 0;
        std::int8_t dy = 0;
        bool letter = false;  // On the letter (white), else on its dark line.
        float centred = 0.0f; // Its grey minus the mean of the masked pixels.
    };
    struct Glyph {
        char c = 0;
        std::vector<GlyphPixel> pixels;
        float spread = 0.0f;           // Square root of the sum of centred squared.
        std::vector<std::uint64_t> rows; // A yellow font's: each row's letter pixels as bits (column x: bit x).
        int ons = 0;                     // How many there are.
    };
    struct Font {
        std::string name;
        int pitch = 0;
        int width = 0;
        int height = 0;
        int letterFrom = 0; // The columns of a picture any glyph's letter is in (to just before letterTo).
        int letterTo = 0;
        bool yellow = false; // Yellow letters (a cooldown's seconds), found by colour, not by grey.
        std::vector<Glyph> glyphs;
    };
    // The cast bar ("cast" line).
    struct Cast {
        std::array<char, 16> piece{};
        int top = 0;
        int bottom = 0;
        int left = 0;  // The fill's head here: 0%; at right: 100%.
        int right = 0;
    };
    // An added skill: its miniature when it was added (ready), grey, side x side.
    struct Skill {
        std::string name;
        bool cooldown = true; // Its cooldown is read (else it is always ready when seen).
        int side = 0;
        std::vector<float> pixels;
        std::vector<std::uint8_t> use; // 1 where it is compared (not its frame, the key's letters or the seconds).
        int uses = 0;
        // Where its cooldown's wipe is read: each compared pixel bright and darkened tell apart on, its sector (of
        // kSectors, from 12 o'clock clockwise round the middle); kSectors elsewhere.
        std::vector<std::uint8_t> sector;
        // For the correlation with a picture (alike()): each compared pixel minus their mean, and the root of their
        // sum of squares.
        std::vector<float> centred;
        float spread = 0.0f;
    };
    // Words written in white on a piece ("words" line), e.g. the target's name: read by Windows' OCR once per picture.
    struct Words {
        std::array<char, 16> piece{};
        std::array<char, 32> name{};
        int top = 0;
        int bottom = 0;
        int left = 0;  // From the piece's left.
        int right = 0; // Below 1: from the piece's right side.
    };
    // A window's close button (its X, light) at TOP BOTTOM rows from its piece's top and LEFT RIGHT columns from its right
    // side: while it is not there, another window covers the piece's right end, and nothing on the piece is read.
    struct Close {
        std::array<char, 16> piece{};
        int top = 0;
        int bottom = 0;
        int left = 0;
        int right = 0;
    };
    // One gauge line of stats.txt, in its order: a bar, the cast bar, all added skills, or words.
    struct Line {
        enum class Kind : std::uint8_t { Bar, Cast, Skills, Words } kind = Kind::Bar;
        int index = 0; // Into bars_ for a bar, words_ for words.
    };
    // A miniature found on this image, and the part its pixels are in.
    struct Found {
        StatsArea box;
        const ColourPart* part = nullptr;
        int skill = -1;        // The skill it is, or -1.
        float score = 1e9f;    // How far it is from that skill (grey levels; lower is nearer).
        bool pressed = false;  // Alike in its shapes, but neither ready nor darkened: the flash after its key.
    };
    struct Bar {
        std::array<char, 16> name{};
        std::array<char, 16> piece{};
        Fill fill = Fill::Red;
        Empty empty = Empty::Dark;
        int top = 0;
        int bottom = 0;
        int left = 0;
        int right = 0; // Below 1: from the piece's right side.
        int font = -1; // -1: no number on it.
        int textRow = 0;
        int textFrom = 0;
    };
    struct Match {
        float score = 0.0f;
        int x = 0;
        int glyph = 0;
    };

    // The number on `bar` (its columns end at x1) in the piece at (px, py), from `part`'s pixels; "" if none. When
    // one is read, *spanFrom and *spanTo are the screen columns it covers (to just before *spanTo).
    void readText(const Bar& bar, int px, int py, int x1, const ColourPart& part, std::array<char, 32>& text,
                  int* spanFrom, int* spanTo);
    // Reads `bar` in `piece` from `part`'s pixels into `gauge` (its piece and name already set).
    void readBar(const Bar& bar, const UiPiece& piece, const ColourPart& part, GameGauge& gauge);
    // How far the cast is on the cast bar in `piece` (0 to 1), from `part`'s pixels.
    float readCast(const UiPiece& piece, const ColourPart& part) const;
    // The skills where each was last seen, and (lookOver_) among the miniatures in every piece named skillsPiece_.
    // Says why one is not found into why_.
    void findSkills(std::span<const UiPiece> pieces, std::span<const ColourPart> parts);
    // How far `skill` is from the miniature at `box` (a pixel either way: the nearest, into *best), grey levels on
    // average; above kSkillMatch if none is that near.
    float distance(const Skill& skill, StatsArea box, const ColourPart& part, StatsArea* best) const;
    // How alike `skill`'s picture and the one at `box` are in their shapes, whatever their brightness (correlation of
    // the compared pixels, -1 to 1; a pixel either way: the most alike, into *best).
    // Whether CLOSE's button is on PIECE (in one of PARTS).
    bool closeShown(const Close& close, const UiPiece& piece, std::span<const ColourPart> parts) const;
    // Whether a miniature's light edge is round BOX (half its edge pixels light in every channel).
    bool framed(StatsArea box, const ColourPart& part) const;
    float alike(const Skill& skill, StatsArea box, const ColourPart& part, StatsArea* best) const;
    // Adds `text` to why_ (" | " between).
    void because(std::string_view text);
    // The share of `skill`'s cooldown left on the miniature at `box`: 0 when ready.
    float cooldownLeft(const Skill& skill, StatsArea box, const ColourPart& part);
    // The seconds written on the miniature at `box` with the yellow font (skillsFont_); "" if none was read.
    void readSeconds(StatsArea box, const ColourPart& part, std::array<char, 32>& text);
    // The words of `words` in `piece` from `part`'s pixels into `text` ("" if none).
    void readWords(const Words& words, const UiPiece& piece, const ColourPart& part, std::array<char, 32>& text);

    std::vector<Font> fonts_;
    std::vector<Bar> bars_;
    std::vector<Words> words_;
    std::vector<Close> closes_;
    std::vector<Line> lines_;
    Cast cast_;
    std::array<char, 16> skillsPiece_{}; // Where the added skills are looked for; empty: no "skills" line.
    int skillsFont_ = -1;                // The yellow font their seconds are written in, or -1.
    std::vector<Skill> skills_;

    // What is kept from image to image, for the cast's name: whether each skill was cooling on the image before, and
    // the read it began on (0: not seen begin).
    std::vector<std::uint8_t> cooling_;
    std::vector<std::uint64_t> began_;
    std::uint64_t reads_ = 0;
    // So finding the skills costs next to nothing: where each was last seen (x1 0: never), kept while it is not there
    // (it is looked for there again on every image), and whether the next image needs the whole pieces looked over (a
    // skill missing; three times a second while one is).
    std::vector<StatsArea> skillAt_;
    std::vector<std::uint64_t> seenOn_; // The read each was last seen ready or cooling on (0: not yet).
    std::vector<std::uint8_t> pressed_; // Each skill pressed on the image before.
    bool lookOver_ = true;
    std::uint64_t lookedOver_ = 0; // The read the pieces were last looked over on.
    std::string why_;              // This read's why (GameStats::why).
    // Every picture of words read so far, by its hash, and what it said (names repeat: each is read once).
    std::vector<std::pair<std::uint64_t, std::string>> wordsRead_;
    std::vector<std::string> namesAgreed_; // Names two drawings of one picture read alike (most trusted).
    // Each words line's white pixels on the image before and what they said: the same picture again is not read.
    struct LastWords {
        std::vector<std::uint8_t> mask;
        int width = 0;
        int height = 0;
        std::string said;
    };
    std::vector<LastWords> lastWords_;
    std::vector<std::uint8_t> wordsMask_; // Working memory: this image's white pixels.
    std::array<char, 32> targetName_{}; // The target's name: the one read most while its window showed.
    std::vector<std::pair<std::array<char, 32>, int>> nameVotes_; // Its names read, and on how many readings.
    // How wide the words picture the target's name was read from is, and this read's (0: no words; wordsWidth_ is the last
    // readWords' own).
    int nameWidth_ = 0;
    // Per bar (bars_), how far left of the bar's middle its number began when last read; -1 before.
    std::vector<int> numberHalf_;
    // The target window seen whole (its close button there) on the image before, and its top left then: while a window
    // covers its right end it stays the target.
    bool targetWhole_ = false;
    std::array<int, 2> targetLeft_{};
    int targetWordsWidth_ = 0;
    int wordsWidth_ = 0;
    // Target Changed: the target last said (empty: None), and a new one held on targetHeld_ readings so far.
    std::array<char, 32> target_{};
    std::array<char, 32> targetNext_{};
    int targetHeld_ = 0;
    TextOcr ocr_;
    std::string ocrError_;

    // Working memory, kept from read to read.
    std::vector<float> grey_;
    std::vector<float> bright_; // The brightest grey of each column.
    std::vector<Match> matches_;
    std::vector<std::uint8_t> pieceGrey_; // A piece's pixels in grey, for finding miniatures in it; words to read.
    std::vector<std::uint64_t> yellowRows_; // A miniature's yellow pixels (its seconds), each row as bits.
    std::vector<Found> found_;
    std::vector<float> lefts_;            // Each skill's cooldown left on this image (-1: not found).
    std::vector<float> sectorBright_;     // Per sector of a miniature: bright pixels, and all.
    std::vector<float> sectorAll_;
};

} // namespace observex
