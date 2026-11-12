#include "core/UiLayout.h"

#include <emmintrin.h>

#include <algorithm>
#include <bit>
#include <cctype>
#include <charconv>
#include <chrono>
#include <climits>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>

namespace observex {

namespace {

// Frame lines, measured on Lineage 2 Reborn at 2560 x 1440 (2026-11-12, clips of that day and the day before).
constexpr int kLineMin = 120;   // A frame line's grey is at least this,
constexpr int kLineStep = 40;   // and this much brighter than the pixels two away on both sides.
constexpr int kLineGap = 6;     // Pixels a piece of line may miss (a letter over it, compression).
constexpr int kPieceMin = 12;   // Shorter pieces are left out (letters, icons).
constexpr int kJoin = 30;       // Pieces this far apart on one line join: a tab or a button broke it.
constexpr int kLineLength = 40; // The shortest line counted.
// At least this percent of a joined line's length is line pixels. Text makes lines too, its letters' tops along a
// row (60% at most, 39% on average) and tall letters stacked down a column (mostly under 65%), and they made windows
// over the chat (the user's clip of 2026-11-12_14-21-53). Frame lines are 95% and more, but the Actions window's
// side beside its close button is 69%.
constexpr int kSolid = 65;
constexpr int kReach = 6;       // How far an upright line may stop short of a level one and still meet it.
constexpr int kSide = 40;       // The shortest side of a window.
constexpr int kTitle = 60;      // How far above where its sides start a window's top line may be (its title bar).
constexpr int kLongSides = 150; // Sides this long make a window even when no top line is seen (bright sky behind).
constexpr int kInside = 4;      // A piece this close inside another is left out.
constexpr int kGrid = 8;        // Pixels apart of the points checked for what a window covers.
constexpr std::size_t kMaxWindows = 1024;
constexpr int kLeftEdge = -1; // The screen's edges, among the upright lines a level line meets.
constexpr int kRightEdge = -2;
constexpr int kNone = INT_MIN; // No such place.

// Anchors.
constexpr int kSameRow = 3;   // A side's anchor is on the same row (or column) within this.
constexpr int kPairsMax = 8;  // Bright and dark pixel pairs checked before the correlation.
constexpr int kPairApart = 3; // Pixels at least this far apart each way, so the pairs check different places.
constexpr int kPairStep = 24; // A pair's pixels differ at least this much in the anchor, else it checks little.
constexpr int kMinPixels = 8; // An anchor needs at least this many masked pixels.
constexpr std::size_t kMaxCandidates = 4096;
constexpr std::size_t kMaxHits = 64; // Per anchor and image.

double msSince(std::chrono::steady_clock::time_point start) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
}

void setName(std::array<char, 24>& name, std::string_view text) {
    name.fill('\0');
    std::memcpy(name.data(), text.data(), std::min(text.size(), name.size() - 1));
}

// Per byte (unsigned): 0xFF where `a` is at least `value`.
inline __m128i atLeast(__m128i a, __m128i value) {
    return _mm_cmpeq_epi8(_mm_max_epu8(a, value), a);
}

inline __m128i bytes(const std::uint8_t* at) {
    return _mm_loadu_si128(reinterpret_cast<const __m128i*>(at));
}

std::vector<std::string_view> words(std::string_view line) {
    std::vector<std::string_view> out;
    std::size_t at = 0;
    while (at < line.size()) {
        while (at < line.size() && (line[at] == ' ' || line[at] == '\t')) {
            ++at;
        }
        const std::size_t start = at;
        while (at < line.size() && line[at] != ' ' && line[at] != '\t') {
            ++at;
        }
        if (at > start) {
            out.push_back(line.substr(start, at - start));
        }
    }
    return out;
}

bool toInt(std::string_view text, int& value) {
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    return error == std::errc{} && end == text.data() + text.size();
}

bool toFloat(std::string_view text, float& value) {
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    return error == std::errc{} && end == text.data() + text.size();
}

bool solid(int pixels, int length) {
    return pixels * 100 >= length * kSolid;
}

// One line being built from its pixels in order (`line` holds pieceFrom, pieceLast, joinFrom and joinTo, -1 when
// none, and the pixel counts): pieces (gaps up to kLineGap) of at least kPieceMin, joined over gaps up to kJoin while
// the joined line stays solid; `keep(from, to)` gets each solid joined line of at least kLineLength.
template <typename Line, typename Keep> void endJoined(Line& line, Keep&& keep) {
    if (line.joinFrom >= 0 && line.joinTo - line.joinFrom >= kLineLength &&
        solid(line.joinPixels, line.joinTo - line.joinFrom)) {
        keep(line.joinFrom, line.joinTo);
    }
    line.joinFrom = line.joinTo = -1;
}

template <typename Line, typename Keep> void endPiece(Line& line, Keep&& keep) {
    if (line.pieceFrom >= 0 && line.pieceLast - line.pieceFrom + 1 >= kPieceMin) {
        if (line.joinFrom >= 0 && line.pieceFrom - line.joinTo <= kJoin &&
            solid(line.joinPixels + line.piecePixels, line.pieceLast + 1 - line.joinFrom)) {
            line.joinTo = line.pieceLast + 1;
            line.joinPixels += line.piecePixels;
        } else {
            endJoined(line, keep);
            line.joinFrom = line.pieceFrom;
            line.joinTo = line.pieceLast + 1;
            line.joinPixels = line.piecePixels;
        }
    }
    line.pieceFrom = line.pieceLast = -1;
}

template <typename Line, typename Keep> void addPixel(Line& line, int at, Keep&& keep) {
    if (line.pieceFrom < 0) {
        line.pieceFrom = line.pieceLast = at;
        line.piecePixels = 1;
    } else if (at - line.pieceLast > kLineGap + 1) {
        endPiece(line, keep);
        line.pieceFrom = line.pieceLast = at;
        line.piecePixels = 1;
    } else {
        line.pieceLast = at;
        ++line.piecePixels;
    }
}

template <typename Line, typename Keep> void finishLine(Line& line, Keep&& keep) {
    endPiece(line, keep);
    endJoined(line, keep);
}

// Lines a row (or column) apart that overlap or nearly touch are one line, broken or drawn across two rows: joined,
// at the longer one's row.
void joinNeighbours(std::vector<UiLayout::Line>& lines, std::vector<UiLayout::Line>& scratch) {
    std::sort(lines.begin(), lines.end(), [](const UiLayout::Line& a, const UiLayout::Line& b) {
        return a.from != b.from ? a.from < b.from : a.at < b.at;
    });
    scratch.clear();
    for (const UiLayout::Line& line : lines) {
        const auto hit = std::find_if(scratch.begin(), scratch.end(), [&](const UiLayout::Line& kept) {
            return std::abs(kept.at - line.at) <= 1 && line.from <= kept.to + kJoin && line.to >= kept.from - kJoin;
        });
        if (hit == scratch.end()) {
            scratch.push_back(line);
            continue;
        }
        if (line.to - line.from > hit->to - hit->from) {
            hit->at = line.at;
        }
        hit->from = std::min(hit->from, line.from);
        hit->to = std::max(hit->to, line.to);
    }
    lines.swap(scratch);
}

bool inside(const UiPiece& inner, const UiPiece& outer) {
    return inner.x0 >= outer.x0 - kInside && inner.y0 >= outer.y0 - kInside && inner.x1 <= outer.x1 + kInside &&
           inner.y1 <= outer.y1 + kInside;
}

} // namespace

