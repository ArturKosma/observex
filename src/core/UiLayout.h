#pragma once

#include "core/Heuristics.h"

#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace observex {

// A piece of the game's interface on a screen image: a window, the chat, the hotbar.
struct UiPiece {
    std::array<char, 24> name{}; // A piece's name from ui.txt, or "Window" for a window found by its frame.
    int x0 = 0;                  // Screen pixels; x1 and y1 just past the last.
    int y0 = 0;
    int x1 = 0;
    int y1 = 0;
};

// What the UI finder found on one screen image.
struct UiPieces {
    static constexpr int kMax = 64;
    int count = 0;
    std::array<UiPiece, kMax> pieces{};
    std::uint64_t image = 0; // The screen image's number (ImageStamp::number); 0: none yet.
};

// Where the game draws its interface (the user asked, 2026-11-12: to hide it from a depth model later, and to read
// from it, e.g. HP or a target's name). Two ways, both on the whole grey screen image, neither needing a model:
// - Framed windows (Map, Skills, Quest, Actions, the minimap...): Lineage 2 draws each with a 1-pixel light frame
//   line, brighter than the pixels two away on both sides. Such lines are found, joined where a tab or button breaks
//   them, and two upright ones standing on a level one, with a level one where they end (or long enough sides), make
//   a window. A window partly under another is found whole: its lines are taken to go on under the one over it.
// - Pieces with no frame (the chat, the hotbar, the status window, the target window): ui.txt names small pictures
//   ("anchors", cut from clips by tools/ui/make_anchors.py) that are found anywhere on the screen by correlation over
//   their mask (only the interface's own pixels, so a see-through background never counts), and where each piece
//   sits relative to its anchor; a side can stretch to another anchor, for windows the user resized. The file's
//   header explains its lines.
// Pieces inside others are left out. Use one from one thread (find() keeps its working memory).
class UiLayout {
public:
    // Reads `folder`/ui.txt and each anchor's NAME.pgm and NAME-mask.pgm there. False with *error (if given) if any
    // is missing or wrong; the layout is then empty.
    bool load(const std::filesystem::path& folder, std::string* error);
    // The same from ui.txt's text; the anchors' pictures are read from `folder`.
    bool parse(std::string_view text, const std::filesystem::path& folder, std::string* error);

    int anchors() const { return static_cast<int>(anchors_.size()); }
    int pieces() const { return static_cast<int>(pieces_.size()); }

    // Finds the interface on `grey`, a whole screen image in grey (a GreyView's full-size level): the framed windows
    // and ui.txt's pieces, without those inside others. Writes up to `max` into `out`, biggest first, and returns
    // how many. No heap allocation once its working memory has grown to the screen's needs.
    int find(const GreyLevel& grey, UiPiece* out, int max);

    // Only the framed windows (inside others left out), as find() finds them. Works with an empty layout too.
    int findFrames(const GreyLevel& grey, UiPiece* out, int max);

    // Where the last find() found each anchor, one line each ("arrow-up: 6 837 0.97, 6 1001 0.95"), for the bench.
    std::string describeHits() const;
    // Why the last find() did not find the piece named `piece`, for a clip's why lines: each of its anchors' best
    // place below its score, if any was near ("target-left-up 0.90 at 1076,8, needs 0.96"); empty if it was found.
    std::string whyNot(std::string_view piece) const;

    // Milliseconds the last find() took for the framed windows and for the anchors and pieces.
    double framesMs() const { return framesMs_; }
    double anchorsMs() const { return anchorsMs_; }

    // A frame line: on row (or column) `at`, from `from` to just before `to`.
    struct Line {
        int at = 0;
        int from = 0;
        int to = 0;
    };

private:
    struct Pixel {
        std::int16_t dx = 0;
        std::int16_t dy = 0;
        float centred = 0.0f; // Its grey minus the mean of the masked pixels.
    };
    struct Pair {         // A bright and a dark masked pixel: where the image is not brighter at the first by at least
        int brightDx = 0; // `margin`, the anchor is not there (checked 16 places at once before the correlation).
        int brightDy = 0;
        int darkDx = 0;
        int darkDy = 0;
        std::uint8_t margin = 0;
    };
    struct Hit {
        int x = 0;
        int y = 0;
        float score = 0.0f;
    };
    struct Anchor {
        std::string name;
        float minScore = 0.8f;
        int width = 0;
        int height = 0;
        std::vector<Pixel> pixels; // The masked ones.
        double spread = 0.0;       // Sum of centred squared.
        std::vector<Pair> pairs;
        std::vector<Hit> hits; // On the last image.
        Hit nearest{0, 0, -2.0f}; // The best place below minScore on the last image (score -2: none checked).
    };
    enum class Side : std::uint8_t { Left, Right, Top, Bottom };
    struct Stretch {
        Side side = Side::Right;
        bool optional = false;
        bool line = false; // To the nearest frame line that way (e.g. the target window's right edge), not an anchor.
        int anchor = 0;
        int x = 0; // Where the anchor sat when the piece was measured.
        int y = 0;
    };
    struct Piece {
        std::array<char, 24> name{};
        int x0 = 0;
        int y0 = 0;
        int x1 = 0;
        int y1 = 0;
        int anchor = 0;
        int x = 0;
        int y = 0;
        std::vector<Stretch> stretches;
    };

    void findLines(const GreyLevel& grey);
    void findWindows();
    // One look for windows among the lines; `again`: the second, with lines lengthened under the windows of the first.
    void searchWindows(bool again);
    // Lengthens the lines that end at a window's edge from outside it across that window; false if none does.
    bool lengthenUnderWindows();
    void findAnchor(const GreyLevel& grey, Anchor& anchor);
    void placePieces(int width, int height);
    int keepOutermost(UiPiece* out, int max);
    int rowCover(int row, int x0, int x1);
    // The highest (or lowest) row from `lo` to `hi` whose lines cover half of `x0` to `x1`, and come within kReach of
    // column `reach` unless that is kNone (a screen edge standing in for a side).
    bool levelLine(int lo, int hi, int x0, int x1, bool highest, int* row, int reach);

    std::vector<Anchor> anchors_;
    std::vector<Piece> pieces_;
    int width_ = 0; // Of the last image.
    double framesMs_ = 0.0;
    double anchorsMs_ = 0.0;

    // Working memory, kept from find to find.
    std::vector<Line> levels_;   // Level (horizontal) lines, by row.
    std::vector<Line> uprights_; // Upright (vertical) lines.
    std::vector<Line> scratch_;
    struct Column {
        int pieceFrom = -1;
        int pieceLast = -1;
        int joinFrom = -1;
        int joinTo = -1;
        int piecePixels = 0; // Line pixels in the piece,
        int joinPixels = 0;  // and in the joined line's pieces.
    };
    std::vector<Column> columns_;
    std::vector<int> crossing_;
    std::vector<std::pair<int, int>> spans_;
    std::vector<Hit> candidates_;
    std::vector<UiPiece> windows_; // The framed windows.
    std::vector<UiPiece> covers_;  // The first look's, while looking again.
    std::vector<Line> seen_;       // The upright lines as seen, before lengthening.
    std::vector<UiPiece> found_;
};

// A grey picture as a binary PGM (P5, 8-bit): reads it into `pixels` (rows top to bottom), false if it is not one.
bool readPgm(const std::filesystem::path& file, std::vector<std::uint8_t>& pixels, int& width, int& height);
bool writePgm(const std::filesystem::path& file, const std::uint8_t* pixels, int width, int height);

} // namespace observex
