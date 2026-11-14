#include "heuristics/Miniatures.h"
#include "heuristics/StatsReader.h"

#include <doctest/doctest.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace observex;

namespace {

// A font of two glyphs, '1' and '/', 4 x 6 each with a pitch of 5: the letter a white column (x 1), its line round it
// dark, the mask the letter and the columns beside it.
constexpr int kW = 4;
constexpr int kH = 6;

void writeFont(const std::filesystem::path& folder) {
    std::vector<std::uint8_t> pixels(static_cast<std::size_t>(kW * 2 * kH), 60);
    std::vector<std::uint8_t> mask(pixels.size(), 0);
    for (int g = 0; g < 2; ++g) {
        for (int y = 0; y < kH; ++y) {
            for (int x = 0; x < 3; ++x) {
                const std::size_t i = static_cast<std::size_t>(y * kW * 2 + g * kW + x);
                mask[i] = 255;
                // '1': upright; '/': its top half one column right.
                const int letterX = g == 0 ? 1 : (y < kH / 2 ? 2 : 1);
                pixels[i] = x == letterX ? 250 : 20;
            }
        }
    }
    REQUIRE(writePgm(folder / "glyphs.pgm", pixels.data(), kW * 2, kH));
    REQUIRE(writePgm(folder / "glyphs-mask.pgm", mask.data(), kW * 2, kH));
}

std::filesystem::path testFolder() {
    const std::filesystem::path folder = std::filesystem::temp_directory_path() / "observex-gamestats-test";
    std::filesystem::create_directories(folder);
    writeFont(folder);
    return folder;
}

constexpr char kLayout[] = "# A test layout\n"
                           "glyphs glyphs 1/ 5 4 6\n"
                           "  from somewhere.jpg 0 0 1/1\n"
                           "bar HP Status red 2 10 4 -4 text glyphs 3 10\n"
                           "bar Target Target red 1 4 2 -2\n";

// A BGRA image of `w` x `h` in one colour.
struct Picture {
    int w = 0;
    int h = 0;
    std::vector<std::uint8_t> bgra;
    Picture(int width, int height, std::uint8_t b, std::uint8_t g, std::uint8_t r) : w(width), h(height) {
        bgra.resize(static_cast<std::size_t>(w * h * 4));
        for (int i = 0; i < w * h; ++i) {
            bgra[static_cast<std::size_t>(i * 4)] = b;
            bgra[static_cast<std::size_t>(i * 4 + 1)] = g;
            bgra[static_cast<std::size_t>(i * 4 + 2)] = r;
            bgra[static_cast<std::size_t>(i * 4 + 3)] = 255;
        }
    }
    void set(int x, int y, std::uint8_t b, std::uint8_t g, std::uint8_t r) {
        std::uint8_t* p = &bgra[static_cast<std::size_t>((y * w + x) * 4)];
        p[0] = b;
        p[1] = g;
        p[2] = r;
    }
    ColourPart part() const { return {{0, 0, w, h}, bgra.data(), static_cast<std::size_t>(w * 4)}; }
};

UiPiece piece(const char* name, int x0, int y0, int x1, int y1) {
    UiPiece p;
    std::snprintf(p.name.data(), p.name.size(), "%s", name);
    p.x0 = x0;
    p.y0 = y0;
    p.x1 = x1;
    p.y1 = y1;
    return p;
}

// Writes the glyph `g` ('1' 0, '/' 1) of the test font with its picture's top left at x, y.
void writeGlyph(Picture& picture, int g, int x, int y) {
    for (int dy = 0; dy < kH; ++dy) {
        for (int dx = 0; dx < 3; ++dx) {
            const int letterX = g == 0 ? 1 : (dy < kH / 2 ? 2 : 1);
            const std::uint8_t v = dx == letterX ? 250 : 20;
            picture.set(x + dx, y + dy, v, v, v);
        }
    }
}

} // namespace

TEST_CASE("StatsReader reads a layout and rejects a broken one") {
    const std::filesystem::path folder = testFolder();
    StatsReader reader;
    std::string error;
    CHECK_MESSAGE(reader.parse(kLayout, folder, &error), error);
    CHECK(reader.bars() == 2);
    CHECK_FALSE(reader.parse("bar HP Status green 2 10 4 -4\n", folder, &error));
    CHECK(error.find("red, blue or lilac") != std::string::npos);
    CHECK_FALSE(reader.parse("glyphs missing 1/ 5 4 6\n", folder, &error));
    CHECK(reader.bars() == 0);
}