bool readPgm(const std::filesystem::path& file, std::vector<std::uint8_t>& pixels, int& width, int& height) {
    std::ifstream in(file, std::ios::binary);
    if (!in) {
        return false;
    }
    const std::string data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    std::size_t at = 0;
    const auto token = [&]() -> std::string_view {
        for (;;) {
            while (at < data.size() && std::isspace(static_cast<unsigned char>(data[at]))) {
                ++at;
            }
            if (at < data.size() && data[at] == '#') {
                while (at < data.size() && data[at] != '\n') {
                    ++at;
                }
                continue;
            }
            break;
        }
        const std::size_t start = at;
        while (at < data.size() && !std::isspace(static_cast<unsigned char>(data[at]))) {
            ++at;
        }
        return std::string_view(data).substr(start, at - start);
    };
    int maxValue = 0;
    if (token() != "P5" || !toInt(token(), width) || !toInt(token(), height) || !toInt(token(), maxValue) ||
        maxValue != 255 || width <= 0 || height <= 0) {
        return false;
    }
    ++at; // The one whitespace after the header.
    const std::size_t size = static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
    if (data.size() < at + size) {
        return false;
    }
    pixels.assign(data.begin() + static_cast<std::ptrdiff_t>(at),
                  data.begin() + static_cast<std::ptrdiff_t>(at + size));
    return true;
}

bool writePgm(const std::filesystem::path& file, const std::uint8_t* pixels, int width, int height) {
    std::ofstream out(file, std::ios::binary);
    out << "P5\n" << width << ' ' << height << "\n255\n";
    out.write(reinterpret_cast<const char*>(pixels), static_cast<std::streamsize>(width) * height);
    return static_cast<bool>(out);
}

bool UiLayout::load(const std::filesystem::path& folder, std::string* error) {
    std::ifstream in(folder / "ui.txt", std::ios::binary);
    if (!in) {
        anchors_.clear();
        pieces_.clear();
        if (error != nullptr) {
            const std::u8string file = (folder / "ui.txt").u8string(); // string() throws on a non-ASCII folder.
            *error = "No " + std::string(file.begin(), file.end());
        }
        return false;
    }
    const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    return parse(text, folder, error);
}

