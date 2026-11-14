#include "heuristics/StatsReader.h"

#include "heuristics/Miniatures.h"

#include <algorithm>
#include <bit>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <numbers>
#include <sstream>

namespace observex {

namespace {

// A glyph is there where its masked correlation is at least this, and its letter is this much brighter (grey levels)
// than the dark line round it (a bar's own edge is neither).
constexpr float kMinMatch = 0.8f;
constexpr float kMinContrast = 40.0f;
// A glyph is only looked for where a column of its letter has a pixel this bright (the letters are white).
constexpr float kBright = 200.0f;
// A bar counts as there when its first columns are filled (or its number was read).
constexpr int kFillStart = 4;
// How many columns of other colours its filled part may have in it: under the number written over it (a letter and
// its dark line cover a few columns from top to bottom). Past that its empty part has begun; the columns before where
// the number may start (the bar's own label, e.g. "HP" on a dark plate) never end it.
constexpr int kFillGap = 8;
// A bar's empty part is dark (dark red, dark blue or grey): no channel above this. Lighter there, a window is over it.
constexpr std::uint8_t kEmptyMost = 120;
// A bar's coloured empty part ("empty red|blue|grey" in stats.txt: dark red, dark blue, grey) is no lighter than this
// in any channel, and pixels lighter than kEmptyLetters in every channel are letters (left out).
constexpr int kEmptyColoured = 160;
constexpr int kEmptyLetters = 150;
// Its own colour's channel (blue of a blue bar, red of a red one) may reach this: a dark blue empty part is ~(120, 80,
// 60) and lighter on some screens. Light frames and white letters of a window over it are light in every channel.
constexpr std::uint8_t kEmptyOwnMost = 150;
// A number agrees with its bar when they are at most this far apart (a share of the bar; one bar column is ~0.25%).
constexpr float kAgree = 0.02f;
// A template pixel this bright is on the letter (white), darker ones on the line round it.
constexpr std::uint8_t kLetterGrey = 150;
// The widest part read: a status window cannot be wider than this.
constexpr int kMaxPartWidth = 1024;

// The cast bar's fill is light blue (cyan), its head a white glow. The fill must begin within this many columns of
// the bar's left, and its run may break for this many (the skill's name written over it).
constexpr int kCastStart = 12;
constexpr int kCastGap = 8;

// A skill's miniature cooling down is its picture darkened (measured on Wind Strike, clip 2026-11-12_23-25-45: each
// pixel 0.38 of its grey plus 10, the same in every colour), but for a sector from 12 o'clock clockwise that grows
// back to the picture as the cooldown passes.
constexpr float kDarkened = 0.38f;
constexpr float kDarkenedPlus = 10.0f;
// A miniature is a skill when its pixels are on average at most this far from the skill's picture or its darkened
// picture, whichever is nearer each (Wind Strike: at most 14 on every frame, cooling or not; the other miniatures 29
// and more).
constexpr float kSkillMatch = 22.0f;
// A miniature's size may differ from the skill's by this much (it is compared at its top left, a pixel either way).
constexpr int kSideSlack = 2;
// Left out of the comparing: the key written at the top left (F2, F12: it is the slot's, not the skill's), and the
// seconds in the middle (shares of the side).
constexpr float kKeyWidth = 0.5f;
constexpr float kKeyHeight = 0.3f;
constexpr float kSecondsWidth = 0.22f;
constexpr float kSecondsHeight = 0.3f;
// Its picture tells bright from darkened only where they are this far apart (the picture's grey above ~56).
constexpr float kTellApart = 25.0f;
// Ready when at least this share of those pixels is bright; else the bright sector's angle says how far the cooldown
// is: kSectors sectors (5 degrees each) from 12 o'clock clockwise, up to the first one less than half bright.
constexpr float kReadyShare = 0.95f;
constexpr int kSectors = 72;
// A yellow letter's pixel (a cooldown's seconds: white-yellow with a gold rim, over a white, blue or dark picture).
inline bool yellow(const std::uint8_t* bgra) { return bgra[2] >= 170 && bgra[2] - bgra[0] >= 12; }
// A yellow glyph is there where the yellow pixels and its letter overlap this much (Dice: twice the overlap over
// both counts).
constexpr float kMinYellowMatch = 0.6f;
// A skill not found is looked for over the whole hotbar again after this many images (the UI finder reads 10 a
// second), and where it was last seen on every one.
constexpr std::uint64_t kLookOverEvery = 3;
// Where a skill was, a picture this alike in its shapes (correlation of its pixels away from the edge, kAlikeEdge) but
// neither ready nor darkened is the look after its key was pressed: the picture dimmed in a light frame for ~0.8 s
// (clip 2026-11-14_14-50-29, Wind Strike: 0.27, 0.50, 0.45, 0.61, 0.68 over it, 0.27 on its darkest first image).
// Only ever where it was: searched over a whole screen anything reaches 0.6-0.68 somewhere (2026-11-14, 22 clips), but
// at one place another picture is not alike. Within kPressedHold readings of being seen there, kPressedAlikeHeld is
// enough (its darkest images).
// Found by its pixels (nearer than kSkillMatch), a skill is there only if its shapes are this alike too (ready 1.0,
// cooling 0.40-0.80 even on darker or lighter screens; elsewhere by chance 0.17 and less, 2026-11-14 stress images).
constexpr float kFoundAlike = 0.3f;
// A miniature's edge is light (each pixel's darkest channel 148-197 on a skill ready, cooling or pressed, even on a
// darker screen): at least half its edge pixels this light. An empty slot's edge, or where the hotbar was, is 60-90.
constexpr int kEdgeLight = 120;
constexpr float kPressedAlike = 0.5f;
constexpr float kPressedAlikeHeld = 0.2f;
constexpr std::uint64_t kPressedHold = 12;
constexpr int kAlikeEdge = 3;
// Words are white: each channel at least this (the target's name 220 and more; a light sky behind its window up to
// ~180, clip 2026-11-12_23-25-45). Letters this far apart or less are one run of words (a space), and a run is words
// when it is at least this wide.
constexpr int kWordsWhite = 200;
// The picture Windows' OCR reads: black where the words are this white or whiter, white where they are this much
// less, grey between (their edges).
constexpr int kWordsBlack = 225;
constexpr int kWordsPaper = 170;
constexpr int kWordsGap = 8;
constexpr int kWordsLeast = 6;
// Pictures of words read, kept (the names of the targets met).
constexpr std::size_t kWordsKept = 64;
// A new target (or none) is said (GameStats::targetChanged) once it held on this many readings in a row.
constexpr int kTargetHolds = 2;
// Names read for one target, kept with how many readings read each.
constexpr std::size_t kNameVotes = 8;
// A window's close button (stats.txt "close"): looked for this many pixels either way of where it sits; there when
// its light pixels (every channel at least kCloseLight) lie along its two diagonals: the share of the diagonals' pixels
// light, less the share of the others, at least kCloseScore (the target window's X: 0.40-0.98 on 185 images in 5
// brightnesses; where another window covers the target window's right end, 0.20 and less; 2026-11-14).
constexpr int kCloseShift = 3;
constexpr int kCloseLight = 170;
constexpr float kCloseScore = 0.3f;
// How much farther left a bar's number may begin than when last read (centred, it grows both ways).
constexpr int kNumberPlay = 16;
// A name picture this many pixels wider or narrower than the name read is another name (a name a letter longer is ~7).
constexpr int kNameWidthSlack = 4;

// Whether two names read are one name, one of them read a little wrong: at most a third of the shorter's letters
// (two of "Gremlin"'s seven) to change, add or take away, ignoring case.
bool sameName(std::string_view a, std::string_view b) {
    if (a.empty() || b.empty()) {
        return false;
    }
    const auto lower = [](char c) { return static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c); };
    std::vector<int> row(b.size() + 1);
    for (std::size_t j = 0; j <= b.size(); ++j) {
        row[j] = static_cast<int>(j);
    }
    for (std::size_t i = 1; i <= a.size(); ++i) {
        int diagonal = row[0];
        row[0] = static_cast<int>(i);
        for (std::size_t j = 1; j <= b.size(); ++j) {
            const int above = row[j];
            row[j] = std::min({row[j] + 1, row[j - 1] + 1, diagonal + (lower(a[i - 1]) == lower(b[j - 1]) ? 0 : 1)});
            diagonal = above;
        }
    }
    return row[b.size()] * 3 <= static_cast<int>(std::min(a.size(), b.size()));
}
// Words are the same as on the image before when at most this share of their white pixels differ, moved up to
// kWordsShift columns either way (a run's ends shift a pixel with the sky behind).
constexpr float kWordsSame = 0.2f;
constexpr int kWordsShift = 2;

template <std::size_t N> void copyName(std::array<char, N>& to, std::string_view from) {
    to.fill('\0');
    std::memcpy(to.data(), from.data(), std::min(from.size(), N - 1));
}

std::vector<std::string_view> splitWords(std::string_view line) {
    std::vector<std::string_view> words;
    std::size_t at = 0;
    while (at < line.size()) {
        while (at < line.size() && (line[at] == ' ' || line[at] == '\t' || line[at] == '\r')) {
            ++at;
        }
        std::size_t end = at;
        while (end < line.size() && line[end] != ' ' && line[end] != '\t' && line[end] != '\r') {
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

// The grey observex uses everywhere (GreyMaker, core/Heuristics' GreyImage, make_digits.py).
inline float greyOf(const std::uint8_t* bgra) {
    return static_cast<float>((bgra[0] * 29 + bgra[1] * 150 + bgra[2] * 77) >> 8);
}

std::array<char, 16> targetPiece() {
    std::array<char, 16> name{};
    copyName(name, kTargetPiece);
    return name;
}

} // namespace

bool StatsReader::load(const std::filesystem::path& folder, std::string* error) {
    std::ifstream in(folder / "stats.txt", std::ios::binary);
    if (!in) {
        fonts_.clear();
        bars_.clear();
        if (error != nullptr) {
            *error = "no stats.txt in " + folder.string();
        }
        return false;
    }
    std::stringstream text;
    text << in.rdbuf();
    return parse(text.str(), folder, error);
}

bool StatsReader::parse(std::string_view text, const std::filesystem::path& folder, std::string* error) {
    const auto clear = [&] {
        fonts_.clear();
        bars_.clear();
        lines_.clear();
        cast_ = {};
        skillsPiece_ = {};
        skillsFont_ = -1;
        words_.clear();
        closes_.clear();
        lastWords_.clear();
        lookOver_ = true; // The skills themselves are setSkills()'.
    };
    clear();
    int lineNumber = 0;
    const auto fail = [&](const std::string& why) {
        clear();
        if (error != nullptr) {
            *error = "stats.txt line " + std::to_string(lineNumber) + ": " + why;
        }
        return false;
    };
    const auto fontNamed = [&](std::string_view name) {
        for (std::size_t i = 0; i < fonts_.size(); ++i) {
            if (fonts_[i].name == name) {
                return static_cast<int>(i);
            }
        }
        return -1;
    };
    std::size_t at = 0;
    while (at <= text.size()) {
        std::size_t end = text.find('\n', at);
        if (end == std::string_view::npos) {
            end = text.size();
        }
        const std::string_view line = text.substr(at, end - at);
        at = end + 1;
        ++lineNumber;
        const std::vector<std::string_view> words = splitWords(line);
        if (words.empty() || words[0].front() == '#' || words[0] == "from") {
            continue; // "from" lines are make_digits.py's.
        }
        if (words[0] == "glyphs") {
            Font font;
            if ((words.size() != 6 && words.size() != 7) || !toInt(words[3], font.pitch) ||
                !toInt(words[4], font.width) || !toInt(words[5], font.height) || font.pitch <= 0 || font.width <= 0 ||
                font.height <= 0 || font.width > 127 || font.height > 127 ||
                (words.size() == 7 && words[6] != "yellow")) {
                return fail("expected: glyphs NAME CHARS PITCH W H [yellow]");
            }
            font.name = std::string(words[1]);
            font.yellow = words.size() == 7;
            const std::string_view chars = words[2];
            std::vector<std::uint8_t> pixels;
            std::vector<std::uint8_t> mask;
            int width = 0;
            int height = 0;
            int maskWidth = 0;
            int maskHeight = 0;
            if (!readPgm(folder / (font.name + ".pgm"), pixels, width, height) ||
                !readPgm(folder / (font.name + "-mask.pgm"), mask, maskWidth, maskHeight)) {
                return fail("could not read " + font.name + ".pgm and " + font.name +
                            "-mask.pgm (run tools/ui/make_digits.py)");
            }
            if (width != font.width * static_cast<int>(chars.size()) || height != font.height ||
                maskWidth != width || maskHeight != height) {
                return fail(font.name + ".pgm is not " + std::to_string(chars.size()) + " pictures of " +
                            std::to_string(font.width) + " x " + std::to_string(font.height));
            }
            for (std::size_t i = 0; i < chars.size(); ++i) {
                Glyph glyph;
                glyph.c = chars[i];
                if (font.yellow) {
                    // Its picture is the share of samples a pixel was yellow on: the letter where it was on half.
                    if (font.width > 64) {
                        return fail("a yellow glyph is at most 64 pixels wide");
                    }
                    glyph.rows.assign(static_cast<std::size_t>(font.height), 0);
                    for (int y = 0; y < font.height; ++y) {
                        for (int x = 0; x < font.width; ++x) {
                            const bool on = pixels[static_cast<std::size_t>(y) * static_cast<std::size_t>(width) +
                                                   i * static_cast<std::size_t>(font.width) +
                                                   static_cast<std::size_t>(x)] >= 128;
                            glyph.rows[static_cast<std::size_t>(y)] |= on ? std::uint64_t{1} << x : 0;
                            glyph.ons += on ? 1 : 0;
                        }
                    }
                    if (glyph.ons == 0) {
                        return fail(std::string("the glyph '") + glyph.c + "' has no letter");
                    }
                    font.glyphs.push_back(std::move(glyph));
                    continue;
                }
                double sum = 0.0;
                for (int y = 0; y < font.height; ++y) {
                    for (int x = 0; x < font.width; ++x) {
                        const std::size_t index =
                            static_cast<std::size_t>(y) * static_cast<std::size_t>(width) + i * font.width + x;
                        if (mask[index] == 0) {
                            continue;
                        }
                        GlyphPixel pixel;
                        pixel.dx = static_cast<std::int8_t>(x);
                        pixel.dy = static_cast<std::int8_t>(y);
                        pixel.letter = pixels[index] >= kLetterGrey;
                        pixel.centred = static_cast<float>(pixels[index]);
                        sum += pixel.centred;
                        glyph.pixels.push_back(pixel);
                    }
                }
                if (glyph.pixels.empty()) {
                    return fail(std::string("the glyph '") + glyph.c + "' has an empty mask");
                }
                const auto mean = static_cast<float>(sum / static_cast<double>(glyph.pixels.size()));
                double spread = 0.0;
                for (GlyphPixel& pixel : glyph.pixels) {
                    pixel.centred -= mean;
                    spread += static_cast<double>(pixel.centred) * pixel.centred;
                }
                glyph.spread = static_cast<float>(std::sqrt(spread));
                font.glyphs.push_back(std::move(glyph));
            }
            font.letterFrom = font.width;
            for (const Glyph& glyph : font.glyphs) {
                for (const GlyphPixel& pixel : glyph.pixels) {
                    if (pixel.letter) {
                        font.letterFrom = std::min(font.letterFrom, static_cast<int>(pixel.dx));
                        font.letterTo = std::max(font.letterTo, pixel.dx + 1);
                    }
                }
            }
            fonts_.push_back(std::move(font));
        } else if (words[0] == "bar") {
            Bar bar;
            std::size_t used = words.size();
            if (used >= 10 && words[used - 2] == "empty") {
                if (words[used - 1] == "red") {
                    bar.empty = Empty::Red;
                } else if (words[used - 1] == "blue") {
                    bar.empty = Empty::Blue;
                } else if (words[used - 1] == "grey") {
                    bar.empty = Empty::Grey;
                } else {
                    return fail("a bar's empty colour is red, blue or grey");
                }
                used -= 2;
            }
            if ((used != 8 && used != 12) || !toInt(words[4], bar.top) ||
                !toInt(words[5], bar.bottom) || !toInt(words[6], bar.left) || !toInt(words[7], bar.right) ||
                bar.bottom <= bar.top) {
                return fail("expected: bar NAME PIECE FILL TOP BOTTOM LEFT RIGHT [text GLYPHS ROW FROM] [empty COLOUR]");
            }
            copyName(bar.name, words[1]);
            copyName(bar.piece, words[2]);
            if (words[3] == "red") {
                bar.fill = Fill::Red;
            } else if (words[3] == "blue") {
                bar.fill = Fill::Blue;
            } else if (words[3] == "lilac") {
                bar.fill = Fill::Lilac;
            } else {
                return fail("a bar's FILL is red, blue or lilac");
            }
            if (used == 12) {
                if (words[8] != "text" || !toInt(words[10], bar.textRow) || !toInt(words[11], bar.textFrom)) {
                    return fail("expected: ... text GLYPHS ROW FROM");
                }
                bar.font = fontNamed(words[9]);
                if (bar.font < 0 || fonts_[static_cast<std::size_t>(bar.font)].yellow) {
                    return fail("no glyphs line (not yellow) names " + std::string(words[9]) + " above this one");
                }
            }
            if (bars_.size() >= static_cast<std::size_t>(GameStats::kMax)) {
                return fail("more than " + std::to_string(GameStats::kMax) + " bars");
            }
            lines_.push_back({Line::Kind::Bar, static_cast<int>(bars_.size())});
            bars_.push_back(bar);
        } else if (words[0] == "cast") {
            if (words.size() != 6 || !toInt(words[2], cast_.top) || !toInt(words[3], cast_.bottom) ||
                !toInt(words[4], cast_.left) || !toInt(words[5], cast_.right) || cast_.bottom <= cast_.top ||
                cast_.right <= cast_.left + kCastStart) {
                return fail("expected: cast PIECE TOP BOTTOM LEFT RIGHT");
            }
            if (cast_.piece[0] != '\0') {
                return fail("a second cast line");
            }
            copyName(cast_.piece, words[1]);
            lines_.push_back({Line::Kind::Cast, 0});
        } else if (words[0] == "skills") {
            if (words.size() != 3) {
                return fail("expected: skills PIECE GLYPHS");
            }
            if (skillsPiece_[0] != '\0') {
                return fail("a second skills line");
            }
            skillsFont_ = fontNamed(words[2]);
            if (skillsFont_ < 0 || !fonts_[static_cast<std::size_t>(skillsFont_)].yellow) {
                return fail("no yellow glyphs line names " + std::string(words[2]) + " above this one");
            }
            copyName(skillsPiece_, words[1]);
            lines_.push_back({Line::Kind::Skills, 0});
        } else if (words[0] == "words") {
            Words read;
            if (words.size() != 7 || !toInt(words[3], read.top) || !toInt(words[4], read.bottom) ||
                !toInt(words[5], read.left) || !toInt(words[6], read.right) || read.bottom <= read.top) {
                return fail("expected: words NAME PIECE TOP BOTTOM LEFT RIGHT");
            }
            copyName(read.name, words[1]);
            copyName(read.piece, words[2]);
            lines_.push_back({Line::Kind::Words, static_cast<int>(words_.size())});
            words_.push_back(read);
        } else if (words[0] == "close") {
            Close close;
            if (words.size() != 6 || !toInt(words[2], close.top) || !toInt(words[3], close.bottom) ||
                !toInt(words[4], close.left) || !toInt(words[5], close.right) || close.bottom <= close.top ||
                close.right <= close.left) {
                return fail("expected: close PIECE TOP BOTTOM LEFT RIGHT");
            }
            copyName(close.piece, words[1]);
            closes_.push_back(close);
        } else {
            return fail("unknown line (glyphs, from, bar, cast, skills, words or close)");
        }
    }
    lastWords_.assign(words_.size(), LastWords{});
    return true;
}

void StatsReader::setSkills(std::span<const SkillPicture> pictures) {
    skills_.clear();
    for (const SkillPicture& picture : pictures) {
        // At most as many as gauges fit, each a square picture a miniature can be.
        if (skills_.size() + 8 >= static_cast<std::size_t>(GameStats::kMax)) {
            break;
        }
        const int width = picture.side;
        if (picture.grey == nullptr || width < 8 || width > 64) {
            continue;
        }
        Skill skill;
        skill.name.assign(picture.name.data(), strnlen(picture.name.data(), picture.name.size()));
        skill.cooldown = picture.cooldown;
        skill.side = width;
        skill.pixels.assign(picture.grey, picture.grey + static_cast<std::size_t>(width) * static_cast<std::size_t>(width));
        skill.use.assign(skill.pixels.size(), 0);
        skill.sector.assign(skill.pixels.size(), static_cast<std::uint8_t>(kSectors));
        const auto side = static_cast<float>(width);
        const float middle = side * 0.5f;
        // Not its frame's top row and left column, the key's letters or the seconds.
        for (int y = 1; y < width; ++y) {
            for (int x = 1; x < width; ++x) {
                const auto fx = static_cast<float>(x);
                const auto fy = static_cast<float>(y);
                const bool key = fx < kKeyWidth * side && fy < kKeyHeight * side;
                const bool seconds = std::abs(fx - middle) < kSecondsWidth * side &&
                                     std::abs(fy - middle) < kSecondsHeight * side;
                if (key || seconds) {
                    continue;
                }
                const std::size_t at = static_cast<std::size_t>(y * width + x);
                skill.use[at] = 1;
                ++skill.uses;
                const float ready = skill.pixels[at];
                if (std::abs(ready - (kDarkened * ready + kDarkenedPlus)) > kTellApart) {
                    // Its angle from 12 o'clock clockwise (screen y down).
                    float angle = std::atan2(fx + 0.5f - middle, middle - (fy + 0.5f));
                    if (angle < 0.0f) {
                        angle += 2.0f * std::numbers::pi_v<float>;
                    }
                    const int sector = static_cast<int>(angle / (2.0f * std::numbers::pi_v<float>) * kSectors);
                    skill.sector[at] = static_cast<std::uint8_t>(std::min(sector, kSectors - 1));
                }
            }
        }
        // For alike(): the compared pixels away from the edge (the pressed flash frames it light blue).
        skill.centred.assign(skill.pixels.size(), 0.0f);
        double sum = 0.0;
        int count = 0;
        for (int y = kAlikeEdge; y < width - kAlikeEdge; ++y) {
            for (int x = kAlikeEdge; x < width - kAlikeEdge; ++x) {
                const std::size_t at = static_cast<std::size_t>(y * width + x);
                if (skill.use[at] != 0) {
                    sum += skill.pixels[at];
                    ++count;
                }
            }
        }
        const float mean = count > 0 ? static_cast<float>(sum / count) : 0.0f;
        double squares = 0.0;
        for (int y = kAlikeEdge; y < width - kAlikeEdge; ++y) {
            for (int x = kAlikeEdge; x < width - kAlikeEdge; ++x) {
                const std::size_t at = static_cast<std::size_t>(y * width + x);
                if (skill.use[at] != 0) {
                    skill.centred[at] = skill.pixels[at] - mean;
                    squares += static_cast<double>(skill.centred[at]) * skill.centred[at];
                }
            }
        }
        skill.spread = static_cast<float>(std::sqrt(squares));
        skills_.push_back(std::move(skill));
    }
    cooling_.assign(skills_.size(), 0);
    began_.assign(skills_.size(), 0);
    skillAt_.assign(skills_.size(), StatsArea{});
    seenOn_.assign(skills_.size(), 0);
    pressed_.assign(skills_.size(), 0);
    lookOver_ = true;
}

int StatsReader::areas(std::span<const UiPiece> pieces, int width, int height, StatsArea* out, int max) const {
    int count = 0;
    // Rows top to just before bottom of `piece` (from its top), across it.
    const auto add = [&](const UiPiece& piece, int top, int bottom) {
        StatsArea area{std::max(piece.x0, 0), std::max(piece.y0 + top, 0),
                       std::min({piece.x1, width, piece.x0 + kMaxPartWidth}), std::min(piece.y0 + bottom, height)};
        if (area.x1 <= area.x0 || area.y1 <= area.y0) {
            return;
        }
        // One area a piece: grown to hold its other bars (and pieces one under another, the hotbar's rows).
        for (int i = 0; i < count; ++i) {
            StatsArea& other = out[i];
            if (other.x0 == area.x0 && other.x1 == area.x1 && other.y1 >= area.y0 - 64 && area.y1 >= other.y0 - 64) {
                other.y0 = std::min(other.y0, area.y0);
                other.y1 = std::max(other.y1, area.y1);
                return;
            }
        }
        if (count < max) {
            out[count++] = area;
        }
    };
    const auto named = [](const UiPiece& piece, const std::array<char, 16>& name) {
        return std::strcmp(piece.name.data(), name.data()) == 0;
    };
    // Every piece by each name: which of several is read is chosen on the image (by its close button).
    for (const UiPiece& piece : pieces) {
        for (const Bar& bar : bars_) {
            if (!named(piece, bar.piece)) {
                continue;
            }
            int top = bar.top;
            int bottom = bar.bottom;
            if (bar.font >= 0) {
                top = std::min(top, bar.textRow - 1);
                bottom = std::max(bottom, bar.textRow + fonts_[static_cast<std::size_t>(bar.font)].height + 1);
            }
            add(piece, top, bottom);
        }
        for (const Words& words : words_) {
            if (named(piece, words.piece)) {
                add(piece, words.top, words.bottom);
            }
        }
        for (const Close& close : closes_) {
            if (named(piece, close.piece)) {
                add(piece, close.top - kCloseShift, close.bottom + kCloseShift);
            }
        }
    }
    // The skills: round where each was last seen (a few kB; also while it is not there, so it is seen at once when it
    // comes back), and the hotbar's pieces whole when they are to be looked over.
    StatsArea around{width, height, 0, 0};
    for (const StatsArea& at : skillAt_) {
        if (at.x1 <= at.x0) {
            continue;
        }
        around = {std::min(around.x0, at.x0 - 3), std::min(around.y0, at.y0 - 3), std::max(around.x1, at.x1 + 3),
                  std::max(around.y1, at.y1 + 3)};
    }
    if (around.x1 > around.x0 && count < max) {
        out[count++] = {std::max(around.x0, 0), std::max(around.y0, 0), std::min(around.x1, width),
                        std::min(around.y1, height)};
    }
    for (const UiPiece& piece : pieces) {
        if (cast_.piece[0] != '\0' && named(piece, cast_.piece)) {
            add(piece, cast_.top, cast_.bottom);
        }
        if (lookOver_ && skillsPiece_[0] != '\0' && !skills_.empty() && named(piece, skillsPiece_)) {
            add(piece, 0, piece.y1 - piece.y0);
        }
    }
    return count;
}

void StatsReader::read(std::span<const UiPiece> pieces, std::span<const ColourPart> parts, GameStats& out) {
    ++reads_;
    // The part holding rows y0 to just before y1 at the left of `piece`, or null.
    const auto partOf = [&](const UiPiece& piece, int y0, int y1) -> const ColourPart* {
        const auto part = std::find_if(parts.begin(), parts.end(), [&](const ColourPart& p) {
            return p.area.x0 <= piece.x0 && p.area.x1 > piece.x0 && p.area.y0 <= y0 && p.area.y1 >= y1;
        });
        return part == parts.end() ? nullptr : &*part;
    };
    // Of the pieces with a close button ("close"), the one whose button shows when several are found by that name (a
    // look-alike of the window's corner elsewhere has none).
    std::vector<const UiPiece*> chosen;
    const auto pieceNamed = [&](const std::array<char, 16>& name) -> const UiPiece* {
        for (const UiPiece* piece : chosen) {
            if (std::strcmp(piece->name.data(), name.data()) == 0) {
                return piece;
            }
        }
        const auto piece = std::find_if(pieces.begin(), pieces.end(), [&](const UiPiece& p) {
            return std::strcmp(p.name.data(), name.data()) == 0;
        });
        return piece == pieces.end() ? nullptr : &*piece;
    };

    why_.clear();
    targetWordsWidth_ = 0;
    // Pieces whose close button is not where it sits: another window covers their right end (their right side found is
    // that window's), so nothing on them is read.
    std::vector<const UiPiece*> covered;
    for (const Close& close : closes_) {
        const UiPiece* first = nullptr;
        const UiPiece* shown = nullptr;
        for (const UiPiece& piece : pieces) {
            if (std::strcmp(piece.name.data(), close.piece.data()) != 0) {
                continue;
            }
            first = first != nullptr ? first : &piece;
            if (shown == nullptr && closeShown(close, piece, parts)) {
                shown = &piece;
            }
        }
        const UiPiece* piece = shown != nullptr ? shown : first;
        if (piece == nullptr) {
            continue;
        }
        chosen.push_back(piece);
        if (shown == nullptr) {
            covered.push_back(piece);
            char line[160];
            std::snprintf(line, sizeof(line), "%s: no close button at %d,%d (a window over its right end?)",
                          close.piece.data(), piece->x1, piece->y0);
            because(line);
        }
    }
    const auto isCovered = [&](const UiPiece* piece) {
        return std::find(covered.begin(), covered.end(), piece) != covered.end();
    };
    // The added skills first: the cast's name comes from them.
    findSkills(pieces, parts);
    lefts_.assign(skills_.size(), -1.0f);
    std::vector<std::uint8_t> pressedNow(skills_.size(), 0);
    for (const Found& found : found_) {
        if (found.skill >= 0) {
            const Skill& skill = skills_[static_cast<std::size_t>(found.skill)];
            lefts_[static_cast<std::size_t>(found.skill)] =
                found.pressed || !skill.cooldown ? 0.0f : cooldownLeft(skill, found.box, *found.part);
            pressedNow[static_cast<std::size_t>(found.skill)] = found.pressed ? 1 : 0;
        }
    }
    // Each one's cooldown that began, or its press, on this image (one not found keeps what it was): the cast's name.
    for (std::size_t s = 0; s < skills_.size(); ++s) {
        if (lefts_[s] < 0.0f) {
            continue;
        }
        const bool cooling = lefts_[s] > 0.0f;
        if ((cooling && cooling_[s] == 0) || (pressedNow[s] != 0 && pressed_[s] == 0)) {
            began_[s] = reads_;
        }
        cooling_[s] = cooling ? 1 : 0;
        pressed_[s] = pressedNow[s];
    }

    out.count = 0;
    const auto next = [&]() -> GameGauge* {
        if (out.count >= GameStats::kMax) {
            return nullptr;
        }
        GameGauge& gauge = out.gauges[static_cast<std::size_t>(out.count++)];
        gauge = {};
        return &gauge;
    };
    for (const Line& line : lines_) {
        if (line.kind == Line::Kind::Bar) {
            const Bar& bar = bars_[static_cast<std::size_t>(line.index)];
            GameGauge* gauge = next();
            if (gauge == nullptr) {
                break;
            }
            gauge->piece = bar.piece;
            copyName(gauge->name, bar.name.data());
            if (const UiPiece* piece = pieceNamed(bar.piece); piece != nullptr && isCovered(piece)) {
                // Said above.
            } else if (piece != nullptr) {
                if (const ColourPart* part = partOf(*piece, piece->y0 + bar.top, piece->y0 + bar.bottom)) {
                    readBar(bar, *piece, *part, *gauge);
                    if (!gauge->seen || gauge->full < 0.0f) {
                        because(std::string(bar.piece.data()) + " " + bar.name.data() +
                                (gauge->seen ? ": bar hidden (a window over it?)" : ": not seen") +
                                (gauge->doubted[0] != '\0' ? std::string(", read ") + gauge->doubted.data() : ""));
                    }
                }
            } else if (std::strcmp(bar.piece.data(), kTargetPiece) != 0) { // No target window: said once, below.
                because(std::string(bar.piece.data()) + " " + bar.name.data() + ": no " + bar.piece.data() +
                        " window");
            }
        } else if (line.kind == Line::Kind::Cast) {
            GameGauge* gauge = next();
            if (gauge == nullptr) {
                break;
            }
            gauge->piece = cast_.piece;
            copyName(gauge->name, "Casting");
            const UiPiece* piece = pieceNamed(cast_.piece);
            const ColourPart* part =
                piece != nullptr ? partOf(*piece, piece->y0 + cast_.top, piece->y0 + cast_.bottom) : nullptr;
            if (part == nullptr) {
                continue;
            }
            gauge->seen = true;
            gauge->full = readCast(*piece, *part);
            // What is cast: the skill whose cooldown began, or that was pressed, last, and goes on (casting begins its
            // cooldown); if none was seen beginning (the reader is new), the one cooling with the most left.
            int casting = -1;
            for (std::size_t s = 0; s < skills_.size(); ++s) {
                if (cooling_[s] == 0 && (pressed_[s] == 0 || began_[s] == 0)) {
                    continue;
                }
                const auto best = static_cast<std::size_t>(casting);
                if (casting < 0 || began_[s] > began_[best] ||
                    (began_[s] == began_[best] && lefts_[s] > lefts_[best])) {
                    casting = static_cast<int>(s);
                }
            }
            if (casting >= 0) {
                copyName(gauge->text, skills_[static_cast<std::size_t>(casting)].name);
            }
        } else if (line.kind == Line::Kind::Words) {
            const Words& words = words_[static_cast<std::size_t>(line.index)];
            GameGauge* gauge = next();
            if (gauge == nullptr) {
                break;
            }
            gauge->piece = words.piece;
            gauge->name = words.name;
            if (const UiPiece* piece = pieceNamed(words.piece); piece != nullptr && !isCovered(piece)) {
                if (const ColourPart* part = partOf(*piece, piece->y0 + words.top, piece->y0 + words.bottom)) {
                    wordsWidth_ = 0;
                    readWords(words, *piece, *part, gauge->text);
                    gauge->seen = gauge->text[0] != '\0';
                    if (std::strcmp(words.piece.data(), kTargetPiece) == 0 &&
                        std::strcmp(words.name.data(), kTargetNameGauge) == 0) {
                        targetWordsWidth_ = wordsWidth_;
                    }
                }
            }
        } else {
            for (std::size_t s = 0; s < skills_.size(); ++s) {
                GameGauge* gauge = next();
                if (gauge == nullptr) {
                    break;
                }
                gauge->piece = skillsPiece_;
                copyName(gauge->name, skills_[s].name);
                const auto found = std::find_if(found_.begin(), found_.end(),
                                                [&](const Found& f) { return f.skill == static_cast<int>(s); });
                if (found == found_.end()) {
                    continue;
                }
                gauge->seen = true;
                gauge->full = lefts_[s];
                gauge->pressed = found->pressed;
                if (lefts_[s] > 0.0f) {
                    readSeconds(found->box, *found->part, gauge->text);
                }
            }
        }
    }

    // The target: its window shows when the UI finder found it; its name is the one last read while it showed (a
    // picture of it the OCR could not read, or the name covered for a moment, keeps it).
    // Without its close button the window is the target's only when it was there whole on the image before, where it is
    // now (a window put over its right end since); else it is a look-alike (the hotbar's row arrow next to a dark
    // window, 2026-11-14 stress images).
    const UiPiece* targetWindow = pieceNamed(targetPiece());
    out.hasTarget = targetWindow != nullptr;
    if (targetWindow != nullptr && !isCovered(targetWindow)) {
        targetWhole_ = true;
        targetLeft_ = {targetWindow->x0, targetWindow->y0};
    } else if (targetWindow != nullptr) {
        out.hasTarget = targetWhole_ && std::abs(targetWindow->x0 - targetLeft_[0]) <= 2 &&
                        std::abs(targetWindow->y0 - targetLeft_[1]) <= 2;
        if (!out.hasTarget) {
            char line[160];
            std::snprintf(line, sizeof(line), "Target: a window like it at %d,%d, not seen whole before (not taken)",
                          targetWindow->x0, targetWindow->y0);
            because(line);
        }
    }
    if (!out.hasTarget) {
        targetWhole_ = false;
        targetName_ = {};
        nameVotes_.clear();
        if (targetWindow == nullptr) {
            because("Target: no window");
        }
    }
    for (int i = 0; i < out.count; ++i) {
        GameGauge& gauge = out.gauges[static_cast<std::size_t>(i)];
        if (std::strcmp(gauge.piece.data(), kTargetPiece) != 0 ||
            std::strcmp(gauge.name.data(), kTargetNameGauge) != 0) {
            continue;
        }
        if (gauge.text[0] != '\0') {
            // While the window stays, a name a letter or two off is the same target read wrong once ("Grernlin" for
            // "Gremlin"): the name read on the most readings wins. One further off is another target.
            if (!sameName(gauge.text.data(), targetName_.data())) {
                nameVotes_.clear();
            }
            const auto known = std::find_if(nameVotes_.begin(), nameVotes_.end(),
                                            [&](const auto& vote) { return vote.first == gauge.text; });
            if (known != nameVotes_.end()) {
                ++known->second;
            } else if (nameVotes_.size() < kNameVotes) {
                nameVotes_.emplace_back(gauge.text, 1);
            }
            targetName_ = std::max_element(nameVotes_.begin(), nameVotes_.end(), [](const auto& a, const auto& b) {
                              return a.second < b.second;
                          })->first;
            nameWidth_ = targetWordsWidth_;
        } else if (targetWordsWidth_ > 0) {
            // Words there the OCR read nothing on. As wide as the name's (something over it for a moment), the name
            // stays; of another width they are another target's name, not known yet (never the one before's).
            char line[160];
            const bool other = std::abs(targetWordsWidth_ - nameWidth_) > kNameWidthSlack;
            std::snprintf(line, sizeof(line), "Target: name not read (%d px wide; '%s' was %d px%s)", targetWordsWidth_,
                          targetName_.data(), nameWidth_, other ? ", another name" : ", kept");
            because(line);
            if (other) {
                targetName_ = {};
                nameVotes_.clear();
            }
        }
        if (out.hasTarget && targetName_[0] != '\0') {
            gauge.text = targetName_;
            gauge.seen = true;
        }
    }
    out.targetName = targetName_;

    // Target Changed: the target now (None, or its name once read; a window whose name is not read yet keeps the one
    // before), said once it held on kTargetHolds readings in a row.
    std::array<char, 32> now = target_;
    if (!out.hasTarget) {
        now = {};
    } else if (targetName_[0] != '\0') {
        now = targetName_;
    }
    out.targetChanged = false;
    if (now == target_) {
        targetHeld_ = 0;
    } else if (now == targetNext_ && targetHeld_ > 0) {
        if (++targetHeld_ >= kTargetHolds) {
            target_ = now;
            targetHeld_ = 0;
            out.targetChanged = true;
        }
    } else {
        targetNext_ = now;
        targetHeld_ = 1;
    }
    out.target = target_;
    copyName(out.why, why_);
}

void StatsReader::readBar(const Bar& bar, const UiPiece& pieceIn, const ColourPart& partIn, GameGauge& gauge) {
    const UiPiece* piece = &pieceIn;
    const ColourPart* part = &partIn;
    const int y0 = piece->y0 + bar.top;
    const int y1 = piece->y0 + bar.bottom;
    {
        // (Indented as the loop over the bars it was.)
        const int x0 = piece->x0 + bar.left;
        const int x1 = std::min(bar.right < 1 ? piece->x1 + bar.right : piece->x0 + bar.right, part->area.x1);
        if (x1 - x0 < kFillStart * 2) {
            return;
        }

        const auto at = [&](int x, int y) {
            return part->bgra + static_cast<std::size_t>(y - part->area.y0) * part->pitch +
                   static_cast<std::size_t>(x - part->area.x0) * 4;
        };
        // The number first: its letters (white, with a dark line round them) cover the bar from top to bottom, so the
        // columns it takes say nothing of how full the bar is.
        int textFrom = x1;
        int textTo = x1;
        if (bar.font >= 0) {
            readText(bar, piece->x0, piece->y0, x1, *part, gauge.text, &textFrom, &textTo);
        }
        const auto under = [&](int x) { return x >= textFrom && x < textTo; };

        // How full: where the bar's colour ends (the letters written over it are white, its empty part dark), if it
        // begins at the bar's left.
        const auto filled = [&](const std::uint8_t* p) {
            const int blue = p[0];
            const int green = p[1];
            const int red = p[2];
            switch (bar.fill) {
            case Fill::Red: return red > 140 && green < 120 && blue < 110 && red > 2 * green;
            case Fill::Blue: return blue > 180 && red < 90 && green < 170;
            // Lilac by its hue (blue well over green and red, red near green), so a darker or lighter screen keeps it
            // (its rows go from (255, 246, 246) to (198, 154, 155); only the lightest rows were over 225 and 200).
            case Fill::Lilac: return blue > 150 && blue >= green + 25 && blue >= red + 20 && red + 10 >= green;
            }
            return false;
        };
        // The first run of it from the left, so white letters over its empty part (a lilac bar's colour is close to
        // white) never count.
        const int label = bar.font >= 0 ? piece->x0 + bar.textFrom : x0;
        int end = 0;
        int gap = 0;
        bool starts = false;
        for (int x = x0; x < x1; ++x) {
            // A third of its rows of its colour at least (two or more): a fill is as tall as the bar, a lone pixel of a
            // like colour in its frame row is not (clip 2026-11-12_11-55-35, past an XP of 27%), nor its bottom frame's
            // two rows on a lighter screen (lilac there at 12% lighter, 2026-11-14 stress images).
            const int needed = std::max(2, (y1 - y0) / 3);
            int rows = 0;
            for (int y = y0; y < y1 && rows < needed; ++y) {
                rows += filled(at(x, y)) ? 1 : 0;
            }
            const bool on = rows >= needed;
            if (on) {
                end = x - x0 + 1;
                gap = 0;
                starts = starts || x - x0 < kFillStart;
            } else if (x - x0 >= kFillStart && !starts) {
                break;
            } else if (x >= label && !under(x) && ++gap > kFillGap) {
                break;
            }
        }
        if (!starts) {
            end = 0;
        }
        const auto width = static_cast<float>(x1 - x0);
        // Past its end the bar is its empty colour (along its middle row, leaving out the label, the number and other
        // white letters): else a window is over it, or its fill was not told by its colour, and how full it is cannot
        // be told. On the 664 images of 22 clips (2026-11-14) 98% and more of an empty part's columns were its colour,
        // under a window 38% and less.
        const auto empty = [&](const std::uint8_t* p) {
            const int blue = p[0];
            const int green = p[1];
            const int red = p[2];
            const int most = std::max({blue, green, red});
            switch (bar.empty) {
            case Empty::Dark: {
                // The bar's own colour may be lighter there (its dark empty blue reaches 122, clip
                // 2026-11-13_00-50-14), the others not.
                const int ownBlue = bar.fill == Fill::Red ? kEmptyMost : kEmptyOwnMost;
                const int ownRed = bar.fill == Fill::Blue ? kEmptyMost : kEmptyOwnMost;
                return blue <= ownBlue && green <= kEmptyMost && red <= ownRed;
            }
            case Empty::Red: return most <= kEmptyColoured && red >= green + 12 && red >= blue + 12;
            case Empty::Blue: return most <= kEmptyColoured && blue >= green + 15 && blue >= red + 25;
            case Empty::Grey: return most <= kEmptyColoured && most - std::min({blue, green, red}) <= 30;
            }
            return false;
        };
        int looked = 0;
        int light = 0;
        int leading = 0; // Columns not its empty colour right after the fill's end (a fill of a colour not told).
        bool leads = true;
        const int middle = (y0 + y1) / 2;
        for (int x = std::max(x0 + end + 2, label); x < x1; ++x) {
            if (!under(x)) {
                const std::uint8_t* p = at(x, middle);
                if (std::min({p[0], p[1], p[2]}) > kEmptyLetters) {
                    continue; // White letters (a name, a number not read).
                }
                ++looked;
                const bool other = !empty(p);
                light += other ? 1 : 0;
                leads = leads && other;
                leading += leads ? 1 : 0;
            }
        }
        // Only told from a few columns: a full bar's fill may end a column or two short of its light end cap.
        const bool covered = looked > kFillGap && (light * 10 > looked || leading > kFillGap);
        float full = covered ? -1.0f : static_cast<float>(end) / width;
        // A fill that ends under the number may end anywhere under it (its letters hide the bar there).
        const float most = !covered && textFrom < x1 && x0 + end >= textFrom - kFillGap && x0 + end <= textTo
                               ? static_cast<float>(textTo - x0) / width
                               : full;

        // A number that does not agree with its bar is not kept (only doubted): one of them was read wrong. One that
        // agrees says exactly how full the bar is.
        float share = 0.0f;
        if (gauge.text[0] != '\0') {
            if (fraction(gauge.text.data(), &share) && full >= 0.0f && share >= full - kAgree &&
                share <= most + kAgree) {
                full = share;
            } else {
                gauge.doubted = gauge.text;
                gauge.text[0] = '\0';
            }
        }
        // With no number agreeing, a fill that ends past where the number begins may end where something over the
        // number begins (a window across the bar's right part ends the fill just where it hides the number; the only
        // wrong bars on 729 stress images of 2026-11-14, and no numbered bar of the 664 clip images went unread): not
        // taken. Where the number began when last read (it is centred on the bar; a few letters' play as it grows),
        // before one was read where it may begin.
        if (bar.font >= 0) {
            const auto b = static_cast<std::size_t>(&bar - bars_.data());
            numberHalf_.resize(bars_.size(), -1);
            if (gauge.text[0] != '\0') {
                numberHalf_[b] = (x0 + x1) / 2 - textFrom;
            } else if (full > 0.0f) {
                const int from = numberHalf_[b] >= 0 ? (x0 + x1) / 2 - numberHalf_[b] - kNumberPlay : label;
                if (x0 + end > from) {
                    char line[160];
                    std::snprintf(line, sizeof(line), "%s %s: number not read, bar ends at %d%% past where it begins",
                                  bar.piece.data(), bar.name.data(), static_cast<int>(full * 100.0f + 0.5f));
                    because(line);
                    full = -1.0f;
                }
            }
        }
        gauge.full = full;
        gauge.seen = full >= 0.0f || gauge.text[0] != '\0';
    }
}

void StatsReader::readText(const Bar& bar, int px, int py, int x1, const ColourPart& part, std::array<char, 32>& text,
                           int* spanFrom, int* spanTo) {
    text[0] = '\0';
    const Font& font = fonts_[static_cast<std::size_t>(bar.font)];
    // The grey rows the glyphs may be on (a row up or down), columns from where the number may start.
    const int gx0 = std::max(px + bar.textFrom, part.area.x0);
    const int gx1 = std::min(x1 + 2, part.area.x1);
    const int gy0 = std::max(py + bar.textRow - 1, part.area.y0);
    const int gy1 = std::min(py + bar.textRow + font.height + 1, part.area.y1);
    const int gw = gx1 - gx0;
    const int gh = gy1 - gy0;
    if (gw < font.width || gh < font.height) {
        return;
    }
    grey_.resize(static_cast<std::size_t>(gw) * static_cast<std::size_t>(gh));
    for (int y = 0; y < gh; ++y) {
        const std::uint8_t* row = part.bgra + static_cast<std::size_t>(gy0 + y - part.area.y0) * part.pitch +
                                  static_cast<std::size_t>(gx0 - part.area.x0) * 4;
        for (int x = 0; x < gw; ++x) {
            grey_[static_cast<std::size_t>(y) * static_cast<std::size_t>(gw) + static_cast<std::size_t>(x)] =
                greyOf(row + static_cast<std::size_t>(x) * 4);
        }
    }

    // The brightest pixel of each column, to skip the places with no white letter (most of a bar).
    bright_.assign(static_cast<std::size_t>(gw), 0.0f);
    for (int y = 0; y < gh; ++y) {
        for (int x = 0; x < gw; ++x) {
            float& most = bright_[static_cast<std::size_t>(x)];
            most = std::max(most, grey_[static_cast<std::size_t>(y) * static_cast<std::size_t>(gw) +
                                        static_cast<std::size_t>(x)]);
        }
    }

    // The best glyph at each column (over the three rows).
    matches_.clear();
    for (int x = 0; x + font.width <= gw; ++x) {
        bool white = false;
        for (int c = x + font.letterFrom; c < x + font.letterTo && !white; ++c) {
            white = bright_[static_cast<std::size_t>(c)] >= kBright;
        }
        if (!white) {
            continue;
        }
        Match best;
        for (int y = 0; y + font.height <= gh; ++y) {
            const float* origin = grey_.data() + static_cast<std::size_t>(y) * static_cast<std::size_t>(gw) + x;
            for (std::size_t g = 0; g < font.glyphs.size(); ++g) {
                const Glyph& glyph = font.glyphs[g];
                float sum = 0.0f;
                float squares = 0.0f;
                float product = 0.0f;
                float letter = 0.0f;
                int letters = 0;
                for (const GlyphPixel& pixel : glyph.pixels) {
                    const float value = origin[static_cast<std::ptrdiff_t>(pixel.dy) * gw + pixel.dx];
                    sum += value;
                    squares += value * value;
                    product += value * pixel.centred;
                    if (pixel.letter) {
                        letter += value;
                        ++letters;
                    }
                }
                const auto n = static_cast<float>(glyph.pixels.size());
                const float variance = squares - sum * sum / n;
                if (variance <= 1.0f || letters == 0 || letters == static_cast<int>(glyph.pixels.size())) {
                    continue;
                }
                const float score = product / (std::sqrt(variance) * glyph.spread);
                const float letterMean = letter / static_cast<float>(letters);
                const float lineMean = (sum - letter) / (n - static_cast<float>(letters));
                if (score >= kMinMatch && score > best.score && letterMean - lineMean >= kMinContrast) {
                    best = {score, gx0 + x, static_cast<int>(g)};
                }
            }
        }
        if (best.score > 0.0f) {
            matches_.push_back(best);
        }
    }
    // The surest first; none closer than a glyph to another.
    std::sort(matches_.begin(), matches_.end(), [](const Match& a, const Match& b) { return a.score > b.score; });
    std::size_t kept = 0;
    for (std::size_t i = 0; i < matches_.size(); ++i) {
        bool apart = true;
        for (std::size_t j = 0; j < kept && apart; ++j) {
            apart = std::abs(matches_[i].x - matches_[j].x) >= font.pitch - 2;
        }
        if (apart) {
            matches_[kept++] = matches_[i];
        }
    }
    matches_.resize(kept);
    std::sort(matches_.begin(), matches_.end(), [](const Match& a, const Match& b) { return a.x < b.x; });
    // The number: the longest run of glyphs a pitch apart (anything else, e.g. a letter of the bar's label taken for a
    // glyph, is left out); of two as long, the surer.
    std::size_t bestFrom = 0;
    std::size_t bestCount = 0;
    float bestScore = 0.0f;
    for (std::size_t from = 0; from < matches_.size();) {
        std::size_t to = from + 1;
        float score = matches_[from].score;
        while (to < matches_.size() && std::abs(matches_[to].x - matches_[to - 1].x - font.pitch) <= 1) {
            score += matches_[to].score;
            ++to;
        }
        if (to - from > bestCount || (to - from == bestCount && score > bestScore)) {
            bestFrom = from;
            bestCount = to - from;
            bestScore = score;
        }
        from = to;
    }
    if (bestCount == 0 || bestCount >= text.size()) {
        return;
    }
    for (std::size_t i = 0; i < bestCount; ++i) {
        text[i] = font.glyphs[static_cast<std::size_t>(matches_[bestFrom + i].glyph)].c;
    }
    text[bestCount] = '\0';
    // Its columns: its letters and the dark line round them.
    *spanFrom = matches_[bestFrom].x + font.letterFrom - 1;
    *spanTo = matches_[bestFrom + bestCount - 1].x + font.letterTo + 1;
}

void StatsReader::findSkills(std::span<const UiPiece> pieces, std::span<const ColourPart> parts) {
    found_.clear();
    if (skills_.empty() || skillsPiece_[0] == '\0') {
        return;
    }
    const auto holding = [&](const StatsArea& box) -> const ColourPart* {
        for (const ColourPart& part : parts) {
            if (part.area.x0 <= box.x0 - 1 && part.area.y0 <= box.y0 - 1 && part.area.x1 >= box.x1 + 1 &&
                part.area.y1 >= box.y1 + 1) {
                return &part;
            }
        }
        return nullptr;
    };
    char line[160];

    // Every image: each where it was last seen (a pixel either way), also while it was not there the image before.
    bool missing = false;
    for (std::size_t s = 0; s < skills_.size(); ++s) {
        StatsArea& at = skillAt_[s];
        const Skill& skill = skills_[s];
        if (at.x1 <= at.x0) {
            missing = true;
            because(skill.name + ": not seen yet");
            continue;
        }
        const ColourPart* part = holding(at);
        if (part == nullptr) {
            // Its box was not read (the hotbar's piece not found this image): kept, looked for again.
            missing = true;
            std::snprintf(line, sizeof(line), "%s: hotbar not found (last at %d,%d)", skill.name.c_str(), at.x0,
                          at.y0);
            because(line);
            continue;
        }
        StatsArea best = at;
        const float far = distance(skill, at, *part, &best);
        // Near in its pixels, and its shapes there too, in a miniature's light edge: a dark smooth place is near a
        // darkened picture in its pixels alone (where the hotbar was before it was moved, 2026-11-14 stress images).
        StatsArea shapeAt = best;
        const float shapes = far <= kSkillMatch ? alike(skill, best, *part, &shapeAt) : -1.0f;
        if (far <= kSkillMatch && shapes >= kFoundAlike && framed(best, *part)) {
            at = best;
            seenOn_[s] = reads_;
            found_.push_back({best, part, static_cast<int>(s), far, false});
            continue;
        }
        // Not ready nor darkened there, but its shapes are, in a miniature's edge: its key was just pressed (a flash
        // over it), or it is cooling darkened more or less than kDarkened (another brightness): then its yellow
        // seconds are there.
        StatsArea alikeBest = at;
        const float likeness = alike(skill, at, *part, &alikeBest);
        const bool recent = seenOn_[s] != 0 && reads_ - seenOn_[s] <= kPressedHold;
        const bool edged = framed(alikeBest, *part);
        if (edged && (likeness >= kPressedAlike || (recent && likeness >= kPressedAlikeHeld))) {
            std::array<char, 32> seconds{};
            if (skill.cooldown) {
                readSeconds(alikeBest, *part, seconds);
            }
            at = alikeBest;
            found_.push_back({alikeBest, part, static_cast<int>(s), far, seconds[0] == '\0'});
            continue;
        }
        missing = true;
        char farText[16] = ">22";
        if (far < 1e8f) {
            std::snprintf(farText, sizeof(farText), "%.1f", static_cast<double>(far));
        }
        std::snprintf(line, sizeof(line), "%s: not at %d,%d (far %s, alike %.2f%s%s)", skill.name.c_str(), at.x0,
                      at.y0, farText, static_cast<double>(std::max(likeness, shapes)), edged ? "" : ", no miniature edge",
                      recent ? ", seen just before" : "");
        because(line);
    }
    // One missing: the hotbar is looked over now and then (its pieces are read whole on the next image).
    if (!missing) {
        lookOver_ = false;
        return;
    }
    if (!lookOver_) {
        lookOver_ = reads_ - lookedOver_ >= kLookOverEvery;
        return;
    }

    // The pieces looked over: every miniature in them, each missing skill to its nearest one not taken, the nearest
    // pairs first.
    lookOver_ = false;
    lookedOver_ = reads_;
    struct Mini {
        StatsArea box;
        const ColourPart* part;
    };
    std::vector<Mini> minis;
    int piecesSeen = 0;
    for (const UiPiece& piece : pieces) {
        if (std::strcmp(piece.name.data(), skillsPiece_.data()) != 0) {
            continue;
        }
        const auto part = std::find_if(parts.begin(), parts.end(), [&](const ColourPart& p) {
            return p.area.x0 <= piece.x0 && p.area.x1 > piece.x0 && p.area.y0 <= piece.y0 && p.area.y1 >= piece.y1;
        });
        if (part == parts.end()) {
            continue;
        }
        ++piecesSeen;
        const int x0 = std::max(piece.x0, part->area.x0);
        const int y0 = std::max(piece.y0, part->area.y0);
        const int x1 = std::min(piece.x1, part->area.x1);
        const int y1 = std::min(piece.y1, part->area.y1);
        const int w = x1 - x0;
        const int h = y1 - y0;
        if (w <= 0 || h <= 0) {
            continue;
        }
        pieceGrey_.resize(static_cast<std::size_t>(w) * static_cast<std::size_t>(h));
        for (int y = 0; y < h; ++y) {
            const std::uint8_t* row = part->bgra + static_cast<std::size_t>(y0 + y - part->area.y0) * part->pitch +
                                      static_cast<std::size_t>(x0 - part->area.x0) * 4;
            std::uint8_t* to = pieceGrey_.data() + static_cast<std::size_t>(y) * static_cast<std::size_t>(w);
            for (int x = 0; x < w; ++x) {
                to[x] = static_cast<std::uint8_t>(greyOf(row + static_cast<std::size_t>(x) * 4));
            }
        }
        std::array<StatsArea, 64> boxes{};
        const int count = findMiniatures(pieceGrey_.data(), w, w, h, {0, 0, w, h}, boxes.data(),
                                         static_cast<int>(boxes.size()));
        for (int i = 0; i < count; ++i) {
            const StatsArea& box = boxes[static_cast<std::size_t>(i)];
            minis.push_back({{box.x0 + x0, box.y0 + y0, box.x1 + x0, box.y1 + y0}, &*part});
        }
    }
    // Those found where they were take their miniatures.
    std::vector<std::uint8_t> taken(minis.size(), 0);
    std::vector<std::uint8_t> isFound(skills_.size(), 0);
    for (const Found& found : found_) {
        isFound[static_cast<std::size_t>(found.skill)] = 1;
        for (std::size_t m = 0; m < minis.size(); ++m) {
            const StatsArea& box = minis[m].box;
            if (std::abs(box.x0 - found.box.x0) <= 3 && std::abs(box.y0 - found.box.y0) <= 3) {
                taken[m] = 1;
            }
        }
    }
    struct Pair {
        float far;
        int skill;
        int mini;
        StatsArea box;
    };
    std::vector<Pair> pairs;
    std::vector<float> nearest(skills_.size(), 1e9f);
    for (std::size_t s = 0; s < skills_.size(); ++s) {
        if (isFound[s] != 0) {
            continue;
        }
        for (std::size_t m = 0; m < minis.size(); ++m) {
            if (taken[m] != 0 || std::abs((minis[m].box.x1 - minis[m].box.x0) - skills_[s].side) > kSideSlack) {
                continue;
            }
            StatsArea best;
            const float far = distance(skills_[s], minis[m].box, *minis[m].part, &best);
            nearest[s] = std::min(nearest[s], far);
            StatsArea shapeAt = best;
            if (far <= kSkillMatch && alike(skills_[s], best, *minis[m].part, &shapeAt) >= kFoundAlike &&
                framed(best, *minis[m].part)) {
                pairs.push_back({far, static_cast<int>(s), static_cast<int>(m), best});
            }
        }
    }
    std::sort(pairs.begin(), pairs.end(), [](const Pair& a, const Pair& b) { return a.far < b.far; });
    for (const Pair& pair : pairs) {
        const auto s = static_cast<std::size_t>(pair.skill);
        if (isFound[s] != 0 || taken[static_cast<std::size_t>(pair.mini)] != 0) {
            continue;
        }
        skillAt_[s] = pair.box;
        seenOn_[s] = reads_;
        isFound[s] = 1;
        taken[static_cast<std::size_t>(pair.mini)] = 1;
        found_.push_back({pair.box, minis[static_cast<std::size_t>(pair.mini)].part, pair.skill, pair.far, false});
    }
    // What the look-over saw, for those still missing.
    for (std::size_t s = 0; s < skills_.size(); ++s) {
        if (isFound[s] != 0) {
            continue;
        }
        if (nearest[s] < 1e8f) {
            std::snprintf(line, sizeof(line), "%s: hotbar looked over (%d pieces, %d miniatures), nearest %.1f",
                          skills_[s].name.c_str(), piecesSeen, static_cast<int>(minis.size()),
                          static_cast<double>(nearest[s]));
        } else {
            std::snprintf(line, sizeof(line), "%s: hotbar looked over (%d pieces, %d miniatures), none its size",
                          skills_[s].name.c_str(), piecesSeen, static_cast<int>(minis.size()));
        }
        because(line);
    }
}

bool StatsReader::closeShown(const Close& close, const UiPiece& piece, std::span<const ColourPart> parts) const {
    const int x0 = piece.x1 + close.left;
    const int x1 = piece.x1 + close.right;
    const int y0 = piece.y0 + close.top;
    const int y1 = piece.y0 + close.bottom;
    const auto part = std::find_if(parts.begin(), parts.end(), [&](const ColourPart& p) {
        return p.area.x0 <= x0 - kCloseShift && p.area.x1 >= x1 + kCloseShift && p.area.y0 <= y0 - kCloseShift &&
               p.area.y1 >= y1 + kCloseShift;
    });
    if (part == parts.end()) {
        return false;
    }
    const int w = x1 - x0;
    const int h = y1 - y0;
    const float cx = static_cast<float>(w - 1) * 0.5f;
    const float cy = static_cast<float>(h - 1) * 0.5f;
    float best = -1.0f;
    for (int sy = -kCloseShift; sy <= kCloseShift; ++sy) {
        for (int sx = -kCloseShift; sx <= kCloseShift; ++sx) {
            int band = 0;
            int bandLight = 0;
            int off = 0;
            int offLight = 0;
            for (int y = 0; y < h; ++y) {
                const std::uint8_t* row = part->bgra +
                                          static_cast<std::size_t>(y0 + sy + y - part->area.y0) * part->pitch +
                                          static_cast<std::size_t>(x0 + sx - part->area.x0) * 4;
                for (int x = 0; x < w; ++x) {
                    const std::uint8_t* p = row + static_cast<std::size_t>(x) * 4;
                    const int light = std::min({p[0], p[1], p[2]}) >= kCloseLight ? 1 : 0;
                    const float across = std::abs(std::abs(static_cast<float>(x) - cx) - std::abs(static_cast<float>(y) - cy));
                    if (across <= 1.5f) {
                        ++band;
                        bandLight += light;
                    } else {
                        ++off;
                        offLight += light;
                    }
                }
            }
            if (band > 0 && off > 0) {
                best = std::max(best, static_cast<float>(bandLight) / static_cast<float>(band) -
                                          static_cast<float>(offLight) / static_cast<float>(off));
            }
        }
    }
    return best >= kCloseScore;
}

bool StatsReader::framed(StatsArea box, const ColourPart& part) const {
    // At the box or a pixel either way (as the skill is compared: a pressed one's best likeness may be a pixel off).
    for (int dy = -1; dy <= 1; ++dy) {
        for (int dx = -1; dx <= 1; ++dx) {
            const int x0 = box.x0 + dx;
            const int y0 = box.y0 + dy;
            const int x1 = box.x1 + dx;
            const int y1 = box.y1 + dy;
            if (x0 < part.area.x0 || y0 < part.area.y0 || x1 > part.area.x1 || y1 > part.area.y1 || x1 - x0 < 3 ||
                y1 - y0 < 3) {
                continue;
            }
            int light = 0;
            int all = 0;
            const auto look = [&](int x, int y) {
                const std::uint8_t* p = part.bgra + static_cast<std::size_t>(y - part.area.y0) * part.pitch +
                                        static_cast<std::size_t>(x - part.area.x0) * 4;
                ++all;
                light += std::min({p[0], p[1], p[2]}) >= kEdgeLight ? 1 : 0;
            };
            for (int x = x0; x < x1; ++x) {
                look(x, y0);
                look(x, y1 - 1);
            }
            for (int y = y0 + 1; y < y1 - 1; ++y) {
                look(x0, y);
                look(x1 - 1, y);
            }
            if (light * 2 >= all) {
                return true;
            }
        }
    }
    return false;
}

float StatsReader::alike(const Skill& skill, StatsArea box, const ColourPart& part, StatsArea* best) const {
    float most = -1.0f;
    if (skill.spread <= 0.0f) {
        return most;
    }
    for (int dy = -1; dy <= 1; ++dy) {
        for (int dx = -1; dx <= 1; ++dx) {
            const int x0 = box.x0 + dx;
            const int y0 = box.y0 + dy;
            if (x0 < part.area.x0 || y0 < part.area.y0 || x0 + skill.side > part.area.x1 ||
                y0 + skill.side > part.area.y1) {
                continue;
            }
            // Correlation over the compared pixels away from the edge: sum(c * s) / (|c| |s - mean s|), as the
            // skill's c sum to 0.
            double sum = 0.0;
            double seen = 0.0;
            double seenSquares = 0.0;
            int count = 0;
            for (int y = kAlikeEdge; y < skill.side - kAlikeEdge; ++y) {
                const std::uint8_t* row = part.bgra + static_cast<std::size_t>(y0 + y - part.area.y0) * part.pitch +
                                          static_cast<std::size_t>(x0 - part.area.x0) * 4;
                const std::size_t at = static_cast<std::size_t>(y) * static_cast<std::size_t>(skill.side);
                for (int x = kAlikeEdge; x < skill.side - kAlikeEdge; ++x) {
                    if (skill.use[at + static_cast<std::size_t>(x)] == 0) {
                        continue;
                    }
                    const float grey = greyOf(row + static_cast<std::size_t>(x) * 4);
                    sum += static_cast<double>(skill.centred[at + static_cast<std::size_t>(x)]) * grey;
                    seen += grey;
                    seenSquares += static_cast<double>(grey) * grey;
                    ++count;
                }
            }
            const double variance = count > 0 ? seenSquares - seen * seen / count : 0.0;
            if (variance <= 1e-6) {
                continue;
            }
            const auto r = static_cast<float>(sum / (static_cast<double>(skill.spread) * std::sqrt(variance)));
            if (r > most) {
                most = r;
                *best = {x0, y0, x0 + skill.side, y0 + skill.side};
            }
        }
    }
    return most;
}

void StatsReader::because(std::string_view text) {
    if (!why_.empty()) {
        why_ += " | ";
    }
    why_ += text;
}

float StatsReader::distance(const Skill& skill, StatsArea box, const ColourPart& part, StatsArea* best) const {
    float nearest = 1e9f;
    for (int dy = -1; dy <= 1; ++dy) {
        for (int dx = -1; dx <= 1; ++dx) {
            const int x0 = box.x0 + dx;
            const int y0 = box.y0 + dy;
            if (x0 < part.area.x0 || y0 < part.area.y0 || x0 + skill.side > part.area.x1 ||
                y0 + skill.side > part.area.y1) {
                continue;
            }
            // Given up on as soon as it cannot be nearer than the nearest so far.
            const float limit = std::min(nearest, kSkillMatch) * static_cast<float>(skill.uses);
            float sum = 0.0f;
            int y = 0;
            for (; y < skill.side && sum <= limit; ++y) {
                const std::uint8_t* row = part.bgra + static_cast<std::size_t>(y0 + y - part.area.y0) * part.pitch +
                                          static_cast<std::size_t>(x0 - part.area.x0) * 4;
                const std::size_t at = static_cast<std::size_t>(y) * static_cast<std::size_t>(skill.side);
                for (int x = 0; x < skill.side; ++x) {
                    if (skill.use[at + static_cast<std::size_t>(x)] == 0) {
                        continue;
                    }
                    const float seen = greyOf(row + static_cast<std::size_t>(x) * 4);
                    const float ready = skill.pixels[at + static_cast<std::size_t>(x)];
                    sum += std::min(std::abs(seen - ready), std::abs(seen - (kDarkened * ready + kDarkenedPlus)));
                }
            }
            if (y == skill.side && sum <= limit) {
                const float far = sum / static_cast<float>(skill.uses);
                if (far < nearest) {
                    nearest = far;
                    *best = {x0, y0, x0 + skill.side, y0 + skill.side};
                }
            }
        }
    }
    return nearest;
}

float StatsReader::cooldownLeft(const Skill& skill, StatsArea box, const ColourPart& part) {
    sectorBright_.assign(kSectors, 0.0f);
    sectorAll_.assign(kSectors, 0.0f);
    int told = 0;
    int bright = 0;
    for (int y = 0; y < skill.side; ++y) {
        const std::uint8_t* row = part.bgra + static_cast<std::size_t>(box.y0 + y - part.area.y0) * part.pitch +
                                  static_cast<std::size_t>(box.x0 - part.area.x0) * 4;
        const std::size_t at = static_cast<std::size_t>(y) * static_cast<std::size_t>(skill.side);
        for (int x = 0; x < skill.side; ++x) {
            const std::uint8_t sector = skill.sector[at + static_cast<std::size_t>(x)];
            if (sector >= kSectors) {
                continue; // Not told apart, or not compared.
            }
            const float seen = greyOf(row + static_cast<std::size_t>(x) * 4);
            const float ready = skill.pixels[at + static_cast<std::size_t>(x)];
            const bool on = std::abs(seen - ready) < std::abs(seen - (kDarkened * ready + kDarkenedPlus));
            sectorAll_[sector] += 1.0f;
            sectorBright_[sector] += on ? 1.0f : 0.0f;
            ++told;
            bright += on ? 1 : 0;
        }
    }
    if (told == 0 || static_cast<float>(bright) >= kReadyShare * static_cast<float>(told)) {
        return 0.0f;
    }
    // The bright sector grows from 12 o'clock clockwise: it ends at the first sector less than half bright.
    int passed = 0;
    while (passed < kSectors &&
           (sectorAll_[static_cast<std::size_t>(passed)] == 0.0f ||
            sectorBright_[static_cast<std::size_t>(passed)] * 2.0f >= sectorAll_[static_cast<std::size_t>(passed)])) {
        ++passed;
    }
    return std::max(1.0f - static_cast<float>(passed) / static_cast<float>(kSectors), 0.5f / kSectors);
}

void StatsReader::readSeconds(StatsArea box, const ColourPart& part, std::array<char, 32>& text) {
    text[0] = '\0';
    if (skillsFont_ < 0) {
        return;
    }
    const Font& font = fonts_[static_cast<std::size_t>(skillsFont_)];
    const int side = box.x1 - box.x0;
    if (side < font.width || side < font.height || side > 64) {
        return;
    }
    // Which of the miniature's pixels are yellow: each row as bits (column x: bit x), so a glyph's overlap with them
    // is a few bit counts.
    yellowRows_.assign(static_cast<std::size_t>(side), 0);
    bool any = false;
    for (int y = 0; y < side; ++y) {
        const std::uint8_t* row = part.bgra + static_cast<std::size_t>(box.y0 + y - part.area.y0) * part.pitch +
                                  static_cast<std::size_t>(box.x0 - part.area.x0) * 4;
        std::uint64_t bits = 0;
        for (int x = 0; x < side; ++x) {
            bits |= yellow(row + static_cast<std::size_t>(x) * 4) ? std::uint64_t{1} << x : 0;
        }
        yellowRows_[static_cast<std::size_t>(y)] = bits;
        any = any || bits != 0;
    }
    if (!any) {
        return; // No yellow at all: nothing written.
    }
    // The best glyph at each column, on the rows round the middle (the seconds are written there).
    const float middle = static_cast<float>(side) * 0.5f;
    const int fromY = std::max(static_cast<int>(middle - kSecondsHeight * static_cast<float>(side)) - 2, 0);
    const int toY = std::min(static_cast<int>(middle + kSecondsHeight * static_cast<float>(side)) + 2, side) -
                    font.height;
    const std::uint64_t window = font.width >= 64 ? ~std::uint64_t{0} : (std::uint64_t{1} << font.width) - 1;
    matches_.clear();
    for (int x = 0; x + font.width <= side; ++x) {
        Match best;
        for (int y = fromY; y <= toY; ++y) {
            int there = 0;
            for (int gy = 0; gy < font.height; ++gy) {
                there += std::popcount((yellowRows_[static_cast<std::size_t>(y + gy)] >> x) & window);
            }
            for (std::size_t g = 0; g < font.glyphs.size(); ++g) {
                const Glyph& glyph = font.glyphs[g];
                if (static_cast<float>(there) * 2.0f < static_cast<float>(glyph.ons) * kMinYellowMatch) {
                    continue; // Too few to overlap enough.
                }
                int overlap = 0;
                for (int gy = 0; gy < font.height; ++gy) {
                    overlap += std::popcount(glyph.rows[static_cast<std::size_t>(gy)] &
                                             (yellowRows_[static_cast<std::size_t>(y + gy)] >> x));
                }
                const float dice =
                    2.0f * static_cast<float>(overlap) / static_cast<float>(glyph.ons + there);
                if (dice >= kMinYellowMatch && dice > best.score) {
                    best = {dice, x, static_cast<int>(g)};
                }
            }
        }
        if (best.score > 0.0f) {
            matches_.push_back(best);
        }
    }
    // The surest first, none closer than a glyph to another; then left to right, the longest run a pitch apart.
    std::sort(matches_.begin(), matches_.end(), [](const Match& a, const Match& b) { return a.score > b.score; });
    std::size_t kept = 0;
    for (std::size_t i = 0; i < matches_.size(); ++i) {
        bool apart = true;
        for (std::size_t j = 0; j < kept && apart; ++j) {
            apart = std::abs(matches_[i].x - matches_[j].x) >= font.pitch - 2;
        }
        if (apart) {
            matches_[kept++] = matches_[i];
        }
    }
    matches_.resize(kept);
    std::sort(matches_.begin(), matches_.end(), [](const Match& a, const Match& b) { return a.x < b.x; });
    std::size_t bestFrom = 0;
    std::size_t bestCount = 0;
    float bestScore = 0.0f;
    for (std::size_t from = 0; from < matches_.size();) {
        std::size_t to = from + 1;
        float score = matches_[from].score;
        while (to < matches_.size() && std::abs(matches_[to].x - matches_[to - 1].x - font.pitch) <= 2) {
            score += matches_[to].score;
            ++to;
        }
        if (to - from > bestCount || (to - from == bestCount && score > bestScore)) {
            bestFrom = from;
            bestCount = to - from;
            bestScore = score;
        }
        from = to;
    }
    if (bestCount == 0 || bestCount >= text.size()) {
        return;
    }
    for (std::size_t i = 0; i < bestCount; ++i) {
        text[i] = font.glyphs[static_cast<std::size_t>(matches_[bestFrom + i].glyph)].c;
    }
    text[bestCount] = '\0';
}

float StatsReader::readCast(const UiPiece& piece, const ColourPart& part) const {
    const int x0 = std::max(piece.x0 + cast_.left - 2, part.area.x0);
    const int x1 = std::min(piece.x0 + cast_.right + 3, part.area.x1);
    const int y0 = std::max(piece.y0 + cast_.top, part.area.y0);
    const int y1 = std::min(piece.y0 + cast_.bottom, part.area.y1);
    const auto at = [&](int x, int y) {
        return part.bgra + static_cast<std::size_t>(y - part.area.y0) * part.pitch +
               static_cast<std::size_t>(x - part.area.x0) * 4;
    };
    // The fill's light blue from the bar's left (its run may break under the skill's name written over it).
    int end = 0;
    int gap = 0;
    for (int x = x0; x < x1; ++x) {
        bool on = false;
        for (int y = y0; y < y1 && !on; ++y) {
            const std::uint8_t* p = at(x, y);
            on = p[0] > 200 && p[1] > 170 && p[2] < 170;
        }
        if (on) {
            end = x - x0 + 1;
            gap = 0;
        } else if (end == 0 ? x - x0 >= kCastStart : ++gap > kCastGap) {
            break;
        }
    }
    if (end == 0) {
        return 0.0f;
    }
    // Its head: the white glow, the column with the most red, at most a few columns past where the blue ends.
    int head = x0;
    int most = -1;
    for (int x = x0; x < std::min(x0 + end + 4, x1); ++x) {
        int red = 0;
        for (int y = y0; y < y1; ++y) {
            red += at(x, y)[2];
        }
        if (red > most) {
            most = red;
            head = x;
        }
    }
    const float share = static_cast<float>(head - (piece.x0 + cast_.left)) /
                        static_cast<float>(cast_.right - cast_.left);
    return std::clamp(share, 0.0f, 1.0f);
}

void StatsReader::readWords(const Words& words, const UiPiece& piece, const ColourPart& part,
                            std::array<char, 32>& text) {
    text[0] = '\0';
    const int x0 = std::max(piece.x0 + words.left, part.area.x0);
    const int x1 = std::min(words.right < 1 ? piece.x1 + words.right : piece.x0 + words.right, part.area.x1);
    const int y0 = std::max(piece.y0 + words.top, part.area.y0);
    const int y1 = std::min(piece.y0 + words.bottom, part.area.y1);
    if (x1 - x0 < 4 || y1 - y0 < 4) {
        return;
    }
    const auto lightest = [&](int x, int y) {
        const std::uint8_t* p = part.bgra + static_cast<std::size_t>(y - part.area.y0) * part.pitch +
                                static_cast<std::size_t>(x - part.area.x0) * 4;
        return std::min({p[0], p[1], p[2]}); // White letters: all three high.
    };
    // The words: the widest run of columns with white in them, gaps of a space or less joined.
    int bestFrom = 0;
    int bestTo = 0;
    int from = -1;
    int last = -1;
    for (int x = x0; x <= x1; ++x) {
        bool white = false;
        for (int y = y0; y < y1 && x < x1 && !white; ++y) {
            white = lightest(x, y) >= kWordsWhite;
        }
        if (white) {
            if (from < 0 || x - last > kWordsGap) {
                from = x;
            }
            last = x;
            if (last + 1 - from > bestTo - bestFrom) {
                bestFrom = from;
                bestTo = last + 1;
            }
        }
    }
    if (bestTo - bestFrom < kWordsLeast) {
        return;
    }
    const int cx0 = std::max(bestFrom - 2, x0);
    const int cx1 = std::min(bestTo + 2, x1);
    // Its white pixels. The same words as on the image before (a few pixels differ at the letters' edges, or the sky
    // behind changed): what they said then, nothing read.
    const int mw = cx1 - cx0;
    wordsWidth_ = mw;
    const int mh = y1 - y0;
    wordsMask_.resize(static_cast<std::size_t>(mw) * static_cast<std::size_t>(mh));
    int whites = 0;
    for (int y = 0; y < mh; ++y) {
        for (int x = 0; x < mw; ++x) {
            const bool white = lightest(cx0 + x, y0 + y) >= kWordsWhite;
            wordsMask_[static_cast<std::size_t>(y * mw + x)] = white ? 1 : 0;
            whites += white ? 1 : 0;
        }
    }
    LastWords& before = lastWords_[static_cast<std::size_t>(&words - words_.data())];
    if (before.height == mh && std::abs(before.width - mw) <= kWordsShift && !before.said.empty()) {
        int fewest = whites + 1;
        for (int dx = -kWordsShift; dx <= kWordsShift; ++dx) {
            int differ = 0;
            for (int y = 0; y < mh; ++y) {
                for (int x = 0; x < mw; ++x) {
                    const int bx = x + dx;
                    const std::uint8_t then = bx >= 0 && bx < before.width
                                                  ? before.mask[static_cast<std::size_t>(y * before.width + bx)]
                                                  : 0;
                    differ += then != wordsMask_[static_cast<std::size_t>(y * mw + x)] ? 1 : 0;
                }
            }
            fewest = std::min(fewest, differ);
        }
        if (static_cast<float>(fewest) <= kWordsSame * static_cast<float>(std::max(whites, 1))) {
            copyName(text, before.said);
            return;
        }
    }
    const auto remember = [&](const std::string& said) {
        before.mask = wordsMask_;
        before.width = mw;
        before.height = mh;
        before.said = said;
    };
    // Else read once per picture: the white pixels' hash says whether it was read before (a target met again).
    std::uint64_t hash = 1469598103934665603ull ^ static_cast<std::uint64_t>(mw);
    for (const std::uint8_t white : wordsMask_) {
        hash = (hash ^ white) * 1099511628211ull;
    }
    for (const auto& [known, said] : wordsRead_) {
        if (known == hash) {
            remember(said);
            copyName(text, said);
            return;
        }
    }
    // A new one: only the rows the letters are on, black on white (whatever is behind them white too), 4 times as
    // large with a margin, as Windows' OCR reads best.
    int ry0 = y1;
    int ry1 = y0;
    for (int y = y0; y < y1; ++y) {
        for (int x = cx0; x < cx1; ++x) {
            if (lightest(x, y) >= kWordsWhite) {
                ry0 = std::min(ry0, y);
                ry1 = std::max(ry1, y + 1);
                break;
            }
        }
    }
    ry0 = std::max(ry0 - 2, y0);
    ry1 = std::min(ry1 + 2, y1);
    // Drawn three ways, as Windows' OCR misreads each now and then (a "W" as "VV", or nothing on a plain "Imp"): 4 times
    // as large in blocks, black from kWordsPaper to kWordsBlack; smoothly 4 and 3 times as large, black between this
    // picture's own ground and letters (a darker or lighter screen). Two that agree are taken; else two more drawings
    // (smoothly 4 times as large, black from lower in the picture's own ground-to-letters span; 3 times as large in
    // blocks) and all five vote: the most read, two at least, and a name two drawings agreed on before wins a tie (on
    // 185 names in 5 brightnesses, 2026-11-14: 181 read right by three, 185 by five; 157 drawn the first way alone).
    const int cw = cx1 - cx0;
    const int ch = ry1 - ry0;
    std::array<int, 256> letters{};
    std::array<int, 256> ground{};
    int letterCount = 0;
    for (int y = ry0; y < ry1; ++y) {
        for (int x = cx0; x < cx1; ++x) {
            const int v = lightest(x, y);
            if (v >= kWordsWhite) {
                ++letters[static_cast<std::size_t>(v)];
                ++letterCount;
            } else {
                ++ground[static_cast<std::size_t>(v)];
            }
        }
    }
    const auto atShare = [](const std::array<int, 256>& counts, int total, float share) {
        int seen = 0;
        for (int v = 0; v < 256; ++v) {
            seen += counts[static_cast<std::size_t>(v)];
            if (static_cast<float>(seen) >= share * static_cast<float>(total)) {
                return v;
            }
        }
        return 255;
    };
    const int top = letterCount > 0 ? atShare(letters, letterCount, 0.95f) : 255;
    const int groundCount = cw * ch - letterCount;
    const int below = groundCount > 0 ? atShare(ground, groundCount, 0.5f) : 40;
    const int ownPaper = below + (top - below) * 60 / 100;
    const int ownBlack = std::max(below + (top - below) * 85 / 100, ownPaper + 1);
    const auto draw = [&](int scale, bool smooth, int paper, int black) {
        constexpr int kMargin = 12;
        const int w = cw * scale + 2 * kMargin;
        const int h = ch * scale + 2 * kMargin;
        pieceGrey_.assign(static_cast<std::size_t>(w) * static_cast<std::size_t>(h), 255);
        for (int y = 0; y < ch * scale; ++y) {
            for (int x = 0; x < cw * scale; ++x) {
                float v = 0.0f;
                if (smooth) {
                    // As a bilinear resize: the source point under this pixel's middle, its four neighbours mixed.
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
                    const auto l = [&](int cx, int cy) { return static_cast<float>(lightest(cx0 + cx, ry0 + cy)); };
                    v = (l(u0, s0) * (1.0f - fu) + l(u1, s0) * fu) * (1.0f - fs) +
                        (l(u0, s1) * (1.0f - fu) + l(u1, s1) * fu) * fs;
                } else {
                    v = static_cast<float>(lightest(cx0 + x / scale, ry0 + y / scale));
                }
                const float white =
                    std::clamp((v - static_cast<float>(paper)) * 255.0f / static_cast<float>(black - paper), 0.0f,
                               255.0f);
                pieceGrey_[static_cast<std::size_t>((y + kMargin) * w + x + kMargin)] =
                    static_cast<std::uint8_t>(255.0f - white);
            }
        }
        return ocr_.read(pieceGrey_.data(), w, h, w, &ocrError_);
    };
    const std::string blocks = draw(4, false, kWordsPaper, kWordsBlack);
    const std::string smooth = draw(4, true, ownPaper, ownBlack);
    std::string said;
    bool agree = !blocks.empty() && blocks == smooth;
    if (agree) {
        said = blocks;
    } else {
        const std::string small = draw(3, true, ownPaper, ownBlack);
        std::string softer;
        std::string coarse;
        if (small.empty() || (small != blocks && small != smooth)) {
            softer = draw(4, true, below + (top - below) * 45 / 100, below + (top - below) * 75 / 100);
            coarse = draw(3, false, kWordsPaper, kWordsBlack);
        }
        const std::array<const std::string*, 5> reads{&blocks, &smooth, &small, &softer, &coarse};
        const auto known = [&](const std::string& one) {
            return std::find(namesAgreed_.begin(), namesAgreed_.end(), one) != namesAgreed_.end();
        };
        int most = 1;
        for (const std::string* one : reads) {
            const auto votes = static_cast<int>(
                std::count_if(reads.begin(), reads.end(), [&](const std::string* other) { return *other == *one; }));
            if (!one->empty() && (votes > most || (votes == most && agree && !known(said) && known(*one)))) {
                said = *one;
                most = votes;
                agree = true;
            }
        }
        if (!agree) {
            // No two agree: one of them agreed on before, else the first read; said in the why.
            const auto earlier = std::find_if(reads.begin(), reads.end(), [&](const std::string* one) {
                return !one->empty() && known(*one);
            });
            const auto first =
                std::find_if(reads.begin(), reads.end(), [](const std::string* one) { return !one->empty(); });
            said = earlier != reads.end() ? **earlier : first != reads.end() ? **first : std::string();
            char line[240];
            std::snprintf(line, sizeof(line), "%s: read '%s' '%s' '%s' '%s' '%s', none agree, took '%s'",
                          words.name.data(), blocks.c_str(), smooth.c_str(), small.c_str(), softer.c_str(),
                          coarse.c_str(), said.c_str());
            because(line);
        }
    }
    if (agree && std::find(namesAgreed_.begin(), namesAgreed_.end(), said) == namesAgreed_.end()) {
        if (namesAgreed_.size() >= kWordsKept) {
            namesAgreed_.erase(namesAgreed_.begin());
        }
        namesAgreed_.push_back(said);
    }
    if (wordsRead_.size() >= kWordsKept) {
        wordsRead_.erase(wordsRead_.begin());
    }
    wordsRead_.emplace_back(hash, said);
    remember(said);
    copyName(text, said);
}

bool StatsReader::fraction(std::string_view text, float* share) {
    const std::size_t slash = text.find('/');
    if (slash != std::string_view::npos) {
        int value = 0;
        int most = 0;
        if (!toInt(text.substr(0, slash), value) || !toInt(text.substr(slash + 1), most) || most <= 0 ||
            value < 0 || value > most) {
            return false;
        }
        *share = static_cast<float>(value) / static_cast<float>(most);
        return true;
    }
    if (text.size() >= 2 && text.back() == '%') {
        const std::string_view number = text.substr(0, text.size() - 1);
        const std::size_t dot = number.find('.');
        int whole = 0;
        int part = 0;
        if (dot == std::string_view::npos || dot == 0 || number.size() - dot != 3 ||
            !toInt(number.substr(0, dot), whole) || !toInt(number.substr(dot + 1), part) || whole > 100) {
            return false;
        }
        *share = (static_cast<float>(whole) + static_cast<float>(part) / 100.0f) / 100.0f;
        return *share <= 1.0f;
    }
    return false;
}

} // namespace observex