TEST_CASE("StatsReader asks for one part a piece") {
    StatsReader reader;
    REQUIRE(reader.parse(kLayout, testFolder(), nullptr));
    const UiPiece pieces[] = {piece("Status", 10, 20, 110, 60), piece("Chat", 0, 0, 5, 5)};
    StatsArea areas[4];
    const int count = reader.areas(pieces, 200, 200, areas, 4);
    REQUIRE(count == 1); // No target window: only the status window's part.
    CHECK(areas[0].x0 == 10);
    CHECK(areas[0].x1 == 110);
    CHECK(areas[0].y0 == 20 + 2);           // The bar's top (above the text's row 3 - 1).
    CHECK(areas[0].y1 == 20 + 3 + kH + 1); // The text's rows and one more.
}

TEST_CASE("StatsReader takes neither a bar nor its number when they disagree") {
    StatsReader reader;
    REQUIRE(reader.parse(kLayout, testFolder(), nullptr));
    // Status window 0..104: the bar is columns 4 to 100 (96 wide), rows 2 to 10; filled red to column 76 (75%), the
    // empty part dark; "1/1" written at column 40 over the red. With no number agreeing, a fill that ends past where
    // the number begins may end where something over the number begins: not taken.
    Picture picture(104, 12, 40, 50, 90);
    for (int y = 2; y < 10; ++y) {
        for (int x = 4; x < 76; ++x) {
            picture.set(x, y, 55, 66, 248);
        }
    }
    writeGlyph(picture, 0, 40, 3);
    writeGlyph(picture, 1, 45, 3);
    writeGlyph(picture, 0, 50, 3);
    const UiPiece pieces[] = {piece("Status", 0, 0, 104, 12)};
    const ColourPart parts[] = {picture.part()};
    GameStats stats;
    reader.read(pieces, parts, stats);
    REQUIRE(stats.count == 2);
    const GameGauge* hp = stats.find("Status", "HP");
    REQUIRE(hp != nullptr);
    // "1/1" says full, the bar 75%: they disagree, so the number is only doubted, and the bar not taken.
    CHECK_FALSE(hp->seen);
    CHECK(hp->full < 0.0f);
    CHECK(std::string(hp->text.data()).empty());
    CHECK(std::string(hp->doubted.data()) == "1/1");
    CHECK(std::string(stats.why.data()).find("number not read") != std::string::npos);
    const GameGauge* target = stats.find("Target", "Target");
    REQUIRE(target != nullptr);
    CHECK_FALSE(target->seen); // No target window.
}

TEST_CASE("StatsReader keeps a number that agrees with its bar") {
    StatsReader reader;
    REQUIRE(reader.parse(kLayout, testFolder(), nullptr));
    Picture picture(104, 12, 40, 50, 90);
    for (int y = 2; y < 10; ++y) {
        for (int x = 4; x < 100; ++x) {
            picture.set(x, y, 55, 66, 248);
        }
    }
    writeGlyph(picture, 0, 40, 3);
    writeGlyph(picture, 1, 45, 3);
    writeGlyph(picture, 0, 50, 3);
    const UiPiece pieces[] = {piece("Status", 0, 0, 104, 12)};
    const ColourPart parts[] = {picture.part()};
    GameStats stats;
    reader.read(pieces, parts, stats);
    const GameGauge* hp = stats.find("Status", "HP");
    REQUIRE(hp != nullptr);
    CHECK(hp->full == doctest::Approx(1.0));
    CHECK(std::string(hp->text.data()) == "1/1");
}

TEST_CASE("StatsReader does not tell how full a bar under a window is") {
    StatsReader reader;
    REQUIRE(reader.parse(kLayout, testFolder(), nullptr));
    // Red to column 60, then a light window over the rest of it: its empty part cannot be seen.
    Picture picture(104, 12, 40, 50, 90);
    for (int y = 2; y < 10; ++y) {
        for (int x = 4; x < 100; ++x) {
            picture.set(x, y, x < 60 ? 55 : 200, x < 60 ? 66 : 200, x < 60 ? 248 : 200);
        }
    }
    const UiPiece pieces[] = {piece("Status", 0, 0, 104, 12)};
    const ColourPart parts[] = {picture.part()};
    GameStats stats;
    reader.read(pieces, parts, stats);
    const GameGauge* hp = stats.find("Status", "HP");
    REQUIRE(hp != nullptr);
    CHECK(hp->full < 0.0f);
    CHECK_FALSE(hp->seen);

    // A full bar whose light end cap is a column, "1/1" over it: still full.
    for (int y = 2; y < 10; ++y) {
        for (int x = 4; x < 100; ++x) {
            picture.set(x, y, x < 99 ? 55 : 200, x < 99 ? 66 : 200, x < 99 ? 248 : 200);
        }
    }
    writeGlyph(picture, 0, 40, 3);
    writeGlyph(picture, 1, 45, 3);
    writeGlyph(picture, 0, 50, 3);
    reader.read(pieces, parts, stats);
    CHECK(hp->seen);
    CHECK(hp->full == doctest::Approx(1.0));
    CHECK(std::string(hp->text.data()) == "1/1");

    // The same bar with its number gone (something over it): ending past where the number began, not taken.
    for (int y = 2; y < 10; ++y) {
        for (int x = 4; x < 100; ++x) {
            picture.set(x, y, x < 99 ? 55 : 200, x < 99 ? 66 : 200, x < 99 ? 248 : 200);
        }
    }
    reader.read(pieces, parts, stats);
    CHECK_FALSE(hp->seen);
    CHECK(std::string(stats.why.data()).find("HP: number not read") != std::string::npos);
}