bool UiLayout::parse(std::string_view text, const std::filesystem::path& folder, std::string* error) {
    anchors_.clear();
    pieces_.clear();
    int lineNumber = 0;
    const auto fail = [&](const std::string& message) {
        anchors_.clear();
        pieces_.clear();
        if (error != nullptr) {
            *error = "ui.txt line " + std::to_string(lineNumber) + ": " + message;
        }
        return false;
    };
    const auto anchorNamed = [&](std::string_view name) {
        for (std::size_t i = 0; i < anchors_.size(); ++i) {
            if (anchors_[i].name == name) {
                return static_cast<int>(i);
            }
        }
        return -1;
    };

    std::size_t at = 0;
    while (at < text.size()) {
        std::size_t end = text.find('\n', at);
        if (end == std::string_view::npos) {
            end = text.size();
        }
        std::string_view line = text.substr(at, end - at);
        at = end + 1;
        ++lineNumber;
        if (!line.empty() && line.back() == '\r') {
            line.remove_suffix(1);
        }
        const bool indented = !line.empty() && (line[0] == ' ' || line[0] == '\t');
        const std::vector<std::string_view> w = words(line);
        if (w.empty() || w[0][0] == '#') {
            continue;
        }
        if (!indented && w[0] == "anchor") {
            Anchor anchor;
            if (w.size() < 3 || !toFloat(w[2], anchor.minScore)) {
                return fail("expected: anchor NAME MIN_SCORE ...");
            }
            anchor.name = std::string(w[1]);
            if (anchorNamed(anchor.name) >= 0) {
                return fail("anchor " + anchor.name + " is named twice");
            }
            std::vector<std::uint8_t> pixels;
            std::vector<std::uint8_t> mask;
            int maskWidth = 0;
            int maskHeight = 0;
            if (!readPgm(folder / (anchor.name + ".pgm"), pixels, anchor.width, anchor.height) ||
                !readPgm(folder / (anchor.name + "-mask.pgm"), mask, maskWidth, maskHeight)) {
                return fail("could not read " + anchor.name + ".pgm and " + anchor.name +
                            "-mask.pgm (run tools/ui/make_anchors.py)");
            }
            if (maskWidth != anchor.width || maskHeight != anchor.height) {
                return fail(anchor.name + "-mask.pgm is not the size of " + anchor.name + ".pgm");
            }
            std::vector<int> masked;
            double sum = 0.0;
            for (int i = 0; i < anchor.width * anchor.height; ++i) {
                if (mask[static_cast<std::size_t>(i)] >= 128) {
                    masked.push_back(i);
                    sum += pixels[static_cast<std::size_t>(i)];
                }
            }
            if (static_cast<int>(masked.size()) < kMinPixels) {
                return fail(anchor.name + " has fewer than " + std::to_string(kMinPixels) + " steady pixels");
            }
            const double mean = sum / static_cast<double>(masked.size());
            for (const int i : masked) {
                const float centred = static_cast<float>(pixels[static_cast<std::size_t>(i)] - mean);
                anchor.pixels.push_back({static_cast<std::int16_t>(i % anchor.width),
                                         static_cast<std::int16_t>(i / anchor.width), centred});
                anchor.spread += static_cast<double>(centred) * centred;
            }
            if (anchor.spread < static_cast<double>(masked.size())) {
                return fail(anchor.name + " is flat (its steady pixels are all about the same grey)");
            }
            // Pairs: the brightest masked pixels with the darkest, each pixel apart from the others chosen.
            std::sort(masked.begin(), masked.end(), [&](int a, int b) {
                return pixels[static_cast<std::size_t>(a)] < pixels[static_cast<std::size_t>(b)];
            });
            const auto pick = [&](auto first, auto last) {
                std::vector<int> chosen;
                for (auto it = first; it != last && static_cast<int>(chosen.size()) < kPairsMax; ++it) {
                    const bool apart = std::all_of(chosen.begin(), chosen.end(), [&](int c) {
                        return std::abs(c % anchor.width - *it % anchor.width) >= kPairApart ||
                               std::abs(c / anchor.width - *it / anchor.width) >= kPairApart;
                    });
                    if (apart) {
                        chosen.push_back(*it);
                    }
                }
                return chosen;
            };
            const std::vector<int> brights = pick(masked.rbegin(), masked.rend());
            const std::vector<int> darks = pick(masked.begin(), masked.end());
            for (std::size_t k = 0; k < std::min(brights.size(), darks.size()); ++k) {
                const int step =
                    pixels[static_cast<std::size_t>(brights[k])] - pixels[static_cast<std::size_t>(darks[k])];
                if (step < kPairStep) {
                    break;
                }
                anchor.pairs.push_back({brights[k] % anchor.width, brights[k] / anchor.width, darks[k] % anchor.width,
                                        darks[k] / anchor.width, static_cast<std::uint8_t>(step / 2)});
            }
            anchors_.push_back(std::move(anchor));
        } else if (!indented && w[0] == "piece") {
            Piece piece;
            if (w.size() != 10 || w[6] != "at" || !toInt(w[2], piece.x0) || !toInt(w[3], piece.y0) ||
                !toInt(w[4], piece.x1) || !toInt(w[5], piece.y1) || !toInt(w[8], piece.x) || !toInt(w[9], piece.y)) {
                return fail("expected: piece NAME X0 Y0 X1 Y1 at ANCHOR AX AY");
            }
            if (piece.x1 <= piece.x0 || piece.y1 <= piece.y0) {
                return fail("piece " + std::string(w[1]) + " is empty");
            }
            piece.anchor = anchorNamed(w[7]);
            if (piece.anchor < 0) {
                return fail("no anchor " + std::string(w[7]) + " above this line");
            }
            setName(piece.name, w[1]);
            pieces_.push_back(std::move(piece));
        } else if (indented) {
            if (pieces_.empty()) {
                return fail("a side line needs a piece line above it");
            }
            Stretch stretch;
            std::string_view side = w[0];
            if (side.ends_with('?')) {
                stretch.optional = true;
                side.remove_suffix(1);
            }
            if (side == "left") {
                stretch.side = Side::Left;
            } else if (side == "right") {
                stretch.side = Side::Right;
            } else if (side == "top") {
                stretch.side = Side::Top;
            } else if (side == "bottom") {
                stretch.side = Side::Bottom;
            } else {
                return fail("expected: left, right, top or bottom (with ? if it may be missing), not " +
                            std::string(w[0]));
            }
            if (w.size() == 2 && w[1] == "line") {
                stretch.line = true;
                pieces_.back().stretches.push_back(stretch);
                continue;
            }
            if (w.size() != 4 || !toInt(w[2], stretch.x) || !toInt(w[3], stretch.y)) {
                return fail("expected: SIDE ANCHOR X Y, or SIDE line");
            }
            stretch.anchor = anchorNamed(w[1]);
            if (stretch.anchor < 0) {
                return fail("no anchor " + std::string(w[1]) + " above this line");
            }
            pieces_.back().stretches.push_back(stretch);
        } else {
            return fail("expected an anchor, piece or indented side line, not " + std::string(w[0]));
        }
    }
    return true;
}

