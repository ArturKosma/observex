#include "core/Attacker.h"

#include <doctest/doctest.h>

#include <vector>

using namespace observex;

TEST_CASE("the attacker is the nearest monster within reach") {
    // The user's clip 2026-11-16_14-04-25: an Imp beside the own character, another 640 game units away.
    std::vector<AttackerCandidate> monsters{{161, {1450, 260, 1500, 330}, 640.0f / kGameUnits},
                                            {172, {1300, 710, 1400, 860}, 1.2f},
                                            {173, {1500, 700, 1560, 800}, 3.0f}};
    CHECK(nearestAttacker(monsters) == 1);
    monsters.erase(monsters.begin() + 1);
    CHECK(nearestAttacker(monsters) == 1); // 173, 141 game units away.
    monsters.pop_back();
    CHECK(nearestAttacker(monsters) == -1); // Only the far one: none.
    CHECK(nearestAttacker(std::vector<AttackerCandidate>{{5, {}, -1.0f}}) == -1); // No distance: none.
    CHECK(nearestAttacker(std::vector<AttackerCandidate>{}) == -1);
}

TEST_CASE("without the ground camera the distance is taken on the screen, in own heights") {
    const StatsArea own{1230, 700, 1330, 870}; // 170 tall.
    // Beside it, feet level: half its width across.
    CHECK(screenDistance({1330, 760, 1430, 870}, own) == doctest::Approx(100.0 / 170.0));
    // Feet 85 lower: as far as twice that, the camera looking down.
    CHECK(screenDistance({1230, 800, 1330, 955}, own) == doctest::Approx(1.0));
    // A monster far up the screen is out of reach.
    CHECK(screenDistance({1450, 260, 1500, 330}, own) > kAttackerReach);
}

TEST_CASE("an attacker over the own character is pointed at beside it") {
    const StatsArea own{1230, 700, 1330, 870};
    float x = 0.0f;
    float y = 0.0f;
    // The clip's Imp: from x 1300 to 1400, its left 30 under the own box: the 70 right of it, at their middle.
    aimBeside({1300, 710, 1400, 860}, &own, x, y);
    CHECK(x == doctest::Approx(1365.0));
    CHECK(y == doctest::Approx(785.0));
    // Clear of it: its middle.
    aimBeside({1500, 700, 1560, 800}, &own, x, y);
    CHECK(x == doctest::Approx(1530.0));
    // Almost all under it (less than kAimWidth beside): its middle all the same.
    aimBeside({1235, 710, 1340, 860}, &own, x, y);
    CHECK(x == doctest::Approx(1287.5));
    // Sticking out left: the part left of it.
    aimBeside({1180, 710, 1300, 860}, &own, x, y);
    CHECK(x == doctest::Approx(1205.0));
    // No own box: its middle.
    aimBeside({1300, 710, 1400, 860}, nullptr, x, y);
    CHECK(x == doctest::Approx(1350.0));
}
