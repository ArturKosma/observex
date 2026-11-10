#include "core/LiveLines.h"

#include <doctest/doctest.h>

#include <array>
#include <string>
#include <string_view>
#include <vector>

using namespace observex;

namespace {

// The lines writeLook writes, one string each.
std::vector<std::string> linesOf(std::span<const char> text) {
    std::vector<std::string> lines;
    std::string line;
    for (const char c : text) {
        if (c == '\n') {
            lines.push_back(line);
            line.clear();
        } else {
            line += c;
        }
    }
    CHECK(line.empty()); // Every line ends in '\n'.
    return lines;
}

Detection thing(float x, float sure, std::uint8_t kind, float kindSure, std::uint8_t secondKind, float second) {
    Detection found{x, 20.0f, 40.0f, 80.0f, sure};
    found.kind = kind;
    found.kindSure = kindSure;
    found.secondKind = secondKind;
    found.second = second;
    return found;
}

} // namespace

TEST_CASE("writeLook writes one look line for a model without kinds, as the logs before kinds") {
    std::array<char, kLookChars> out{};
    const std::vector<Detection> found{{1241, 694, 91, 175, 0.92f}, {933, 529, 149, 171, 0.5f}};
    const std::size_t length = writeLook(out, 469805, 19.84, found);
    const auto lines = linesOf(std::span<const char>(out.data(), length));
    REQUIRE(lines.size() == 1);
    CHECK(lines[0] == "look 469805 19.8 0.92 1241 694 91 175 0.50 933 529 149 171");
    CHECK(linesOf(std::span<const char>(out.data(), writeLook(out, 7, 1.0, {})))[0] == "look 7 1.0");
}

TEST_CASE("writeLook adds a kinds line for a model with kinds, and the reader gets back what was found") {
    std::array<char, kLookChars> out{};
    // A monster 90% (NPC 30%), and a player whose sureness is that of being either of its two kinds.
    const std::vector<Detection> found{thing(100, 0.93f, 0, 0.9f, 2, 0.3f), thing(400, 0.8f, 1, 0.6f, 0, 0.5f)};
    const std::size_t length = writeLook(out, 12, 33.5, found);
    const auto lines = linesOf(std::span<const char>(out.data(), length));
    REQUIRE(lines.size() == 2);
    CHECK(lines[1] == "kinds 12 0 0.90 2 0.30 1 0.60 0 0.50");

    unsigned long long image = 0;
    double timeMs = 0.0;
    std::vector<Detection> back;
    REQUIRE(readLook(lines[0], image, timeMs, back));
    CHECK(image == 12);
    CHECK(timeMs == doctest::Approx(33.5));
    REQUIRE(back.size() == 2);
    CHECK(back[1].x == 400.0f);
    CHECK(back[1].sure == doctest::Approx(0.8f));
    CHECK(back[1].second < 0.0f); // No kinds before the kinds line.
    REQUIRE(readKinds(lines[1], image, back));
    CHECK(back[0].kind == 0);
    CHECK(kindSureOf(back[0]) == doctest::Approx(0.9f));
    CHECK(back[1].kind == 1);
    CHECK(kindSureOf(back[1]) == doctest::Approx(0.6f));
    CHECK(back[1].secondKind == 0);
    CHECK(back[1].second == doctest::Approx(0.5f));
}

TEST_CASE("readLook and readKinds take only their own lines") {
    unsigned long long image = 0;
    double timeMs = 0.0;
    std::vector<Detection> found;
    CHECK_FALSE(readLook("saved 000001.jpg 469806 33.5", image, timeMs, found));
    CHECK_FALSE(readLook("lookup 1 2", image, timeMs, found));
    CHECK_FALSE(readLook("# look IMAGE TIME", image, timeMs, found));
    REQUIRE(readLook("look 5 10.0 0.70 1 2 3 4", image, timeMs, found));
    CHECK_FALSE(readKinds("kinds 6 1 0.70 0 0.10", image, found)); // Another look's.
    CHECK_FALSE(readKinds("mirror 5 0.120", image, found));
    CHECK(found[0].second < 0.0f);
}

TEST_CASE("writeLook leaves out what does not fit from both lines alike") {
    std::array<char, 4 * 64> small{}; // Room for two things.
    const std::vector<Detection> found(5, thing(10, 0.9f, 3, 0.9f, 1, 0.1f));
    const auto lines = linesOf(std::span<const char>(small.data(), writeLook(small, 1, 2.0, found)));
    REQUIRE(lines.size() == 2);
    unsigned long long image = 0;
    double timeMs = 0.0;
    std::vector<Detection> back;
    REQUIRE(readLook(lines[0], image, timeMs, back));
    CHECK(back.size() == 2);
    CHECK(lines[1] == "kinds 1 3 0.90 1 0.10 3 0.90 1 0.10");
}