int UiLayout::find(const GreyLevel& grey, UiPiece* out, int max) {
    found_.clear();
    framesMs_ = anchorsMs_ = 0.0;
    if (grey.pixels == nullptr || grey.width < 8 || grey.height < 8) {
        return 0;
    }
    // The framed windows first (a piece's side may stretch to a frame line, or be hidden under a window), then the
    // pieces, which go first in the list, so a window just as big as a named piece is the one left out.
    const auto framesStart = std::chrono::steady_clock::now();
    findLines(grey);
    findWindows();
    framesMs_ = msSince(framesStart);
    const auto anchorsStart = std::chrono::steady_clock::now();
    for (Anchor& anchor : anchors_) {
        findAnchor(grey, anchor);
    }
    // A place is the anchor's that matches it best: a chat's down arrow also matches the up arrow at 0.91, and taken
    // for one it ended the search for the chat's bottom there. Best by the correlation times the square root of the
    // pixels compared (how sure a match that good is), so a bigger anchor holding a smaller one wins at nearly the
    // same score: a hotbar row (0.984, 431 pixels) over its own up arrow taken for the chat's (0.989, 314).
    for (std::size_t i = 0; i < anchors_.size(); ++i) {
        for (std::size_t j = i + 1; j < anchors_.size(); ++j) {
            Anchor& a = anchors_[i];
            Anchor& b = anchors_[j];
            const float weightA = std::sqrt(static_cast<float>(a.pixels.size()));
            const float weightB = std::sqrt(static_cast<float>(b.pixels.size()));
            for (Hit& hitA : a.hits) {
                for (Hit& hitB : b.hits) {
                    const int across = std::min(hitA.x + a.width, hitB.x + b.width) - std::max(hitA.x, hitB.x);
                    const int down = std::min(hitA.y + a.height, hitB.y + b.height) - std::max(hitA.y, hitB.y);
                    if (hitA.score >= 0.0f && hitB.score >= 0.0f && across > 0 && down > 0 &&
                        2 * across * down >= std::min(a.width * a.height, b.width * b.height)) {
                        (hitA.score * weightA < hitB.score * weightB ? hitA : hitB).score = -1.0f;
                    }
                }
            }
        }
    }
    for (Anchor& anchor : anchors_) {
        std::erase_if(anchor.hits, [](const Hit& hit) { return hit.score < 0.0f; });
    }
    placePieces(grey.width, grey.height);
    anchorsMs_ = msSince(anchorsStart);
    found_.insert(found_.end(), windows_.begin(), windows_.end());
    return keepOutermost(out, max);
}

std::string UiLayout::describeHits() const {
    std::string text;
    for (const Anchor& anchor : anchors_) {
        text += anchor.name + ":";
        for (const Hit& hit : anchor.hits) {
            char item[48];
            std::snprintf(item, sizeof(item), " %d %d %.2f", hit.x, hit.y, hit.score);
            text += item;
        }
        text += '\n';
    }
    return text;
}

int UiLayout::findFrames(const GreyLevel& grey, UiPiece* out, int max) {
    found_.clear();
    if (grey.pixels == nullptr || grey.width < 8 || grey.height < 8) {
        return 0;
    }
    findLines(grey);
    findWindows();
    found_ = windows_;
    return keepOutermost(out, max);
}

void UiLayout::findLines(const GreyLevel& grey) {
    levels_.clear();
    uprights_.clear();
    const int width = grey.width;
    const int height = grey.height;
    width_ = width;
    columns_.assign(static_cast<std::size_t>(width), Column{});
    const __m128i step = _mm_set1_epi8(static_cast<char>(kLineStep));
    const __m128i low = _mm_set1_epi8(static_cast<char>(kLineMin));
    const auto row = [&](int y) { return grey.pixels + grey.stride * y; };
    const auto isLine = [](int middle, int a, int b) {
        return middle >= kLineMin && middle >= a + kLineStep && middle >= b + kLineStep;
    };

    for (int y = 2; y + 2 < height; ++y) {
        const std::uint8_t* middle = row(y);
        const std::uint8_t* above = row(y - 2);
        const std::uint8_t* below = row(y + 2);
        // Level lines: brighter than two rows above and below.
        Column level;
        const auto keepLevel = [&](int from, int to) { levels_.push_back({y, from, to}); };
        int x = 0;
        for (; x + 16 <= width; x += 16) {
            const __m128i m = bytes(middle + x);
            const __m128i bright = _mm_and_si128(_mm_and_si128(atLeast(_mm_subs_epu8(m, bytes(above + x)), step),
                                                               atLeast(_mm_subs_epu8(m, bytes(below + x)), step)),
                                                 atLeast(m, low));
            for (unsigned bits = static_cast<unsigned>(_mm_movemask_epi8(bright)); bits != 0; bits &= bits - 1) {
                addPixel(level, x + static_cast<int>(std::countr_zero(bits)), keepLevel);
            }
        }
        for (; x < width; ++x) {
            if (isLine(middle[x], above[x], below[x])) {
                addPixel(level, x, keepLevel);
            }
        }
        finishLine(level, keepLevel);
        // Upright lines: brighter than two columns left and right, built column by column as the rows come.
        const auto addUpright = [&](int column) {
            addPixel(columns_[static_cast<std::size_t>(column)], y,
                     [&](int from, int to) { uprights_.push_back({column, from, to}); });
        };
        x = 2;
        for (; x + 18 <= width; x += 16) {
            const __m128i m = bytes(middle + x);
            const __m128i bright = _mm_and_si128(_mm_and_si128(atLeast(_mm_subs_epu8(m, bytes(middle + x - 2)), step),
                                                               atLeast(_mm_subs_epu8(m, bytes(middle + x + 2)), step)),
                                                 atLeast(m, low));
            for (unsigned bits = static_cast<unsigned>(_mm_movemask_epi8(bright)); bits != 0; bits &= bits - 1) {
                addUpright(x + static_cast<int>(std::countr_zero(bits)));
            }
        }
        for (; x + 2 < width; ++x) {
            if (isLine(middle[x], middle[x - 2], middle[x + 2])) {
                addUpright(x);
            }
        }
    }
    for (int x = 0; x < width; ++x) {
        finishLine(columns_[static_cast<std::size_t>(x)],
                   [&](int from, int to) { uprights_.push_back({x, from, to}); });
    }
    joinNeighbours(levels_, scratch_);
    joinNeighbours(uprights_, scratch_);
    std::sort(levels_.begin(), levels_.end(), [](const Line& a, const Line& b) { return a.at < b.at; });
}

