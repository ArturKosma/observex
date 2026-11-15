#include "heuristics/LocationReader.h"

#include <algorithm>
#include <bit>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

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

// A minus as OCR may write it, at `text`[i] (a dash, '~', or UTF-8's en dash, em dash or minus sign); its length in
// bytes, 0 if none.
std::size_t minusAt(std::string_view text, std::size_t i) {
    if (text[i] == '-' || text[i] == '~') {
        return 1;
    }
    if (i + 2 < text.size() && static_cast<unsigned char>(text[i]) == 0xE2 &&
        ((static_cast<unsigned char>(text[i + 1]) == 0x80 &&
          (static_cast<unsigned char>(text[i + 2]) == 0x93 || static_cast<unsigned char>(text[i + 2]) == 0x94)) ||
         (static_cast<unsigned char>(text[i + 1]) == 0x88 && static_cast<unsigned char>(text[i + 2]) == 0x92))) {
        return 3;
    }
    return 0;
}

bool digitAt(std::string_view text, std::size_t i) {
    return i < text.size() && std::isdigit(static_cast<unsigned char>(text[i])) != 0;
}

std::string lowered(std::string_view text) {
    std::string out(text);
    for (char& c : out) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return out;
}

// Whether OCR's text says "location" (a letter or so read wrong: "Iocation", "locatlon").
bool saysLocation(std::string_view text) {
    const std::string lower = lowered(text);
    return lower.find("ocation") != std::string::npos || lower.find("locat") != std::string::npos ||
           lower.find("cation:") != std::string::npos;
}

} // namespace

StatsArea LocationReader::area(const UiPiece& piece, int width, int height) {
    const int bottom = std::min(piece.y1 - kBottom, height);
    return {std::max(piece.x0, 0), std::max({piece.y0, bottom - kRows, 0}), std::min(piece.x1, width), bottom};
}

bool LocationReader::parse(std::string_view text, int& x, int& y, int& z) {
    const std::string lower = lowered(text);
    std::size_t at = lower.find("ocation");
    if (at == std::string::npos) {
        at = lower.find("locat");
    }
    if (at == std::string::npos) {
        at = lower.find("cation:");
    }
    if (at == std::string::npos) {
        return false;
    }
    const std::size_t colon = text.find(':', at);
    std::size_t i = colon != std::string_view::npos ? colon + 1 : at + 5;
    // Every number after the colon: digits, thousands after ',' or '.' (and a space OCR may put after it).
    std::vector<long long> numbers;
    std::vector<int> lengths;
    while (i < text.size() && numbers.size() < 8) {
        bool negative = false;
        if (const std::size_t minus = minusAt(text, i); minus > 0) {
            std::size_t j = i + minus;
            if (j < text.size() && text[j] == ' ') {
                ++j;
            }
            if (digitAt(text, j)) {
                negative = true;
                i = j;
            }
        }
        if (!digitAt(text, i)) {
            ++i;
            continue;
        }
        long long value = 0;
        int digits = 0;
        while (i < text.size()) {
            if (digitAt(text, i)) {
                value = value * 10 + (text[i] - '0');
                ++digits;
                ++i;
            } else if ((text[i] == ',' || text[i] == '.') &&
                       (digitAt(text, i + 1) || (i + 2 < text.size() && text[i + 1] == ' ' && digitAt(text, i + 2) &&
                                                 digitAt(text, i + 3) && digitAt(text, i + 4)))) {
                i += text[i + 1] == ' ' ? 2 : 1;
            } else {
                break;
            }
            if (digits > 7) {
                return false;
            }
        }
        numbers.push_back(negative ? -value : value);
        lengths.push_back(digits);
    }
    // A '/' between them read as a lone digit (1, 7) is no number.
    if (numbers.size() > 3) {
        std::vector<long long> kept;
        for (std::size_t k = 0; k < numbers.size(); ++k) {
            if (lengths[k] != 1) {
                kept.push_back(numbers[k]);
            }
        }
        if (kept.size() >= 3) {
            numbers = kept;
        }
    }
    if (numbers.size() < 3) {
        return false;
    }
    // In Lineage 2's world (its regions span about -330,000 to 230,000 east and -260,000 to 300,000 south; heights
    // within +-20,000).
    if (std::llabs(numbers[0]) > 400000 || std::llabs(numbers[1]) > 400000 || std::llabs(numbers[2]) > 32000) {
        return false;
    }
    x = static_cast<int>(numbers[0]);
    y = static_cast<int>(numbers[1]);
    z = static_cast<int>(numbers[2]);
    return true;
}

void LocationReader::lines(const ColourPart& part, std::vector<Line>& out) {
    out.clear();
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
            out.push_back(line);
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

std::string LocationReader::ocr(const ColourPart& part, int top, int bottom, int right, int scale, bool smooth,
                                TextOcr& textOcr, std::string* error) {
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
    // Dark letters on white paper, as Windows' OCR reads best: each pixel's darkest channel (a white letter's is light,
    // the window's ground dark), from 80 (paper) to 200 (ink).
    const auto lightness = [&](int x, int y) {
        const std::uint8_t* p = part.bgra + static_cast<std::size_t>(y0 + y) * part.pitch +
                                static_cast<std::size_t>(x0 + x) * 4;
        return static_cast<float>(std::min({p[0], p[1], p[2]}));
    };
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
            const float ink = std::clamp((v - 80.0f) * 255.0f / 120.0f, 0.0f, 255.0f);
            picture_[static_cast<std::size_t>((y + kMargin) * w + x + kMargin)] = static_cast<std::uint8_t>(255.0f - ink);
        }
    }
    return textOcr.read(picture_.data(), w, h, w, error);
}

