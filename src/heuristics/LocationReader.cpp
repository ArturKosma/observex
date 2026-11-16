#include "heuristics/LocationReader.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace observex {

namespace {

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

bool LocationReader::answer(MessageLines& lines, std::size_t at, TextOcr& textOcr, int& x, int& y, int& z,
                            std::string& text, std::string& why) {
    const std::vector<MessageLines::Line>& found = lines.lines();
    const MessageLines::Line line = found[at];
    // The line alone first (most new lines are other messages).
    const std::string said = lines.read(line.top, line.bottom, line.right, false, 3, true, textOcr, &error_);
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
        r.text = bottom == line.bottom && scale == 3 && smooth
                     ? said
                     : lines.read(line.top, bottom, right, false, scale, smooth, textOcr, &error_);
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

void LocationReader::read(MessageLines& lines, const MinimapRead& map, TextOcr& ocr, LocationRead& out,
                          std::string& why) {
    const std::size_t count = lines.lines().size();
    const std::size_t newest = count > static_cast<std::size_t>(kNewest) ? count - kNewest : 0;
    for (std::size_t i = count; i-- > newest;) {
        const MessageLines::Line& line = lines.lines()[i];
        if (!line.white || !line.fresh) {
            continue;
        }
        int x = 0;
        int y = 0;
        int z = 0;
        std::string text;
        if (!answer(lines, i, ocr, x, y, z, text, why)) {
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

} // namespace observex