int UiLayout::rowCover(int row, int x0, int x1) {
    spans_.clear();
    const auto first = std::lower_bound(levels_.begin(), levels_.end(), row - 1,
                                        [](const Line& line, int at) { return line.at < at; });
    for (auto it = first; it != levels_.end() && it->at <= row + 1; ++it) {
        const int from = std::max(it->from, x0);
        const int to = std::min(it->to, x1);
        if (from < to) {
            spans_.emplace_back(from, to);
        }
    }
    std::sort(spans_.begin(), spans_.end());
    int covered = 0;
    int reached = x0;
    for (const auto& [from, to] : spans_) {
        const int start = std::max(from, reached);
        if (to > start) {
            covered += to - start;
            reached = to;
        }
    }
    return covered;
}

bool UiLayout::levelLine(int lo, int hi, int x0, int x1, bool highest, int* row, int reach) {
    const auto first =
        std::lower_bound(levels_.begin(), levels_.end(), lo, [](const Line& line, int at) { return line.at < at; });
    const auto last =
        std::upper_bound(levels_.begin(), levels_.end(), hi, [](int at, const Line& line) { return at < line.at; });
    const auto check = [&](const Line& line, int& tried) {
        if (line.at == tried || line.to <= x0 || line.from >= x1) {
            return false;
        }
        tried = line.at;
        return rowCover(line.at, x0, x1) * 2 >= x1 - x0 &&
               (reach == kNone ||
                rowCover(line.at, std::max(0, reach - kReach), std::min(width_, reach + kReach + 1)) > 0);
    };
    int tried = INT_MIN;
    if (highest) {
        for (auto it = first; it != last; ++it) {
            if (check(*it, tried)) {
                *row = it->at;
                return true;
            }
        }
    } else {
        for (auto it = last; it != first;) {
            --it;
            if (check(*it, tried)) {
                *row = it->at;
                return true;
            }
        }
    }
    return false;
}

void UiLayout::findWindows() {
    windows_.clear();
    searchWindows(false);
    // A window partly under another is found first as a rectangle of its visible part, which is not all of it when
    // that part is no rectangle: the minimap with the Inventory over its lower left (the user's clip of 2026-11-12)
    // kept only its strip right of the Inventory, its top line broken by its buttons. Its lines end where the window
    // over it begins, so they are taken to go on under it and the windows are looked for once more.
    if (!windows_.empty() && lengthenUnderWindows()) {
        searchWindows(true);
    }
}

bool UiLayout::lengthenUnderWindows() {
    covers_ = windows_;
    seen_ = uprights_;
    bool lengthened = false;
    // A line on a row (or column) well inside the window's, ending at its near edge from outside, goes on to its far
    // edge. For an upright line, the window's columns are `rowsFrom` to `rowsTo` and its rows `spanFrom` to `spanTo`.
    const auto lengthen = [&](Line& line, int rowsFrom, int rowsTo, int spanFrom, int spanTo) {
        if (line.at <= rowsFrom + kReach || line.at >= rowsTo - kReach) {
            return;
        }
        if (std::abs(line.from - spanTo) <= kReach && line.to > spanTo + kReach) {
            line.from = std::min(line.from, spanFrom);
            lengthened = true;
        } else if (std::abs(line.to - spanFrom) <= kReach && line.from < spanFrom - kReach) {
            line.to = std::max(line.to, spanTo);
            lengthened = true;
        }
    };
    for (const UiPiece& over : covers_) {
        for (Line& level : levels_) {
            lengthen(level, over.y0, over.y1, over.x0, over.x1);
        }
        for (Line& upright : uprights_) {
            lengthen(upright, over.x0, over.x1, over.y0, over.y1);
        }
    }
    return lengthened;
}