bool LocationReader::answer(const ColourPart& part, const std::vector<Line>& found, std::size_t at, TextOcr& textOcr,
                            int& x, int& y, int& z, std::string& text, std::string& why) {
    const Line& line = found[at];
    // The line alone first (most new lines are other messages).
    const std::string said = ocr(part, line.top, line.bottom, line.right, 3, true, textOcr, &error_);
    if (!saysLocation(said)) {
        return false;
    }
    // With the line under it, if white and right under it (the numbers may go on there).
    int bottom = line.bottom;
    int right = line.right;
    if (at + 1 < found.size() && found[at + 1].white && found[at + 1].top - line.bottom < 8) {
        bottom = found[at + 1].bottom;
        right = std::max(right, found[at + 1].right);
    }
    struct Reading {
        std::string text;
        bool parsed = false;
        int x = 0;
        int y = 0;
        int z = 0;
    };
    Reading readings[3];
    const auto readAs = [&](Reading& r, int scale, bool smooth) {
        r.text = bottom == line.bottom && scale == 3 && smooth ? said
                                                                : ocr(part, line.top, bottom, right, scale, smooth,
                                                                      textOcr, &error_);
        r.parsed = parse(r.text, r.x, r.y, r.z);
    };
    const auto same = [](const Reading& a, const Reading& b) {
        return a.parsed && b.parsed && a.x == b.x && a.y == b.y && a.z == b.z;
    };
    readAs(readings[0], 3, true);
    readAs(readings[1], 4, false);
    const Reading* taken = nullptr;
    if (same(readings[0], readings[1])) {
        taken = &readings[0];
    } else {
        readAs(readings[2], 4, true);
        if (same(readings[0], readings[2]) || same(readings[1], readings[2])) {
            taken = &readings[2];
        }
    }
    if (taken == nullptr) {
        why += std::string(why.empty() ? "" : " | ") + "Location: read '" + readings[0].text + "', '" +
               readings[1].text + "', '" + readings[2].text + "': they disagree";
        return false;
    }
    x = taken->x;
    y = taken->y;
    z = taken->z;
    text = readings[0].text;
    return true;
}

void LocationReader::read(const UiPiece* piece, const ColourPart* part, const MinimapRead& map, TextOcr& ocr,
                          LocationRead& out, std::string& why) {
    if (piece == nullptr || part == nullptr) {
        return; // Not seen: what was there before stays the image before's.
    }
    lines(*part, found_);
    if (first_) {
        first_ = false;
    } else {
        const std::size_t newest = found_.size() > static_cast<std::size_t>(kNewest) ? found_.size() - kNewest : 0;
        for (std::size_t i = found_.size(); i-- > newest;) {
            const Line& line = found_[i];
            if (!line.white || seenBefore(line)) {
                continue;
            }
            int x = 0;
            int y = 0;
            int z = 0;
            std::string text;
            if (!answer(*part, found_, i, ocr, x, y, z, text, why)) {
                continue;
            }
            // The same answer again (the window scrolled back to it, or /loc typed again where it was): nothing new.
            if (out.count != 0 && x == out.x && y == out.y && z == out.z) {
                break;
            }
            // The first from a clock, so a new reader (the stats DLL reloaded) never repeats an old reader's count.
            out.count = out.count != 0 ? out.count + 1
                                       : static_cast<std::uint32_t>(
                                             std::chrono::steady_clock::now().time_since_epoch().count()) |
                                             1u;
            out.x = x;
            out.y = y;
            out.z = z;
            out.map = map;
            std::snprintf(out.text.data(), out.text.size(), "%s", text.c_str());
            char note[160];
            std::snprintf(note, sizeof(note), "Location: read %d, %d, %d", x, y, z);
            if (!why.empty()) {
                why += " | ";
            }
            why += note;
            break;
        }
    }
    before_.swap(found_);
    beforeRows_.swap(rows_);
    beforeWords_ = words_;
}

bool LocationReader::seenBefore(const Line& line) const {
    const int rows = line.bottom - line.top;
    const int words = std::min(words_, beforeWords_);
    for (const Line& old : before_) {
        const int oldRows = old.bottom - old.top;
        if (std::abs(oldRows - rows) > 2) {
            continue;
        }
        int shared = 0;
        int either = 0;
        for (int r = 0; r < std::min(rows, oldRows); ++r) {
            const std::uint64_t* a = rows_.data() + static_cast<std::size_t>(line.top + r) * static_cast<std::size_t>(words_);
            const std::uint64_t* b =
                beforeRows_.data() + static_cast<std::size_t>(old.top + r) * static_cast<std::size_t>(beforeWords_);
            for (int k = 0; k < words; ++k) {
                shared += std::popcount(a[k] & b[k]);
                either += std::popcount(a[k] | b[k]);
            }
        }
        if (either > 0 && static_cast<float>(shared) >= kSame * static_cast<float>(either)) {
            return true;
        }
    }
    return false;
}

} // namespace observex
