#include "heuristics/MessageLines.h"

#include <algorithm>
#include <bit>
#include <cstdlib>

namespace observex {

namespace {

// A message's letters (anti-aliased, on the window's dark see-through ground): white, the system's (the dimmest
// letter pixels of a white line are 160 or so in every channel, its middle 220), or yellow (items, adena).
bool whiteAt(const std::uint8_t* p) {
    const int lo = std::min({p[0], p[1], p[2]});
    const int hi = std::max({p[0], p[1], p[2]});
    return lo >= 150 && hi - lo <= 70;
}
bool yellowAt(const std::uint8_t* p) {
    return p[2] >= 150 && p[1] >= 150 && p[0] <= 130;
}

} // namespace

StatsArea MessageLines::area(const UiPiece& piece, int width, int height) {
    const int bottom = std::min(piece.y1 - kBottom, height);
    return {std::max(piece.x0, 0), std::max({piece.y0, bottom - kRows, 0}), std::min(piece.x1, width), bottom};
}

void MessageLines::look(const ColourPart* part) {
    if (seen_) {
        before_.swap(found_);
        beforeRows_.swap(rows_);
        beforeWords_ = words_;
        haveBefore_ = true;
    }
    found_.clear();
    seen_ = part != nullptr;
    if (!seen_) {
        part_ = {};
        return;
    }
    part_ = *part;
    find(part_);
    if (haveBefore_) {
        markFresh();
    } else {
        for (Line& line : found_) {
            line.fresh = firstFresh_;
        }
    }
}

void MessageLines::find(const ColourPart& part) {
    const int width = part.area.x1 - part.area.x0;
    const int height = part.area.y1 - part.area.y0;
    Line line;
    bool inside = false;
    int blank = 0;
    int white = 0;
    int yellow = 0;
    const auto close = [&]() {
        if (inside && line.top > 0) { // One touching the part's top is cut: left out.
            line.white = white >= 2 * yellow && white > 0;
            found_.push_back(line);
        }
        inside = false;
    };
    words_ = std::max(width - kLeft, 0) / 64 + 1;
    rows_.assign(static_cast<std::size_t>(std::max(height, 0)) * static_cast<std::size_t>(words_), 0);
    for (int y = 0; y < height; ++y) {
        const std::uint8_t* row = part.bgra + static_cast<std::size_t>(y) * part.pitch;
        std::uint64_t* bits = rows_.data() + static_cast<std::size_t>(y) * static_cast<std::size_t>(words_);
        int count = 0;
        int last = -1;
        int rowWhite = 0;
        int rowYellow = 0;
        for (int x = kLeft; x < width; ++x) {
            const std::uint8_t* p = row + static_cast<std::size_t>(x) * 4;
            const bool w = whiteAt(p);
            const bool t = w || yellowAt(p);
            if (t) {
                bits[(x - kLeft) / 64] |= 1ull << ((x - kLeft) % 64);
                ++count;
                last = x;
                rowWhite += w ? 1 : 0;
                rowYellow += w ? 0 : 1;
            }
        }
        if (count >= 2) {
            if (!inside) {
                line = Line{};
                line.top = y;
                line.right = 0;
                white = yellow = 0;
                inside = true;
            }
            line.bottom = y + 1;
            line.right = std::max(line.right, last + 1);
            white += rowWhite;
            yellow += rowYellow;
            blank = 0;
        } else if (inside) {
            // A blank row inside a line (a word with no letter reaching it) does not end it; two do.
            if (++blank >= 2) {
                close();
            }
        }
    }
    close();
}

float MessageLines::alike(const Line& a, const Line& b) const {
    const int rows = std::min(a.bottom - a.top, b.bottom - b.top);
    const int words = std::min(words_, beforeWords_);
    int shared = 0;
    int either = 0;
    for (int r = 0; r < rows; ++r) {
        const std::uint64_t* p = rows_.data() + static_cast<std::size_t>(a.top + r) * static_cast<std::size_t>(words_);
        const std::uint64_t* q =
            beforeRows_.data() + static_cast<std::size_t>(b.top + r) * static_cast<std::size_t>(beforeWords_);
        for (int k = 0; k < words; ++k) {
            shared += std::popcount(p[k] & q[k]);
            either += std::popcount(p[k] | q[k]);
        }
    }
    return either > 0 ? static_cast<float>(shared) / static_cast<float>(either) : 0.0f;
}

void MessageLines::markFresh() {
    for (Line& line : found_) {
        line.fresh = false;
    }
    // An empty window before (or one whose letters were not found): none is new, as an old message must not be read
    // again.
    if (found_.empty() || before_.empty()) {
        return;
    }
    const auto heightOf = [](const Line& line) { return line.bottom - line.top; };
    const auto lineAt = [&](int top, int height) -> const Line* {
        for (const Line& b : before_) {
            if (std::abs(b.top - top) <= 2 && std::abs(heightOf(b) - height) <= 2) {
                return &b;
            }
        }
        return nullptr;
    };
    // The shifts to try: none, and every one that puts a line now where one of the same height was.
    shifts_.assign(1, 0);
    for (const Line& a : found_) {
        for (const Line& b : before_) {
            if (b.top > a.top && std::abs(heightOf(a) - heightOf(b)) <= 2) {
                shifts_.push_back(b.top - a.top);
            }
        }
    }
    std::sort(shifts_.begin(), shifts_.end());
    shifts_.erase(std::unique(shifts_.begin(), shifts_.end()), shifts_.end());
    const int firstTop = before_.front().top;
    const int lastTop = before_.back().top;
    for (const int shift : shifts_) {
        bool fits = true;
        int same = 0;
        for (const Line& a : found_) {
            const int at = a.top + shift; // Where it was on the image before.
            if (const Line* b = lineAt(at, heightOf(a)); b != nullptr) {
                if (alike(a, *b) >= kSame) {
                    ++same;
                    continue;
                }
                fits = false;
                break;
            }
            if (at > lastTop + 2 || at < firstTop - 2) {
                continue; // Below the lines before (new), or above them (cut off then).
            }
            fits = false; // Among them, where none was.
            break;
        }
        if (fits && same > 0) {
            for (Line& a : found_) {
                const int at = a.top + shift;
                a.fresh = at > lastTop + 2 && lineAt(at, heightOf(a)) == nullptr;
            }
            return;
        }
    }
    // No shift fits: new where no line before has its letters.
    for (Line& a : found_) {
        a.fresh = std::none_of(before_.begin(), before_.end(), [&](const Line& b) {
            return std::abs(heightOf(a) - heightOf(b)) <= 2 && alike(a, b) >= kSame;
        });
    }
}

std::string MessageLines::read(int top, int bottom, int right, bool yellow, int scale, bool smooth, TextOcr& textOcr,
                               std::string* error) {
    if (!seen_) {
        return {};
    }
    const ColourPart& part = part_;
    const int width = part.area.x1 - part.area.x0;
    const int height = part.area.y1 - part.area.y0;
    const int x0 = std::max(kLeft - 2, 0);
    const int x1 = std::min(right + 3, width);
    const int y0 = std::max(top - 2, 0);
    const int y1 = std::min(bottom + 2, height);
    const int cw = x1 - x0;
    const int ch = y1 - y0;
    if (cw <= 0 || ch <= 0) {
        return {};
    }
    // Dark letters on white paper, as Windows' OCR reads best. White letters: each pixel's darkest channel (a letter's
    // is light, the window's ground dark), from 80 (paper) to 200 (ink). Yellow letters: how much yellower than blue
    // ((red + green) / 2 - blue: grass and the dark ground behind the see-through window little, a letter's about 210),
    // from 70 (paper) to 230 (ink). Their darkest channel (red or green) read "Imp Shackles" as "Imp Shaddes" and "Magic
    // Ring" as "Magic Ping" on the user's screenshots (2026-11-16); this read all 200 pickups on them right, and so did
    // 60..80 to 220..240.
    const auto lightness = [&](int x, int y) {
        const std::uint8_t* p = part.bgra + static_cast<std::size_t>(y0 + y) * part.pitch +
                                static_cast<std::size_t>(x0 + x) * 4;
        if (yellow) {
            return (static_cast<float>(p[1]) + static_cast<float>(p[2])) * 0.5f - static_cast<float>(p[0]);
        }
        return static_cast<float>(std::min({p[0], p[1], p[2]}));
    };
    const float paper = yellow ? 70.0f : 80.0f;
    const float inkAt = yellow ? 230.0f : 200.0f;
    constexpr int kMargin = 12;
    const int w = cw * scale + 2 * kMargin;
    const int h = ch * scale + 2 * kMargin;
    picture_.assign(static_cast<std::size_t>(w) * static_cast<std::size_t>(h), 255);
    for (int y = 0; y < ch * scale; ++y) {
        for (int x = 0; x < cw * scale; ++x) {
            float v = 0.0f;
            if (smooth) {
                const float u = std::clamp((static_cast<float>(x) + 0.5f) / static_cast<float>(scale) - 0.5f, 0.0f,
                                           static_cast<float>(cw - 1));
                const float s = std::clamp((static_cast<float>(y) + 0.5f) / static_cast<float>(scale) - 0.5f, 0.0f,
                                           static_cast<float>(ch - 1));
                const int u0 = static_cast<int>(u);
                const int s0 = static_cast<int>(s);
                const int u1 = std::min(u0 + 1, cw - 1);
                const int s1 = std::min(s0 + 1, ch - 1);
                const float fu = u - static_cast<float>(u0);
                const float fs = s - static_cast<float>(s0);
                v = (lightness(u0, s0) * (1.0f - fu) + lightness(u1, s0) * fu) * (1.0f - fs) +
                    (lightness(u0, s1) * (1.0f - fu) + lightness(u1, s1) * fu) * fs;
            } else {
                v = lightness(x / scale, y / scale);
            }
            const float ink = std::clamp((v - paper) * 255.0f / (inkAt - paper), 0.0f, 255.0f);
            picture_[static_cast<std::size_t>((y + kMargin) * w + x + kMargin)] = static_cast<std::uint8_t>(255.0f - ink);
        }
    }
    return textOcr.read(picture_.data(), w, h, w, error);
}

} // namespace observex