void UiLayout::searchWindows(bool again) {
    // Looking again, a window is one only if at least one corner is under a window of the first look (it is partly
    // under another), each other corner is where its side's own line (as seen) ends, not along a longer one, and it
    // covers something those do not. Lines lengthened under a window that only touches another make no window over
    // the world below them, nor one over two found already; nor do the lines of two windows that only line up (with
    // five windows open, 2026-11-12, the Actions window's bottom and the minimap's left side lengthened under the
    // Skills window made one over the ground above Skills).
    const auto under = [&](int x, int y) {
        return std::any_of(covers_.begin(), covers_.end(), [&](const UiPiece& over) {
            return x > over.x0 + kReach && x < over.x1 - kReach && y > over.y0 + kReach && y < over.y1 - kReach;
        });
    };
    int hidden = 0; // Corners under a window of the first look.
    const auto corner = [&](int side, int x, int y, bool top) {
        if (side < 0) {
            return true; // A screen edge, which both its level lines reach.
        }
        const Line& own = seen_[static_cast<std::size_t>(side)];
        if (top ? own.from <= y + kTitle + kReach && own.from >= y - kReach
                : own.to >= y - kReach && own.to <= y + 1 + kReach) {
            return true;
        }
        if (under(x, y)) {
            ++hidden;
            return true;
        }
        return false;
    };
    const auto addsCover = [&](int x0, int y0, int x1, int y1) {
        for (int y = y0 + kGrid / 2; y < y1; y += kGrid) {
            for (int x = x0 + kGrid / 2; x < x1; x += kGrid) {
                if (std::none_of(covers_.begin(), covers_.end(), [&](const UiPiece& over) {
                        return x >= over.x0 && x < over.x1 && y >= over.y0 && y < over.y1;
                    })) {
                    return true;
                }
            }
        }
        return false;
    };
    const auto add = [&](int sideA, int sideB, int x0, int y0, int x1, int y1) {
        if (windows_.size() >= kMaxWindows) {
            return;
        }
        for (const UiPiece& known : windows_) {
            if (known.x0 == x0 && known.y0 == y0 && known.x1 == x1 && known.y1 == y1) {
                return;
            }
        }
        hidden = 0;
        if (again &&
            !(corner(sideA, x0, y0, true) && corner(sideA, x0, y1 - 1, false) && corner(sideB, x1 - 1, y0, true) &&
              corner(sideB, x1 - 1, y1 - 1, false) && hidden > 0 && addsCover(x0, y0, x1, y1))) {
            return;
        }
        UiPiece piece;
        setName(piece.name, "Window");
        piece.x0 = x0;
        piece.y0 = y0;
        piece.x1 = x1;
        piece.y1 = y1;
        windows_.push_back(piece);
    };
    for (const Line& level : levels_) {
        const int y = level.at;
        // The upright lines it meets; a level line reaching the screen's left or right edge also meets that edge,
        // as a window going off the screen has its side there (kLeftEdge, kRightEdge).
        crossing_.clear();
        for (std::size_t i = 0; i < uprights_.size(); ++i) {
            const Line& upright = uprights_[i];
            if (upright.at >= level.from - kReach && upright.at <= level.to + kReach && y >= upright.from - kReach &&
                y <= upright.to + kReach) {
                crossing_.push_back(static_cast<int>(i));
            }
        }
        std::sort(crossing_.begin(), crossing_.end(), [&](int a, int b) {
            return uprights_[static_cast<std::size_t>(a)].at < uprights_[static_cast<std::size_t>(b)].at;
        });
        // Not when a side's own line is that close to the edge: the side is there (else the Equipment panel at the
        // right edge was two windows, one to its side line 5 pixels from the edge and one to the edge).
        const bool left = level.from <= kReach &&
                          (crossing_.empty() || uprights_[static_cast<std::size_t>(crossing_.front())].at > 2 * kReach);
        const bool right =
            level.to >= width_ - 1 - kReach &&
            (crossing_.empty() || uprights_[static_cast<std::size_t>(crossing_.back())].at < width_ - 1 - 2 * kReach);
        if (left) {
            crossing_.insert(crossing_.begin(), kLeftEdge);
        }
        if (right) {
            crossing_.push_back(kRightEdge);
        }
        for (std::size_t i = 0; i < crossing_.size(); ++i) {
            for (std::size_t j = i + 1; j < crossing_.size(); ++j) {
                const bool edgeA = crossing_[i] < 0;
                const bool edgeB = crossing_[j] < 0;
                if (edgeA && edgeB) {
                    continue;
                }
                // A screen edge reaches as far as the other side does, and the window's other level line must reach
                // the edge too: the left edge with the chat's tab row (to 582, from 104) and the hotbar's end made
                // a window over the ground between them (the user's clip of 2026-11-12, the chat widened).
                Line a = edgeA ? Line{0, 0, 0} : uprights_[static_cast<std::size_t>(crossing_[i])];
                Line b = edgeB ? Line{width_ - 1, 0, 0} : uprights_[static_cast<std::size_t>(crossing_[j])];
                if (edgeA) {
                    a.from = b.from;
                    a.to = b.to;
                }
                if (edgeB) {
                    b.from = a.from;
                    b.to = a.to;
                }
                const int reach = edgeA ? 0 : edgeB ? width_ - 1 : kNone;
                if (b.at - a.at < kSide) {
                    continue;
                }
                // The level line is the bottom: the sides go up, to a top line where they start (the title bar's
                // may be a little higher), or are long enough without one (two real sides only). Where both start:
                // a top line near only one side's start, far above the other's, is another window's (the
                // Inventory's bottom line made the hotbar's top, its left side starting there and its right one 280
                // pixels lower, over the ground between them: the user's clip 2026-11-12_14-21-53).
                if (a.from <= y - kSide && b.from <= y - kSide) {
                    int top = 0;
                    const int start = std::max(a.from, b.from);
                    bool found = levelLine(start - kTitle, start + kReach, a.at, b.at, true, &top, reach);
                    if (!found && !edgeA && !edgeB && y - start >= kLongSides) {
                        top = start;
                        found = true;
                    }
                    if (found && y - top >= kSide) {
                        add(crossing_[i], crossing_[j], a.at, top, b.at + 1, y + 1);
                    }
                }
                // The level line is the top: the sides go down to a bottom line where they end.
                if (a.to >= y + kSide && b.to >= y + kSide) {
                    int bottom = 0;
                    if (levelLine(std::min(a.to, b.to) - kReach, std::max(a.to, b.to) + kReach, a.at, b.at, false,
                                  &bottom, reach) &&
                        bottom - y >= kSide) {
                        int top = y;
                        levelLine(y - kTitle, y - 3, a.at, b.at, true, &top, reach);
                        add(crossing_[i], crossing_[j], a.at, top, b.at + 1, bottom + 1);
                    }
                }
            }
        }
    }
}

