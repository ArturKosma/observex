#include "heuristics/KeyLabels.h"

#include <doctest/doctest.h>

#include <array>
#include <cstdint>
#include <string>
#include <vector>

using namespace observex;

namespace {

constexpr int kSide = 42; // The hotbar's slots at the user's UI scale.

using Bitmap = std::array<const char*, 7>;
constexpr Bitmap kF = {"#####", "#....", "#....", "####.", "#....", "#....", "#...."};
constexpr Bitmap kOne = {"..#..", ".##..", "..#..", "..#..", "..#..", "..#..", ".###."};
constexpr Bitmap kTwo = {".###.", "#...#", "....#", "...#.", "..#..", ".#...", "#####"};

// A slot: a light frame (row 0, column 0), a bluish picture, and `letters` written from column 3, row 2, in cream with
// a dark rim, two columns apart (BGRA).
std::vector<std::uint8_t> slot(const std::vector<Bitmap>& letters) {
    std::vector<std::uint8_t> pixels(static_cast<std::size_t>(kSide * kSide * 4));
    const auto set = [&](int x, int y, std::uint8_t b, std::uint8_t g, std::uint8_t r) {
        std::uint8_t* p = pixels.data() + static_cast<std::size_t>((y * kSide + x) * 4);
        p[0] = b;
        p[1] = g;
        p[2] = r;
        p[3] = 255;
    };
    for (int y = 0; y < kSide; ++y) {
        for (int x = 0; x < kSide; ++x) {
            if (x == 0 || y == 0) {
                set(x, y, 190, 200, 200);
            } else {
                set(x, y, static_cast<std::uint8_t>(170 + (x * 7 + y * 3) % 40), 110, 40);
            }
        }
    }
    std::vector<std::uint8_t> letter(static_cast<std::size_t>(kSide * kSide), 0);
    int left = 3;
    for (const Bitmap& bitmap : letters) {
        for (int y = 0; y < 7; ++y) {
            for (int x = 0; x < 5; ++x) {
                if (bitmap[static_cast<std::size_t>(y)][x] == '#') {
                    letter[static_cast<std::size_t>((y + 2) * kSide + left + x)] = 1;
                }
            }
        }
        left += 7;
    }
    for (int y = 1; y < kSide - 1; ++y) {
        for (int x = 1; x < kSide - 1; ++x) {
            if (letter[static_cast<std::size_t>(y * kSide + x)] != 0) {
                set(x, y, 180, 225, 235);
                continue;
            }
            for (int dy = -1; dy <= 1; ++dy) {
                for (int dx = -1; dx <= 1; ++dx) {
                    if (letter[static_cast<std::size_t>((y + dy) * kSide + x + dx)] != 0) {
                        set(x, y, 20, 20, 20);
                    }
                }
            }
        }
    }
    return pixels;
}

KeyPicture pictureOf(const std::vector<std::uint8_t>& pixels) {
    return {pixels.data(), static_cast<std::size_t>(kSide * 4), 4, kSide};
}

} // namespace

TEST_CASE("key labels are learned from typed keys and read on other slots") {
    KeyFont font;
    const std::vector<std::uint8_t> f1 = slot({kF, kOne});
    const std::vector<std::uint8_t> f2 = slot({kF, kTwo});
    const std::vector<std::uint8_t> f12 = slot({kF, kOne, kTwo});
    const std::vector<std::uint8_t> none = slot({});

    CHECK(font.read(pictureOf(f1)) == "??"); // Letters, none known yet.
    CHECK(font.read(pictureOf(none)).empty());
    CHECK(font.teach(pictureOf(f1), "F1") == 2); // A letter each.
    CHECK(font.read(pictureOf(f1)) == "F1");
    CHECK(font.read(pictureOf(f2)) == "F?");
    CHECK(font.teach(pictureOf(f2), "F2") == 1); // Only the 2: F is known.
    CHECK(font.read(pictureOf(f12)) == "F12");
    std::vector<KeyPart> parts;
    CHECK(font.read(pictureOf(f12), &parts) == "F12");
    REQUIRE(parts.size() == 3);
    CHECK(parts[0].from == 3);
    CHECK(parts[1].known);
    CHECK(parts[2].score > 0.99f);

    // Written and read back, it reads alike.
    KeyFont again;
    std::string error;
    REQUIRE_MESSAGE(again.parse(font.write(), &error), error);
    CHECK(again.glyphs() == font.glyphs());
    CHECK(again.read(pictureOf(f12)) == "F12");
    CHECK(again.read(pictureOf(f2)) == "F2");

    // Typed keys that do not agree with what is known: one glyph over the whole label, which then wins (wider).
    CHECK(font.teach(pictureOf(f1), "Q") == 1);
    CHECK(font.read(pictureOf(f1)) == "Q");
    CHECK(font.read(pictureOf(f2)) == "F2");

    // Nothing written: nothing learned.
    CHECK(font.teach(pictureOf(none), "F9") == 0);
}

TEST_CASE("a profile's BGR picture reads like the screen's BGRA") {
    KeyFont font;
    const std::vector<std::uint8_t> f2 = slot({kF, kTwo});
    REQUIRE(font.teach(pictureOf(f2), "F2") == 2);
    std::vector<std::uint8_t> bgr;
    for (std::size_t i = 0; i < f2.size(); i += 4) {
        bgr.insert(bgr.end(), {f2[i], f2[i + 1], f2[i + 2]});
    }
    CHECK(font.read({bgr.data(), static_cast<std::size_t>(kSide * 3), 3, kSide}) == "F2");
}

TEST_CASE("a wrong keys file is not read") {
    KeyFont font;
    std::string error;
    CHECK_FALSE(font.parse("glyph 42 2 1 ff10 LD F\nglyph 42 2 1 ff LL F\n", &error));
    CHECK(error.find("line 2") != std::string::npos);
    CHECK_FALSE(font.parse("glyph 42 2 1 ffzz LL F\n", &error));
    CHECK_FALSE(font.parse("glyph 42 2 1 ff10 .. F\n", &error)); // No letter pixels.
    CHECK(font.parse("# a comment\n\nglyph 42 2 1 ff10 LD F\r\n", &error));
    CHECK(font.glyphs() == 1);
}
