#include "heuristics/KeyLabels.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <fstream>
#include <sstream>
#include <system_error>

namespace observex {

namespace {

// Measured on the hotbar's labels at the 42-pixel UI scale (2026-11-15: Wind Strike's F2, Sit's F12, clip
// 2026-11-14_14-50-29's F1, F2, F4 and F12 on 241 frames each, all read right once '1' over a white sword and '4' were
// taught; scratch prototype).
constexpr int kLight = 160;      // A letter pixel's grey at least,
constexpr int kBluish = 24;      // its blue at most this above its red and its green.
constexpr int kDark = 100;       // A rim pixel's grey at most.
constexpr float kMatch = 0.8f;   // A glyph's correlation at least,
constexpr float kContrast = 60;  // its letters this much lighter than their rim,
constexpr float kExtra = 0.15f;  // and rimmed letter pixels it does not have at most this share of its own.
constexpr float kWider = 0.02f;  // Of two glyphs this near in score, the wider.
constexpr int kGap = 3;          // The next letters start at most this many columns after a glyph.
constexpr int kMinRun = 3;       // Letters not known: at least this many letter pixels, two columns wide,
constexpr int kMinRows = 5;      // over this many rows,
constexpr int kTopRow = 3;       // the first at most this row.
constexpr int kMaxParts = 8;
constexpr float kRowsShare = 0.22f;  // The letters' rows: 1 to just before this share of the side.
constexpr float kStartShare = 0.2f;  // The label starts in this share of the width.
constexpr int kSideSlack = 2;        // Glyphs learned at a side this near are used.
constexpr char kHex[] = "0123456789abcdef";

std::vector<std::string_view> wordsOf(std::string_view line) {
    std::vector<std::string_view> words;
    std::size_t at = 0;
    while (at < line.size()) {
        while (at < line.size() && (line[at] == ' ' || line[at] == '\t')) {
            ++at;
        }
        std::size_t end = at;
        while (end < line.size() && line[end] != ' ' && line[end] != '\t') {
            ++end;
        }
        if (end > at) {
            words.push_back(line.substr(at, end - at));
        }
        at = end;
    }
    return words;
}

bool toInt(std::string_view word, int& value) {
    const auto [end, problem] = std::from_chars(word.data(), word.data() + word.size(), value);
    return problem == std::errc() && end == word.data() + word.size();
}

} // namespace

KeyFont::Seen KeyFont::see(const KeyPicture& picture) {
    Seen seen;
    const int side = picture.side;
    seen.side = side;
    seen.top = 1;
    seen.bottom = std::max(2, static_cast<int>(std::lround(static_cast<float>(side) * kRowsShare)));
    const auto count = static_cast<std::size_t>(side) * static_cast<std::size_t>(side);
    seen.grey.assign(count, 0);
    seen.letter.assign(count, 0);
    seen.dark.assign(count, 0);
    seen.columns.assign(static_cast<std::size_t>(std::max(side, 0)), 0);
    if (picture.pixels == nullptr || side < 8) {
        return seen;
    }
    for (int y = 0; y < side; ++y) {
        const std::uint8_t* row = picture.pixels + static_cast<std::size_t>(y) * picture.pitch;
        for (int x = 0; x < side; ++x) {
            const std::uint8_t* p = row + static_cast<std::size_t>(x) * static_cast<std::size_t>(picture.step);
            const int b = p[0];
            const int g = p[1];
            const int r = p[2];
            const int grey = (b * 29 + g * 150 + r * 77) >> 8; // The grey observex uses everywhere (GreyMaker).
            const std::size_t at = static_cast<std::size_t>(y * side + x);
            seen.grey[at] = grey;
            seen.letter[at] = grey >= kLight && b <= r + kBluish && b <= g + kBluish ? 1 : 0;
            seen.dark[at] = grey <= kDark ? 1 : 0;
        }
    }
    for (int x = 1; x < side - 1; ++x) {
        int letters = 0;
        for (int y = seen.top; y < seen.bottom; ++y) {
            letters += seen.letter[static_cast<std::size_t>(y * side + x)];
        }
        seen.columns[static_cast<std::size_t>(x)] = letters;
    }
    return seen;
}

bool KeyFont::prepare(Glyph& glyph) {
    double sum = 0.0;
    int compared = 0;
    glyph.letters = 0;
    glyph.lead = glyph.width;
    glyph.trail = -1;
    for (int y = 0; y < glyph.height; ++y) {
        for (int x = 0; x < glyph.width; ++x) {
            const std::size_t at = static_cast<std::size_t>(y * glyph.width + x);
            if (glyph.kind[at] == '.') {
                continue;
            }
            sum += glyph.grey[at];
            ++compared;
            if (glyph.kind[at] == 'L') {
                ++glyph.letters;
                glyph.lead = std::min(glyph.lead, x);
                glyph.trail = std::max(glyph.trail, x);
            }
        }
    }
    if (glyph.letters == 0) {
        return false;
    }
    const double mean = sum / compared;
    double squares = 0.0;
    glyph.centred.assign(glyph.grey.size(), 0.0f);
    for (std::size_t at = 0; at < glyph.grey.size(); ++at) {
        if (glyph.kind[at] != '.') {
            glyph.centred[at] = static_cast<float>(glyph.grey[at] - mean);
            squares += static_cast<double>(glyph.centred[at]) * glyph.centred[at];
        }
    }
    glyph.spread = static_cast<float>(std::sqrt(squares));
    return glyph.spread > 0.0f;
}

bool KeyFont::cut(const Seen& seen, int a, int z, std::string text, Glyph* glyph) {
    const int side = seen.side;
    const int x0 = std::max(a - 1, 1);
    const int x1 = std::min(z + 2, side - 1);
    const int height = seen.bottom - seen.top + 1; // A row under the letters for the rim.
    if (x1 <= x0 || seen.top + height > side) {
        return false;
    }
    Glyph made;
    made.text = std::move(text);
    made.side = side;
    made.width = x1 - x0;
    made.height = height;
    made.grey.resize(static_cast<std::size_t>(made.width * height));
    made.kind.assign(made.grey.size(), '.');
    const auto inPicture = [&](int tx, int ty) { return static_cast<std::size_t>((seen.top + ty) * side + x0 + tx); };
    const auto isLetter = [&](int tx, int ty) {
        return ty < seen.bottom - seen.top && x0 + tx >= a && x0 + tx <= z && seen.letter[inPicture(tx, ty)] != 0;
    };
    for (int ty = 0; ty < height; ++ty) {
        for (int tx = 0; tx < made.width; ++tx) {
            const std::size_t at = static_cast<std::size_t>(ty * made.width + tx);
            made.grey[at] = static_cast<std::uint8_t>(seen.grey[inPicture(tx, ty)]);
            if (isLetter(tx, ty)) {
                made.kind[at] = 'L';
            }
        }
    }
    for (int ty = 0; ty < height; ++ty) {
        for (int tx = 0; tx < made.width; ++tx) {
            const std::size_t at = static_cast<std::size_t>(ty * made.width + tx);
            if (made.kind[at] == 'L' || seen.dark[inPicture(tx, ty)] == 0) {
                continue;
            }
            bool rim = false;
            for (int ny = std::max(ty - 1, 0); ny <= std::min(ty + 1, height - 1) && !rim; ++ny) {
                for (int nx = std::max(tx - 1, 0); nx <= std::min(tx + 1, made.width - 1) && !rim; ++nx) {
                    rim = isLetter(nx, ny);
                }
            }
            if (rim) {
                made.kind[at] = 'D';
            }
        }
    }
    if (!prepare(made)) {
        return false;
    }
    *glyph = std::move(made);
    return true;
}

float KeyFont::score(const Glyph& glyph, const Seen& seen, int x, int dy) {
    const int side = seen.side;
    const int y0 = seen.top + dy;
    if (x < 1 || x + glyph.width > side - 1 || y0 < 0 || y0 + glyph.height > side) {
        return -1.0f;
    }
    double letterSum = 0.0;
    double darkSum = 0.0;
    int darks = 0;
    double sum = 0.0;
    double squares = 0.0;
    double cross = 0.0;
    int compared = 0;
    for (int ty = 0; ty < glyph.height; ++ty) {
        for (int tx = 0; tx < glyph.width; ++tx) {
            const std::size_t at = static_cast<std::size_t>(ty * glyph.width + tx);
            const char kind = glyph.kind[at];
            if (kind == '.') {
                continue;
            }
            const double grey = seen.grey[static_cast<std::size_t>((y0 + ty) * side + x + tx)];
            if (kind == 'L') {
                letterSum += grey;
            } else {
                darkSum += grey;
                ++darks;
            }
            sum += grey;
            squares += grey * grey;
            cross += glyph.centred[at] * grey;
            ++compared;
        }
    }
    const double letterMean = letterSum / glyph.letters;
    const double darkMean = darks > 0 ? darkSum / darks : 0.0;
    if (letterMean - darkMean < kContrast) {
        return -1.0f;
    }
    const double variance = squares - sum * sum / compared;
    if (variance <= 1e-6) {
        return -1.0f;
    }
    const auto correlation = static_cast<float>(cross / (glyph.spread * std::sqrt(variance)));
    if (correlation < kMatch) {
        return correlation;
    }
    // Rimmed letter pixels in its letter columns and the letter rows that it does not have: another letter there.
    int extra = 0;
    for (int ty = 0; ty < seen.bottom - seen.top; ++ty) {
        const int y = y0 + ty;
        if (y < seen.top || y >= seen.bottom) {
            continue;
        }
        for (int tx = glyph.lead; tx <= glyph.trail; ++tx) {
            const int px = x + tx;
            if (glyph.kind[static_cast<std::size_t>(ty * glyph.width + tx)] == 'L' ||
                seen.letter[static_cast<std::size_t>(y * side + px)] == 0) {
                continue;
            }
            bool rimmed = false;
            for (int ny = std::max(y - 1, seen.top); ny < std::min(y + 2, seen.bottom) && !rimmed; ++ny) {
                for (int nx = std::max(px - 1, 1); nx < std::min(px + 2, side - 1) && !rimmed; ++nx) {
                    rimmed = seen.dark[static_cast<std::size_t>(ny * side + nx)] != 0;
                }
            }
            extra += rimmed ? 1 : 0;
        }
    }
    return static_cast<float>(extra) <= kExtra * static_cast<float>(glyph.letters) ? correlation : -2.0f;
}

std::string KeyFont::read(const KeyPicture& picture, std::vector<KeyPart>* parts) const {
    return read(see(picture), parts);
}

std::string KeyFont::read(const Seen& seen, std::vector<KeyPart>* parts) const {
    if (parts != nullptr) {
        parts->clear();
    }
    const int side = seen.side;
    if (side < 8) {
        return {};
    }
    const auto& columns = seen.columns;
    const auto lettersAt = [&](int x) { return x >= 1 && x < side - 1 ? columns[static_cast<std::size_t>(x)] : 0; };
    // The first column with letters from `from` to `to` (before side - 1), or -1.
    const auto firstFrom = [&](int from, int to) {
        for (int x = std::max(from, 1); x <= std::min(to, side - 2); ++x) {
            if (lettersAt(x) > 0) {
                return x;
            }
        }
        return -1;
    };
    int pos = firstFrom(1, static_cast<int>(static_cast<float>(side) * kStartShare));
    std::string text;
    int count = 0;
    while (pos >= 0 && count < kMaxParts) {
        const Glyph* best = nullptr;
        float bestScore = 0.0f;
        int bestX = 0;
        for (const Glyph& glyph : glyphs_) {
            if (std::abs(glyph.side - side) > kSideSlack) {
                continue;
            }
            for (int x = pos - glyph.lead - 1; x <= pos - glyph.lead + kGap; ++x) {
                for (int dy = -1; dy <= 1; ++dy) {
                    const float s = score(glyph, seen, x, dy);
                    if (s < kMatch) {
                        continue;
                    }
                    if (best == nullptr || s > bestScore + kWider ||
                        (s > bestScore - kWider && glyph.trail - glyph.lead > best->trail - best->lead)) {
                        best = &glyph;
                        bestScore = s;
                        bestX = x;
                    }
                }
            }
        }
        int end = pos;
        if (best != nullptr) {
            text += best->text;
            ++count;
            if (parts != nullptr) {
                parts->push_back({best->text, true, bestX + best->lead, bestX + best->trail, bestScore});
            }
            end = bestX + best->trail + 1;
        } else {
            // Letters no glyph knows: one '?' if they look like a letter.
            int e = pos;
            int letters = 0;
            while (e < side - 1 && lettersAt(e) > 0) {
                letters += lettersAt(e);
                ++e;
            }
            int rows = 0;
            int firstRow = -1;
            for (int y = seen.top; y < seen.bottom; ++y) {
                bool any = false;
                for (int x = pos; x < e && !any; ++x) {
                    any = seen.letter[static_cast<std::size_t>(y * side + x)] != 0;
                }
                if (any) {
                    ++rows;
                    firstRow = firstRow < 0 ? y : firstRow;
                }
            }
            if (letters >= kMinRun && e - pos >= 2 && rows >= kMinRows && firstRow <= kTopRow) {
                text += '?';
                ++count;
                if (parts != nullptr) {
                    parts->push_back({"?", false, pos, e - 1, 0.0f});
                }
            }
            end = e;
        }
        pos = firstFrom(end, end + kGap);
    }
    return text;
}

int KeyFont::teach(const KeyPicture& picture, std::string_view typed) {
    const Seen seen = see(picture);
    std::vector<KeyPart> parts;
    read(seen, &parts);
    if (parts.empty()) {
        return 0;
    }
    // The known parts must be in `typed` in order; the text between them goes to the parts not known there.
    struct Block {
        std::size_t first = 0; // Into parts.
        std::size_t last = 0;
        std::string_view text;
    };
    std::vector<Block> blocks;
    bool agrees = true;
    std::size_t at = 0;
    std::size_t open = parts.size(); // The first part not known since the last known one; parts.size(): none.
    for (std::size_t p = 0; p < parts.size() && agrees; ++p) {
        if (!parts[p].known) {
            open = open == parts.size() ? p : open;
            continue;
        }
        const std::size_t found = parts[p].text.empty() ? at : typed.find(parts[p].text, at);
        if (found == std::string_view::npos) {
            agrees = false;
        } else if (open != parts.size()) {
            blocks.push_back({open, p - 1, typed.substr(at, found - at)});
            open = parts.size();
        } else if (found != at) {
            agrees = false; // Typed letters where nothing is.
        }
        at = found + parts[p].text.size();
    }
    if (agrees && open != parts.size()) {
        blocks.push_back({open, parts.size() - 1, typed.substr(at)});
        at = typed.size();
    }
    agrees = agrees && at == typed.size();

    std::vector<Glyph> learned;
    Glyph glyph;
    if (!agrees) {
        if (cut(seen, parts.front().from, parts.back().to, std::string(typed), &glyph)) {
            learned.push_back(std::move(glyph));
        }
    } else {
        for (const Block& block : blocks) {
            const std::size_t many = block.last - block.first + 1;
            if (many > 1 && many == block.text.size()) {
                for (std::size_t i = 0; i < many; ++i) {
                    const KeyPart& part = parts[block.first + i];
                    if (cut(seen, part.from, part.to, std::string(1, block.text[i]), &glyph)) {
                        learned.push_back(std::move(glyph));
                    }
                }
            } else if (cut(seen, parts[block.first].from, parts[block.last].to, std::string(block.text), &glyph)) {
                learned.push_back(std::move(glyph));
            }
        }
    }
    const int count = static_cast<int>(learned.size());
    for (Glyph& one : learned) {
        glyphs_.push_back(std::move(one));
    }
    return count;
}

bool KeyFont::parse(std::string_view text, std::string* error) {
    std::vector<Glyph> glyphs;
    int number = 0;
    while (!text.empty()) {
        const std::size_t end = text.find('\n');
        std::string_view line = text.substr(0, end);
        text = end == std::string_view::npos ? std::string_view() : text.substr(end + 1);
        ++number;
        if (const std::size_t cr = line.find('\r'); cr != std::string_view::npos) {
            line = line.substr(0, cr);
        }
        const std::vector<std::string_view> words = wordsOf(line);
        if (words.empty() || words[0].front() == '#') {
            continue;
        }
        Glyph glyph;
        const auto fail = [&](const char* why) {
            if (error != nullptr) {
                *error = std::string(kKeysFile) + " line " + std::to_string(number) + ": " + why;
            }
            return false;
        };
        if (words[0] != "glyph" || words.size() < 6 || words.size() > 7 || !toInt(words[1], glyph.side) ||
            !toInt(words[2], glyph.width) || !toInt(words[3], glyph.height) || glyph.side < 8 || glyph.side > 64 ||
            glyph.width < 1 || glyph.width > 64 || glyph.height < 1 || glyph.height > 64) {
            return fail("expected: glyph SIDE WIDTH HEIGHT GREY KINDS [TEXT]");
        }
        const auto pixels = static_cast<std::size_t>(glyph.width * glyph.height);
        const std::string_view grey = words[4];
        const std::string_view kinds = words[5];
        if (grey.size() != pixels * 2 || kinds.size() != pixels) {
            return fail("GREY is 2 hex digits a pixel and KINDS 1 letter a pixel (WIDTH x HEIGHT)");
        }
        glyph.grey.resize(pixels);
        glyph.kind.resize(pixels);
        for (std::size_t i = 0; i < pixels; ++i) {
            const auto digit = [](char c) {
                return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
            };
            const int high = digit(grey[i * 2]);
            const int low = digit(grey[i * 2 + 1]);
            if (high < 0 || low < 0 || (kinds[i] != 'L' && kinds[i] != 'D' && kinds[i] != '.')) {
                return fail("GREY is hex digits, KINDS L (letter), D (its rim) or . (not compared)");
            }
            glyph.grey[i] = static_cast<std::uint8_t>(high * 16 + low);
            glyph.kind[i] = kinds[i];
        }
        glyph.text = words.size() > 6 ? std::string(words[6]) : std::string();
        if (!prepare(glyph)) {
            return fail("a glyph needs letter pixels (L) that are not all one grey");
        }
        glyphs.push_back(std::move(glyph));
    }
    glyphs_ = std::move(glyphs);
    return true;
}

bool KeyFont::load(const std::filesystem::path& file, std::string* error) {
    std::ifstream in(file, std::ios::binary);
    if (!in) {
        glyphs_.clear();
        return true;
    }
    std::stringstream text;
    text << in.rdbuf();
    return parse(text.str(), error);
}

std::string KeyFont::write() const {
    std::string text = "# observex hotbar key labels (heuristics/KeyLabels): glyphs learned from slots whose key was "
                       "typed or told.\n"
                       "# glyph SIDE WIDTH HEIGHT GREY KINDS [TEXT]: learned on a SIDE x SIDE slot; GREY 2 hex digits a "
                       "pixel, rows top to bottom; KINDS L its letter, D the dark rim round it, . not compared; TEXT "
                       "what it says (none: not a letter).\n";
    for (const Glyph& glyph : glyphs_) {
        text += "glyph " + std::to_string(glyph.side) + " " + std::to_string(glyph.width) + " " +
                std::to_string(glyph.height) + " ";
        for (const std::uint8_t byte : glyph.grey) {
            text += kHex[byte >> 4];
            text += kHex[byte & 15];
        }
        text += ' ';
        text.append(glyph.kind.begin(), glyph.kind.end());
        if (!glyph.text.empty()) {
            text += " " + glyph.text;
        }
        text += '\n';
    }
    return text;
}

bool KeyFont::save(const std::filesystem::path& file, std::string* error) const {
    std::filesystem::path temporary = file;
    temporary += ".tmp";
    {
        std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
        out << write();
        if (!out) {
            if (error != nullptr) {
                *error = "could not write " + temporary.string();
            }
            return false;
        }
    }
    std::error_code problem;
    std::filesystem::rename(temporary, file, problem);
    if (problem) {
        if (error != nullptr) {
            *error = "could not replace " + file.string() + ": " + problem.message();
        }
        return false;
    }
    return true;
}

} // namespace observex