void UiLayout::findAnchor(const GreyLevel& grey, Anchor& anchor) {
    anchor.hits.clear();
    candidates_.clear();
    const int lastX = grey.width - anchor.width;
    const int lastY = grey.height - anchor.height;
    if (lastX < 0 || lastY < 0) {
        return;
    }
    const std::ptrdiff_t stride = grey.stride;
    const double count = static_cast<double>(anchor.pixels.size());
    const auto score = [&](int x, int y) {
        const std::uint8_t* base = grey.pixels + stride * y + x;
        int sum = 0;
        std::int64_t squares = 0;
        float cross = 0.0f;
        for (const Pixel& pixel : anchor.pixels) {
            const int value = base[stride * pixel.dy + pixel.dx];
            sum += value;
            squares += value * value;
            cross += static_cast<float>(value) * pixel.centred;
        }
        const double spread = static_cast<double>(squares) - static_cast<double>(sum) * sum / count;
        if (spread < count) { // The image is flat there: no picture to match.
            return;
        }
        const float correlation = static_cast<float>(cross / std::sqrt(spread * anchor.spread));
        if (correlation >= anchor.minScore && candidates_.size() < kMaxCandidates) {
            candidates_.push_back({x, y, correlation});
        }
    };

    const int pairs = static_cast<int>(anchor.pairs.size());
    std::array<const std::uint8_t*, kPairsMax> brights{};
    std::array<const std::uint8_t*, kPairsMax> darks{};
    std::array<__m128i, kPairsMax> margins{};
    for (int k = 0; k < pairs; ++k) {
        margins[static_cast<std::size_t>(k)] =
            _mm_set1_epi8(static_cast<char>(anchor.pairs[static_cast<std::size_t>(k)].margin));
    }
    for (int y = 0; y <= lastY; ++y) {
        for (int k = 0; k < pairs; ++k) {
            const Pair& pair = anchor.pairs[static_cast<std::size_t>(k)];
            brights[static_cast<std::size_t>(k)] = grey.pixels + stride * (y + pair.brightDy) + pair.brightDx;
            darks[static_cast<std::size_t>(k)] = grey.pixels + stride * (y + pair.darkDy) + pair.darkDx;
        }
        int x = 0;
        for (; x + 15 <= lastX; x += 16) {
            __m128i pass = _mm_set1_epi8(-1);
            unsigned bits = 0xFFFF;
            for (int k = 0; k < pairs; ++k) {
                const std::size_t p = static_cast<std::size_t>(k);
                pass =
                    _mm_and_si128(pass, atLeast(_mm_subs_epu8(bytes(brights[p] + x), bytes(darks[p] + x)), margins[p]));
                bits = static_cast<unsigned>(_mm_movemask_epi8(pass));
                if (bits == 0) {
                    break;
                }
            }
            for (; bits != 0; bits &= bits - 1) {
                score(x + static_cast<int>(std::countr_zero(bits)), y);
            }
        }
        for (; x <= lastX; ++x) {
            bool pass = true;
            for (int k = 0; k < pairs && pass; ++k) {
                const std::size_t p = static_cast<std::size_t>(k);
                pass = brights[p][x] >= darks[p][x] + anchor.pairs[p].margin;
            }
            if (pass) {
                score(x, y);
            }
        }
    }
    // The best of each place: no two hits overlap.
    std::sort(candidates_.begin(), candidates_.end(), [](const Hit& a, const Hit& b) { return a.score > b.score; });
    for (const Hit& candidate : candidates_) {
        const bool apart = std::all_of(anchor.hits.begin(), anchor.hits.end(), [&](const Hit& hit) {
            return std::abs(hit.x - candidate.x) >= anchor.width || std::abs(hit.y - candidate.y) >= anchor.height;
        });
        if (apart) {
            anchor.hits.push_back(candidate);
            if (anchor.hits.size() >= kMaxHits) {
                break;
            }
        }
    }
}

