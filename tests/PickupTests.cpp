#include "core/GameStats.h"
#include "heuristics/MessageLines.h"
#include "heuristics/PickupReader.h"

#include <doctest/doctest.h>

#include <cstdint>
#include <string>
#include <vector>

using namespace observex;

namespace {

// A Messages window's lowest rows as MessageLines looks at them: dark, a message a line 20 rows apart from the bottom
// up, its "letters" columns 3 wide lit by the message's id (as different texts' letters fall in different places).
// An id's `variant` changes only its last few letters (as "69 Adena" and "72 Adena" differ).
struct Window {
    static constexpr int kWidth = 320;
    static constexpr int kHeight = MessageLines::kRows;
    struct Message {
        int id = 0;
        int variant = 0;
        bool yellow = false;
    };
    std::vector<Message> messages; // Oldest first; the newest at the bottom.
    std::vector<std::uint8_t> bgra;

    static bool lit(int id, int variant, int block) {
        const int seed = block >= 58 ? id * 7919 + variant * 104729 + block : id * 7919 + block;
        return (static_cast<unsigned>(seed) * 2654435761u >> 16) % 3 == 0;
    }

    ColourPart draw() {
        bgra.assign(static_cast<std::size_t>(kWidth) * kHeight * 4, 0);
        for (std::size_t i = 0; i < bgra.size(); i += 4) {
            bgra[i] = bgra[i + 1] = bgra[i + 2] = 30; // The window's dark ground.
            bgra[i + 3] = 255;
        }
        const int shown = static_cast<int>(messages.size());
        for (int k = 0; k < shown && k < 6; ++k) {
            const Message& m = messages[static_cast<std::size_t>(shown - 1 - k)];
            const int top = kHeight - 20 * (k + 1) + 2;
            for (int y = top; y < top + 12; ++y) {
                for (int block = 0; block < 66; ++block) {
                    if (!lit(m.id, m.variant, block)) {
                        continue;
                    }
                    for (int x = MessageLines::kLeft + block * 3; x < MessageLines::kLeft + block * 3 + 3; ++x) {
                        std::uint8_t* p = bgra.data() + (static_cast<std::size_t>(y) * kWidth + x) * 4;
                        p[0] = m.yellow ? 40 : 220;
                        p[1] = 220;
                        p[2] = 220;
                    }
                }
            }
        }
        return ColourPart{{0, 0, kWidth, kHeight}, bgra.data(), static_cast<std::size_t>(kWidth) * 4};
    }
};

std::vector<bool> freshOf(const MessageLines& lines) {
    std::vector<bool> out;
    for (const MessageLines::Line& line : lines.lines()) {
        out.push_back(line.fresh);
    }
    return out;
}

} // namespace

TEST_CASE("A pickup message says what and how many") {
    std::string item;
    std::int64_t amount = 0;
    REQUIRE(PickupReader::parse("You have obtained 69 Adena.", item, amount));
    CHECK(item == "Adena");
    CHECK(amount == 69);
    REQUIRE(PickupReader::parse("You have earned 1st Onyx Talisman.", item, amount));
    CHECK(item == "1st Onyx Talisman");
    CHECK(amount == 1);
    REQUIRE(PickupReader::parse("You have earned 3 Animal Bone.", item, amount));
    CHECK(item == "Animal Bone");
    CHECK(amount == 3);
    REQUIRE(PickupReader::parse("You have obtained 1,234 adena", item, amount)); // Thousands, the name in any case.
    CHECK(item == "Adena");
    CHECK(amount == 1234);
    REQUIRE(PickupReader::parse("You have obtalned 7 Adena.", item, amount)); // A letter read wrong.
    CHECK(amount == 7);
    REQUIRE(PickupReader::parse("You have ear ned Imp Shackles", item, amount)); // A space inside the word.
    CHECK(item == "Imp Shackles");
    CHECK_FALSE(PickupReader::parse("leq has inflicted 115 damage on Goblin Lookout.", item, amount));
    CHECK_FALSE(PickupReader::parse("You have obtained", item, amount)); // No name.
    CHECK_FALSE(PickupReader::parse("You have obtained 69.", item, amount));

    CHECK(pickupText(Pickup{{'A', 'd', 'e', 'n', 'a'}, 69}) == "69 Adena");
    CHECK(pickupText(Pickup{{'B', 'o', 'n', 'e'}, 1}) == "Bone");
}

TEST_CASE("Only messages that came since the image before are new") {
    Window window;
    for (int id = 1; id <= 6; ++id) {
        window.messages.push_back({id});
    }
    MessageLines lines;
    ColourPart part = window.draw();
    lines.look(&part);
    REQUIRE(lines.lines().size() == 6);
    CHECK(freshOf(lines) == std::vector<bool>(6, false)); // The first look: none (old messages are not read again).

    part = window.draw();
    lines.look(&part);
    CHECK(freshOf(lines) == std::vector<bool>(6, false)); // Nothing came.

    window.messages.push_back({7, 0, true});
    part = window.draw();
    lines.look(&part);
    REQUIRE(lines.lines().size() == 6);
    CHECK(freshOf(lines) == std::vector<bool>{false, false, false, false, false, true});
    CHECK_FALSE(lines.lines().back().white);
    CHECK(lines.lines().front().white);

    // One like it (its last letters only differ: "72 Adena" after "69 Adena"), then the same one again: each new.
    window.messages.push_back({7, 1, true});
    part = window.draw();
    lines.look(&part);
    CHECK(freshOf(lines) == std::vector<bool>{false, false, false, false, false, true});
    window.messages.push_back({7, 1, true});
    part = window.draw();
    lines.look(&part);
    CHECK(freshOf(lines) == std::vector<bool>{false, false, false, false, false, true});

    // Two at once.
    window.messages.push_back({8});
    window.messages.push_back({9, 0, true});
    part = window.draw();
    lines.look(&part);
    CHECK(freshOf(lines) == std::vector<bool>{false, false, false, false, true, true});

    // Not seen for a while, then as it was: nothing new; then one more.
    lines.look(nullptr);
    CHECK(lines.lines().empty());
    part = window.draw();
    lines.look(&part);
    CHECK(freshOf(lines) == std::vector<bool>(6, false));
    window.messages.push_back({10});
    part = window.draw();
    lines.look(&part);
    CHECK(freshOf(lines) == std::vector<bool>{false, false, false, false, false, true});
}