TEST_CASE("A bar's number as a share") {
    float share = 0.0f;
    CHECK(StatsReader::fraction("238/256", &share));
    CHECK(share == doctest::Approx(238.0 / 256.0));
    CHECK(StatsReader::fraction("26.64%", &share));
    CHECK(share == doctest::Approx(0.2664));
    CHECK(StatsReader::fraction("0/256", &share));
    CHECK(share == 0.0f);
    CHECK_FALSE(StatsReader::fraction("300/256", &share));
    CHECK_FALSE(StatsReader::fraction("26.6%", &share));
    CHECK_FALSE(StatsReader::fraction("/", &share));
    CHECK_FALSE(StatsReader::fraction("", &share));
}

TEST_CASE("A stats line reads back as written") {
    GameStats stats;
    stats.count = 2;
    std::snprintf(stats.gauges[0].piece.data(), stats.gauges[0].piece.size(), "%s", "Status");
    std::snprintf(stats.gauges[0].name.data(), stats.gauges[0].name.size(), "%s", "HP");
    stats.gauges[0].seen = true;
    stats.gauges[0].full = 0.9262f;
    std::snprintf(stats.gauges[0].text.data(), stats.gauges[0].text.size(), "%s", "238/256");
    std::snprintf(stats.gauges[1].piece.data(), stats.gauges[1].piece.size(), "%s", "Target");
    std::snprintf(stats.gauges[1].name.data(), stats.gauges[1].name.size(), "%s", "Target");
    std::string text;
    appendStatsLine(text, 812, 5021.4, stats);
    CHECK(text == "stats 812 5021.4 Status HP 0.9262 238/256 Target Target - -\n");

    std::uint64_t image = 0;
    double ms = 0.0;
    GameStats back;
    REQUIRE(readStatsLine(text, &image, &ms, &back));
    CHECK(image == 812);
    CHECK(ms == doctest::Approx(5021.4));
    REQUIRE(back.count == 2);
    CHECK(back.gauges[0].seen);
    CHECK(back.gauges[0].full == doctest::Approx(0.9262f));
    CHECK(std::string(back.gauges[0].text.data()) == "238/256");
    CHECK_FALSE(back.gauges[1].seen);
    CHECK(back.find("Target", "Target") == &back.gauges[1]);
    CHECK_FALSE(readStatsLine("stats 1 2 Status HP", &image, &ms, &back));
    CHECK_FALSE(readStatsLine("note 1 hello", &image, &ms, &back));
}

TEST_CASE("A frame's stats are the last read on its image or one before") {
    const std::string text = "# what was read\n"
                             "stats 12 -200 Status HP 0.5 -\n"
                             "stats 10 -300 Status HP 1 -\n"
                             "something else\n"
                             "stats 14 -100 Status HP 0.25 -\n";
    const std::vector<ClipStats> lines = readClipStats(text);
    REQUIRE(lines.size() == 3);
    CHECK(statsAt(lines, 9) == nullptr);
    CHECK(statsAt(lines, 10)->stats.image == 10);
    CHECK(statsAt(lines, 13)->stats.image == 12);
    CHECK(statsAt(lines, 500)->stats.image == 14);
    CHECK(statsAt(lines, 13)->ms == doctest::Approx(-200.0));
}