void UiLayout::placePieces(int width, int height) {
    for (const Piece& piece : pieces_) {
        const Anchor& main = anchors_[static_cast<std::size_t>(piece.anchor)];
        for (const Hit& hit : main.hits) {
            // An anchor well inside a framed window is the window's own (the inventory's scroll arrows look like the
            // chat's): the piece is not there.
            const bool inWindow = std::any_of(windows_.begin(), windows_.end(), [&](const UiPiece& window) {
                return hit.x > window.x0 + kInside && hit.y > window.y0 + kInside &&
                       hit.x + main.width < window.x1 - kInside && hit.y + main.height < window.y1 - kInside;
            });
            if (inWindow) {
                continue;
            }
            const int dx = hit.x - piece.x;
            const int dy = hit.y - piece.y;
            UiPiece placed;
            placed.name = piece.name;
            placed.x0 = piece.x0 + dx;
            placed.y0 = piece.y0 + dy;
            placed.x1 = piece.x1 + dx;
            placed.y1 = piece.y1 + dy;
            // Each side's lines are tried in order and the first found sets it. With none found, the side is under a
            // framed window if one begins across the way it was looked for (the user's screenshot of 2026-11-12: a
            // window over the resized chat's corner, though not over where the corner was measured), at most the
            // piece's measured size past its measured side: it then ends where that window begins, the piece's
            // visible part.
            std::array<bool, 4> tried{};
            std::array<bool, 4> set{};
            std::array<bool, 4> optional{};
            std::array<int, 4> under{}; // Where the nearest window across the way begins; kNone: none.
            under.fill(kNone);
            const auto windowAcross = [&](const Stretch& stretch, int alongFrom, int alongTo, int limit) {
                const bool level = stretch.side == Side::Left || stretch.side == Side::Right;
                const bool forward = stretch.side == Side::Right || stretch.side == Side::Bottom;
                const int size = level ? piece.x1 - piece.x0 : piece.y1 - piece.y0;
                const int from =
                    forward ? (level ? placed.x0 : placed.y0) + kSide : (level ? placed.x1 : placed.y1) - kSide;
                const int farthest = forward ? (level ? piece.x1 + dx : piece.y1 + dy) + size
                                             : (level ? piece.x0 + dx : piece.y0 + dy) - size;
                int nearest = kNone;
                for (const UiPiece& window : windows_) {
                    if ((level ? window.y0 : window.x0) >= alongTo || (level ? window.y1 : window.x1) <= alongFrom) {
                        continue;
                    }
                    const int edge = forward ? (level ? window.x0 : window.y0) : (level ? window.x1 : window.y1);
                    const bool way = forward ? edge >= from && edge < limit && edge <= farthest
                                             : edge <= from && edge > limit && edge >= farthest;
                    if (way && (nearest == kNone || (forward ? edge < nearest : edge > nearest))) {
                        nearest = edge;
                    }
                }
                return nearest;
            };
            const auto keepNearest = [](int& kept, int edge, bool forward) {
                if (edge != kNone && (kept == kNone || (forward ? edge < kept : edge > kept))) {
                    kept = edge;
                }
            };
            for (const Stretch& stretch : piece.stretches) {
                const auto s = static_cast<std::size_t>(stretch.side);
                if (set[s]) {
                    continue;
                }
                tried[s] = true;
                optional[s] = optional[s] || stretch.optional;
                const bool level = stretch.side == Side::Left || stretch.side == Side::Right;
                const bool forward = stretch.side == Side::Right || stretch.side == Side::Bottom;
                if (stretch.line) {
                    // The nearest frame line that way, at least a window's side from the other side, along the
                    // piece for at least a window's side.
                    const std::vector<Line>& lines = level ? uprights_ : levels_;
                    const int from =
                        forward ? (level ? placed.x0 : placed.y0) + kSide : (level ? placed.x1 : placed.y1) - kSide;
                    const int alongFrom = level ? placed.y0 : placed.x0;
                    const int alongTo = level ? placed.y1 : placed.x1;
                    const Line* best = nullptr;
                    for (const Line& line : lines) {
                        if (std::min(line.to, alongTo) - std::max(line.from, alongFrom) < kSide ||
                            (forward ? line.at < from : line.at > from)) {
                            continue;
                        }
                        if (best == nullptr || (forward ? line.at < best->at : line.at > best->at)) {
                            best = &line;
                        }
                    }
                    if (best == nullptr) {
                        keepNearest(under[s], windowAcross(stretch, alongFrom, alongTo, forward ? INT_MAX : INT_MIN),
                                    forward);
                    } else if (level) {
                        (forward ? placed.x1 : placed.x0) = forward ? best->at + 1 : best->at;
                        set[s] = true;
                    } else {
                        (forward ? placed.y1 : placed.y0) = forward ? best->at + 1 : best->at;
                        set[s] = true;
                    }
                    continue;
                }
                // How far the search goes: to the next of the piece's own anchor that way (the next window).
                int limit = forward ? INT_MAX : INT_MIN;
                for (const Hit& other : main.hits) {
                    const bool inLine =
                        level ? std::abs(other.y - hit.y) <= kSameRow : std::abs(other.x - hit.x) <= kSameRow;
                    const int along = level ? other.x : other.y;
                    const int from = level ? hit.x : hit.y;
                    if (inLine && (forward ? along > from && along < limit : along < from && along > limit)) {
                        limit = along;
                    }
                }
                const int from = level ? hit.x : hit.y;
                // Where the side's anchor is across the way: as far from the piece's nearer edge as when measured
                // (the resized chat's bottom-right corner stays with its bottom, its top-right one with its top).
                const auto across = [](int measured, int low, int high, int placedLow, int placedHigh) {
                    return measured - low <= high - measured ? placedLow + measured - low
                                                             : placedHigh + measured - high;
                };
                const int expected = level ? across(stretch.y, piece.y0, piece.y1, placed.y0, placed.y1)
                                           : across(stretch.x, piece.x0, piece.x1, placed.x0, placed.x1);
                const Hit* best = nullptr;
                for (const Hit& side : anchors_[static_cast<std::size_t>(stretch.anchor)].hits) {
                    const bool inLine = std::abs((level ? side.y : side.x) - expected) <= kSameRow;
                    const int along = level ? side.x : side.y;
                    if (!inLine || (forward ? along <= from || along >= limit : along >= from || along <= limit)) {
                        continue;
                    }
                    const int bestAlong = best == nullptr ? 0 : level ? best->x : best->y;
                    if (best == nullptr || (forward ? along < bestAlong : along > bestAlong)) {
                        best = &side;
                    }
                }
                if (best == nullptr) {
                    // The way it was looked for: the side anchor's rows (or columns).
                    const Anchor& sideAnchor = anchors_[static_cast<std::size_t>(stretch.anchor)];
                    const int alongTo = expected + (level ? sideAnchor.height : sideAnchor.width);
                    keepNearest(under[s], windowAcross(stretch, expected, alongTo, limit), forward);
                    continue;
                }
                switch (stretch.side) {
                case Side::Left:
                    placed.x0 = piece.x0 + best->x - stretch.x;
                    break;
                case Side::Right:
                    placed.x1 = piece.x1 + best->x - stretch.x;
                    break;
                case Side::Top:
                    placed.y0 = piece.y0 + best->y - stretch.y;
                    break;
                case Side::Bottom:
                    placed.y1 = piece.y1 + best->y - stretch.y;
                    break;
                }
                set[s] = true;
            }
            bool whole = true;
            for (std::size_t s = 0; s < 4; ++s) {
                if (!tried[s] || set[s]) {
                    continue;
                }
                if (under[s] == kNone) {
                    whole = whole && optional[s]; // Missing: as measured if it may be.
                    continue;
                }
                switch (static_cast<Side>(s)) {
                case Side::Left:
                    placed.x0 = under[s];
                    break;
                case Side::Right:
                    placed.x1 = under[s];
                    break;
                case Side::Top:
                    placed.y0 = under[s];
                    break;
                case Side::Bottom:
                    placed.y1 = under[s];
                    break;
                }
            }
            placed.x0 = std::clamp(placed.x0, 0, width);
            placed.x1 = std::clamp(placed.x1, 0, width);
            placed.y0 = std::clamp(placed.y0, 0, height);
            placed.y1 = std::clamp(placed.y1, 0, height);
            if (whole && placed.x0 < placed.x1 && placed.y0 < placed.y1) {
                found_.push_back(placed);
            }
        }
    }
}

int UiLayout::keepOutermost(UiPiece* out, int max) {
    const auto area = [](const UiPiece& piece) {
        return static_cast<std::int64_t>(piece.x1 - piece.x0) * (piece.y1 - piece.y0);
    };
    std::stable_sort(found_.begin(), found_.end(),
                     [&](const UiPiece& a, const UiPiece& b) { return area(a) > area(b); });
    int count = 0;
    for (const UiPiece& piece : found_) {
        if (count >= max) {
            break;
        }
        if (std::none_of(out, out + count, [&](const UiPiece& kept) { return inside(piece, kept); })) {
            out[count++] = piece;
        }
    }
    return count;
}

} // namespace observex
