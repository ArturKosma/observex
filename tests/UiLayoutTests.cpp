#include "core/UiLayout.h"

#include <doctest/doctest.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

using namespace observex;

namespace {

// A grey screen image to draw on.
struct Image {
    int width = 0;
    int height = 0;
    std::vector<std::uint8_t> pixels;

    Image(int w, int h, std::uint8_t grey) : width(w), height(h), pixels(static_cast<std::size_t>(w) * h, grey) {}

    std::uint8_t& at(int x, int y) { return pixels[static_cast<std::size_t>(y) * width + x]; }
    GreyLevel level() const { return {pixels.data(), width, height, width}; }

    // Noise between 40 and 100, never a frame line: something for anchors to stand out from.
    void noise(unsigned seed) {
        for (std::uint8_t& pixel : pixels) {
            seed = seed * 1664525u + 1013904223u;
            pixel = static_cast<std::uint8_t>(40 + (seed >> 24) % 61);
        }
    }
    // A 1-pixel frame from (x0, y0) to (x1, y1), both included.
    void frame(int x0, int y0, int x1, int y1, std::uint8_t grey = 200) {
        for (int x = x0; x <= x1; ++x) {
            at(x, y0) = at(x, y1) = grey;
        }
        for (int y = y0; y <= y1; ++y) {
            at(x0, y) = at(x1, y) = grey;
        }
    }
    void put(const std::vector<std::uint8_t>& picture, int w, int h, int x0, int y0) {
        for (int y = 0; y < h; ++y) {
            for (int x = 0; x < w; ++x) {
                at(x0 + x, y0 + y) = picture[static_cast<std::size_t>(y) * w + x];
            }
        }
    }
};

// A 10 x 10 picture that looks like nothing else, different for each seed.
std::vector<std::uint8_t> picture(unsigned seed) {
    std::vector<std::uint8_t> pixels(100);
    for (std::uint8_t& pixel : pixels) {
        seed = seed * 22695477u + 1u;
        pixel = static_cast<std::uint8_t>(20 + (seed >> 16) % 220);
    }
    return pixels;
}

struct Folder {
    std::filesystem::path path =
        std::filesystem::temp_directory_path() /
        ("observex-ui-tests-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    Folder() { std::filesystem::create_directories(path); }
    ~Folder() { std::filesystem::remove_all(path); }

    void anchor(const std::string& name, const std::vector<std::uint8_t>& pixels) {
        const std::vector<std::uint8_t> mask(pixels.size(), 255);
        REQUIRE(writePgm(path / (name + ".pgm"), pixels.data(), 10, 10));
        REQUIRE(writePgm(path / (name + "-mask.pgm"), mask.data(), 10, 10));
    }
};

std::string text(const UiPiece& piece) {
    return std::string(piece.name.data()) + " " + std::to_string(piece.x0) + " " + std::to_string(piece.y0) + " " +
           std::to_string(piece.x1) + " " + std::to_string(piece.y1);
}

const char* const kPanel = "anchor corner 0.9 10 10\n"
                           "anchor edge 0.9 10 10\n"
                           "# A panel 100 x 50 with its corner anchor at 5 5 and its right edge's at 90 5.\n"
                           "piece Panel 0 0 100 50 at corner 5 5\n";

} // namespace

TEST_CASE("a framed window is found by its frame, and a bright square is not one") {
    Image image(400, 300, 60);
    image.frame(50, 40, 250, 200);
    for (int y = 220; y < 280; ++y) {
        for (int x = 300; x < 380; ++x) {
            image.at(x, y) = 220; // Filled: its edges are no 1-pixel lines.
        }
    }
    UiLayout layout;
    std::vector<UiPiece> out(8);
    REQUIRE(layout.findFrames(image.level(), out.data(), 8) == 1);
    CHECK(text(out[0]) == "Window 50 40 251 201");
}

TEST_CASE("a window's frame broken by a button, or hidden at the top by bright sky, is still one window") {
    Image image(400, 300, 60);
    image.frame(50, 40, 250, 200);
    for (int x = 120; x < 140; ++x) {
        image.at(x, 200) = 60; // A button in the bottom line.
    }
    for (int y = 0; y < 39; ++y) {
        for (int x = 0; x < 400; ++x) {
            image.at(x, y) = 255; // Sky right above the top line: no darker pixels above it there.
        }
    }
    UiLayout layout;
    std::vector<UiPiece> out(8);
    REQUIRE(layout.findFrames(image.level(), out.data(), 8) == 1);
    CHECK(text(out[0]) == "Window 50 41 251 201"); // From where its sides start: the top line is not seen.
}

TEST_CASE("a window inside another is left out") {
    Image image(400, 300, 60);
    image.frame(20, 20, 380, 280);
    image.frame(100, 100, 200, 180);
    UiLayout layout;
    std::vector<UiPiece> out(8);
    REQUIRE(layout.findFrames(image.level(), out.data(), 8) == 1);
    CHECK(text(out[0]) == "Window 20 20 381 281");
}

TEST_CASE("text is no frame: a frame as sparse as a row of letters' tops makes no window") {
    Image image(400, 300, 60);
    // Every other pixel, as the tops of a chat line's letters or tall letters stacked down a column.
    for (int x = 50; x <= 250; x += 2) {
        image.at(x, 40) = image.at(x, 200) = 200;
    }
    for (int y = 40; y <= 200; y += 2) {
        image.at(50, y) = image.at(250, y) = 200;
    }
    UiLayout layout;
    std::vector<UiPiece> out(8);
    CHECK(layout.findFrames(image.level(), out.data(), 8) == 0);
    image.frame(50, 40, 250, 200);
    REQUIRE(layout.findFrames(image.level(), out.data(), 8) == 1);
    CHECK(text(out[0]) == "Window 50 40 251 201");
}

TEST_CASE("a window's top line is near where both its sides start, not only one") {
    Image image(400, 400, 60);
    // Another window's bottom line above, as the Inventory's over the hotbar: the left side starts just below it,
    // the right one far lower.
    for (int x = 20; x <= 330; ++x) {
        image.at(x, 80) = 200;
    }
    for (int y = 100; y <= 300; ++y) {
        image.at(50, y) = 200;
    }
    for (int y = 250; y <= 300; ++y) {
        image.at(300, y) = 200;
    }
    for (int x = 50; x <= 300; ++x) {
        image.at(x, 300) = 200;
    }
    UiLayout layout;
    std::vector<UiPiece> out(8);
    CHECK(layout.findFrames(image.level(), out.data(), 8) == 0);
    for (int x = 50; x <= 300; ++x) {
        image.at(x, 250) = 200; // Its own top, where the right side starts.
    }
    REQUIRE(layout.findFrames(image.level(), out.data(), 8) == 1);
    CHECK(text(out[0]) == "Window 50 250 301 301");
}

TEST_CASE("the screen's edge is a window's side only when both its level lines reach it") {
    Image image(400, 300, 60);
    // Going off the screen on the left: top and bottom lines from the edge, its right side at 150.
    for (int x = 0; x <= 150; ++x) {
        image.at(x, 40) = image.at(x, 120) = 200;
    }
    for (int y = 40; y <= 120; ++y) {
        image.at(150, y) = 200;
    }
    // A bottom line from the edge, an upright at 200 standing on it, and a line above covering more than half of the
    // way from the edge but not reaching it (the chat's tab row next to the hotbar's end): no window.
    for (int x = 0; x < 400; ++x) {
        image.at(x, 260) = 200;
    }
    for (int y = 200; y <= 260; ++y) {
        image.at(200, y) = 200;
    }
    for (int x = 60; x <= 180; ++x) {
        image.at(x, 200) = 200;
    }
    UiLayout layout;
    std::vector<UiPiece> out(8);
    REQUIRE(layout.findFrames(image.level(), out.data(), 8) == 1);
    CHECK(text(out[0]) == "Window 0 40 151 121");

    // A window whose right side is 5 pixels from the edge, its top line running on to the edge: the side is its own.
    Image near(400, 300, 60);
    near.frame(250, 150, 394, 250);
    for (int x = 394; x < 400; ++x) {
        near.at(x, 150) = 200;
    }
    REQUIRE(layout.findFrames(near.level(), out.data(), 8) == 1);
    CHECK(text(out[0]) == "Window 250 150 395 251");
}

TEST_CASE("a window partly under another is found whole, two side by side stay two") {
    SUBCASE("under") {
        Image image(400, 300, 60);
        // The lower one (the minimap): its top line broken by buttons, so no piece of it meets both sides.
        image.frame(200, 20, 380, 150);
        for (int x = 246; x < 285; ++x) {
            image.at(x, 20) = 60;
        }
        for (int x = 351; x < 380; ++x) {
            image.at(x, 20) = 60;
        }
        // The one over it (the Inventory) hides its lower left.
        for (int y = 100; y <= 280; ++y) {
            for (int x = 100; x <= 260; ++x) {
                image.at(x, y) = 60;
            }
        }
        image.frame(100, 100, 260, 280);
        UiLayout layout;
        std::vector<UiPiece> out(8);
        REQUIRE(layout.findFrames(image.level(), out.data(), 8) == 2);
        CHECK(text(out[0]) == "Window 100 100 261 281");
        CHECK(text(out[1]) == "Window 200 20 381 151"); // Not only its part right of the one over it.
    }
    SUBCASE("side by side") {
        Image image(400, 300, 60);
        image.frame(50, 50, 150, 150);
        image.frame(150, 100, 300, 250);
        UiLayout layout;
        std::vector<UiPiece> out(8);
        REQUIRE(layout.findFrames(image.level(), out.data(), 8) == 2);
        CHECK(text(out[0]) == "Window 150 100 301 251");
        CHECK(text(out[1]) == "Window 50 50 151 151");
    }
}

TEST_CASE("a piece is placed where its anchor is found, a side stretched to its own anchor") {
    Folder folder;
    folder.anchor("corner", picture(1));
    folder.anchor("edge", picture(2));
    UiLayout layout;
    std::string error;
    REQUIRE_MESSAGE(layout.parse(std::string(kPanel) + "  right edge 90 5\n", folder.path, &error), error);
    CHECK(layout.anchors() == 2);
    CHECK(layout.pieces() == 1);

    Image image(500, 300, 0);
    image.noise(7);
    image.put(picture(1), 10, 10, 105, 85); // The panel moved 100 right and 80 down,
    image.put(picture(2), 10, 10, 230, 86); // and is 40 pixels wider (its edge a pixel lower: within the same row).
    std::vector<UiPiece> out(8);
    REQUIRE(layout.find(image.level(), out.data(), 8) == 1);
    CHECK(text(out[0]) == "Panel 100 80 240 130");
}

TEST_CASE("a side that must be found drops the piece without it; one that may be missing keeps its place") {
    Folder folder;
    folder.anchor("corner", picture(1));
    folder.anchor("edge", picture(2));
    Image image(500, 300, 0);
    image.noise(7);
    image.put(picture(1), 10, 10, 105, 85);
    std::vector<UiPiece> out(8);
    UiLayout layout;
    REQUIRE(layout.parse(std::string(kPanel) + "  right edge 90 5\n", folder.path, nullptr));
    CHECK(layout.find(image.level(), out.data(), 8) == 0);
    REQUIRE(layout.parse(std::string(kPanel) + "  right? edge 90 5\n", folder.path, nullptr));
    REQUIRE(layout.find(image.level(), out.data(), 8) == 1);
    CHECK(text(out[0]) == "Panel 100 80 200 130");
}

TEST_CASE("a side's search stops at the next window's anchor") {
    Folder folder;
    folder.anchor("corner", picture(1));
    folder.anchor("edge", picture(2));
    Image image(500, 300, 0);
    image.noise(7);
    image.put(picture(1), 10, 10, 105, 85); // Two panels in a row; only the second's edge is there.
    image.put(picture(1), 10, 10, 305, 85);
    image.put(picture(2), 10, 10, 330, 85);
    UiLayout layout;
    REQUIRE(layout.parse(std::string(kPanel) + "  right edge 90 5\n", folder.path, nullptr));
    std::vector<UiPiece> out(8);
    REQUIRE(layout.find(image.level(), out.data(), 8) == 1);
    CHECK(text(out[0]) == "Panel 300 80 340 130");
}

TEST_CASE("a side under a framed window ends where the window begins, if that is not too far") {
    Folder folder;
    folder.anchor("corner", picture(1));
    folder.anchor("edge", picture(2));
    UiLayout layout;
    REQUIRE(layout.parse(std::string(kPanel) + "  right edge 90 5\n", folder.path, nullptr));
    std::vector<UiPiece> out(8);
    {
        // The panel was made wider and a window lies over its edge, though not over where the edge was measured.
        Image image(500, 300, 0);
        image.noise(7);
        image.put(picture(1), 10, 10, 105, 85);
        image.frame(230, 60, 330, 200);
        REQUIRE(layout.find(image.level(), out.data(), 8) == 2);
        CHECK(text(out[0]) == "Window 230 60 331 201");
        CHECK(text(out[1]) == "Panel 100 80 230 130");
    }
    {
        // A window further than the panel's measured width past its measured edge: the edge is just missing.
        Image image(500, 300, 0);
        image.noise(7);
        image.put(picture(1), 10, 10, 105, 85);
        image.frame(320, 60, 420, 200);
        REQUIRE(layout.find(image.level(), out.data(), 8) == 1);
        CHECK(text(out[0]) == "Window 320 60 421 201");
    }
}

TEST_CASE("a side's next line is tried when its first is missing, its anchor kept as far from the nearer edge") {
    Folder folder;
    folder.anchor("corner", picture(1));
    folder.anchor("edge", picture(2));
    folder.anchor("low", picture(3));
    folder.anchor("foot", picture(4));
    UiLayout layout;
    std::string error;
    // The right side by its top anchor, else by one near the bottom, which moves with the bottom.
    REQUIRE_MESSAGE(layout.parse("anchor corner 0.9 10 10\n"
                                 "anchor edge 0.9 10 10\n"
                                 "anchor low 0.9 10 10\n"
                                 "anchor foot 0.9 10 10\n"
                                 "piece Panel 0 0 100 50 at corner 5 5\n"
                                 "  bottom foot 5 38\n"
                                 "  right edge 90 5\n"
                                 "  right low 90 38\n",
                                 folder.path, &error),
                    error);
    Image image(500, 300, 0);
    image.noise(7);
    image.put(picture(1), 10, 10, 105, 85);  // Moved 100 right and 80 down,
    image.put(picture(4), 10, 10, 105, 138); // made 20 taller (its top stays)
    image.put(picture(3), 10, 10, 230, 138); // and 40 wider; the top-right anchor is not there.
    std::vector<UiPiece> out(8);
    REQUIRE(layout.find(image.level(), out.data(), 8) == 1);
    CHECK(text(out[0]) == "Panel 100 80 240 150");
}

TEST_CASE("an anchor is not found where the image only looks a little like it") {
    Folder folder;
    folder.anchor("corner", picture(1));
    Image image(300, 200, 0);
    image.noise(7);
    std::vector<std::uint8_t> blurred = picture(1);
    for (std::size_t i = 0; i < blurred.size(); i += 2) {
        blurred[i] = 128; // Half its pixels lost.
    }
    image.put(blurred, 10, 10, 50, 50);
    UiLayout layout;
    REQUIRE(layout.parse("anchor corner 0.9 10 10\npiece Panel 0 0 100 50 at corner 5 5\n", folder.path, nullptr));
    std::vector<UiPiece> out(8);
    CHECK(layout.find(image.level(), out.data(), 8) == 0);
}

TEST_CASE("ui.txt mistakes are reported with their line") {
    Folder folder;
    folder.anchor("corner", picture(1));
    UiLayout layout;
    std::string error;
    CHECK_FALSE(
        layout.parse("anchor corner 0.9 10 10\n\npiece Panel 0 0 100 50 at nowhere 5 5\n", folder.path, &error));
    CHECK(error == "ui.txt line 3: no anchor nowhere above this line");
    CHECK(layout.anchors() == 0);
    CHECK_FALSE(layout.parse("anchor missing 0.9 10 10\n", folder.path, &error));
    CHECK(error.starts_with("ui.txt line 1: could not read missing.pgm"));
    CHECK_FALSE(layout.parse("  right corner 1 1\n", folder.path, &error));
    CHECK(error == "ui.txt line 1: a side line needs a piece line above it");
    CHECK_FALSE(layout.parse("anchor corner 0.9 10 10\npiece Panel 0 0 100 50 at corner 5 5\n  middle corner 1 1\n",
                             folder.path, &error));
    CHECK(error.starts_with("ui.txt line 3: expected: left, right, top or bottom"));
    CHECK_FALSE(layout.load(folder.path / "none", &error));
}

TEST_CASE("a PGM survives writing and reading") {
    Folder folder;
    const std::vector<std::uint8_t> pixels = picture(3);
    REQUIRE(writePgm(folder.path / "p.pgm", pixels.data(), 10, 10));
    std::vector<std::uint8_t> read;
    int width = 0;
    int height = 0;
    REQUIRE(readPgm(folder.path / "p.pgm", read, width, height));
    CHECK(width == 10);
    CHECK(height == 10);
    CHECK(read == pixels);
}