TEST_CASE("A stats line keeps a skill's and a target's spaces, and the target") {
    GameStats stats;
    stats.count = 2;
    std::snprintf(stats.gauges[0].piece.data(), stats.gauges[0].piece.size(), "%s", kSkillsPiece);
    std::snprintf(stats.gauges[0].name.data(), stats.gauges[0].name.size(), "%s", "Wind Strike");
    stats.gauges[0].seen = true;
    stats.gauges[0].full = 0.75f;
    std::snprintf(stats.gauges[0].text.data(), stats.gauges[0].text.size(), "%s", "5");
    std::snprintf(stats.gauges[1].piece.data(), stats.gauges[1].piece.size(), "%s", kTargetPiece);
    std::snprintf(stats.gauges[1].name.data(), stats.gauges[1].name.size(), "%s", kTargetNameGauge);
    stats.gauges[1].seen = true;
    std::snprintf(stats.gauges[1].text.data(), stats.gauges[1].text.size(), "%s", "Ashen Wolf");
    std::string text;
    appendStatsLine(text, 5, -10.0, stats);
    CHECK(text == "stats 5 -10 Hotbar Wind_Strike 0.7500 5 Target Name - Ashen_Wolf\n");

    std::uint64_t image = 0;
    double ms = 0.0;
    GameStats back;
    REQUIRE(readStatsLine(text, &image, &ms, &back));
    CHECK(back.find(kSkillsPiece, "Wind Strike") != nullptr);
    CHECK(back.hasTarget);
    CHECK(std::string(back.targetName.data()) == "Ashen Wolf");
    CHECK(std::string(back.target.data()) == "Ashen Wolf");
    CHECK_FALSE(back.targetChanged);
    REQUIRE(readStatsLine("stats 6 -5 Target HP - -", &image, &ms, &back));
    CHECK_FALSE(back.hasTarget);
    CHECK(back.target[0] == '\0');
}

TEST_CASE("A skill's name is a file name") {
    CHECK(skillFileName("  Wind Strike ") == "Wind Strike");
    CHECK(skillFileName("Wind_Strike").empty()); // "_" is a clip line's space.
    CHECK(skillFileName("a/b").empty());
    CHECK(skillFileName("Heal.").empty());
    CHECK(skillFileName("   ").empty());
    CHECK(skillFileName(std::string(40, 'a')).size() <= 31);
}

TEST_CASE("Miniatures are found by their frames' top and left lines") {
    // A dark screen with two miniatures, 30 and 40 pixels: each a light top row and left column (the bevel), a
    // darker right and bottom (cooling), and a picture inside.
    constexpr int kWidth = 160;
    constexpr int kHeight = 80;
    std::vector<std::uint8_t> grey(static_cast<std::size_t>(kWidth * kHeight), 30);
    const auto box = [&](int x0, int y0, int side) {
        for (int i = 0; i < side; ++i) {
            grey[static_cast<std::size_t>(y0 * kWidth + x0 + i)] = 200;           // Top.
            grey[static_cast<std::size_t>((y0 + i) * kWidth + x0)] = 200;         // Left.
            grey[static_cast<std::size_t>((y0 + side - 1) * kWidth + x0 + i)] = 60; // Bottom.
        }
        for (int y = 1; y < side - 1; ++y) {
            for (int x = 1; x < side - 1; ++x) {
                grey[static_cast<std::size_t>((y0 + y) * kWidth + x0 + x)] = static_cast<std::uint8_t>(90 + (x * 7) % 60);
            }
        }
    };
    box(10, 10, 30);
    box(80, 20, 40);
    StatsArea found[8];
    const int count = findMiniatures(grey.data(), kWidth, kWidth, kHeight, {0, 0, kWidth, kHeight}, found, 8);
    REQUIRE(count == 2);
    CHECK(found[0].x0 == 10);
    CHECK(found[0].y0 == 10);
    CHECK(found[0].x1 - found[0].x0 == 30);
    CHECK(found[1].x0 == 80);
    CHECK(found[1].y0 == 20);
    CHECK(found[1].y1 - found[1].y0 == 40);
}

TEST_CASE("StatsReader reads the cast, skills and words lines") {
    const std::filesystem::path folder = testFolder();
    // A yellow font of one glyph, and a skill.
    std::vector<std::uint8_t> yellow(static_cast<std::size_t>(kW * kH), 255);
    REQUIRE(writePgm(folder / "seconds.pgm", yellow.data(), kW, kH));
    REQUIRE(writePgm(folder / "seconds-mask.pgm", yellow.data(), kW, kH));
    std::vector<std::uint8_t> picture(32 * 32, 120);
    const std::string layout = std::string(kLayout) + "glyphs seconds 1 5 4 6 yellow\n"
                                                      "cast Casting 9 26 14 283\n"
                                                      "skills Hotbar seconds\n"
                                                      "words Name Target 5 22 44 -16\n";
    StatsReader reader;
    std::string error;
    CHECK_MESSAGE(reader.parse(layout, folder, &error), error);
    CHECK(reader.skills() == 0); // None until given (the profile's).
    SkillPicture skill;
    std::snprintf(skill.name.data(), skill.name.size(), "Wind Strike");
    skill.side = 32;
    skill.grey = picture.data();
    reader.setSkills({&skill, 1});
    CHECK(reader.skills() == 1);
    CHECK_FALSE(reader.parse(std::string(kLayout) + "skills Hotbar glyphs\n", folder, &error));
    CHECK(error.find("yellow") != std::string::npos);
    CHECK_FALSE(reader.parse("cast Casting 9 26 14\n", folder, &error));
}
