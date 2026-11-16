#include "core/TargetHitWatch.h"

#include <doctest/doctest.h>

#include <optional>

using namespace observex;

namespace {

// A reading of target `name` at `seconds`: its bar `hp` (below 0: not read), the own MP `mp` (below 0: not read), the
// cast bar shown or not.
TargetHitWatch::Reading at(double seconds, float hp, int mp, bool casting, const char* name = "Goblin") {
    TargetHitWatch::Reading reading;
    reading.seconds = seconds;
    reading.hasTarget = true;
    reading.target = name;
    reading.targetHp = hp;
    reading.mp = mp;
    reading.casting = casting;
    return reading;
}

bool hitAt(TargetHitWatch& watch, const TargetHitWatch::Reading& reading) { return watch.read(reading).has_value(); }

} // namespace

TEST_CASE("Target hit by others: a fall mid-cast with no MP taken, said once kOurs after it showed") {
    // Clip 2026-11-15_20-53-34: Wind Strike cast from 0.0 s, a Goblin's bar fell 100% -> 68% at a fifth of the cast.
    TargetHitWatch watch;
    CHECK_FALSE(hitAt(watch, at(0.0, 1.0f, 171, true)));
    CHECK_FALSE(hitAt(watch, at(0.1, 1.0f, 171, true)));
    CHECK_FALSE(hitAt(watch, at(0.2, 0.6812f, 171, true))); // Lower, read once.
    CHECK_FALSE(hitAt(watch, at(0.3, 0.6812f, 171, true))); // Taken; the own sign may still come.
    CHECK_FALSE(hitAt(watch, at(0.4, 0.6812f, 171, true)));
    const std::optional<TargetHitWatch::Hit> hit = watch.read(at(0.5, 0.6812f, 171, true));
    REQUIRE(hit.has_value());
    CHECK(hit->from == doctest::Approx(1.0f));
    CHECK(hit->to == doctest::Approx(0.6812f));
    CHECK(hit->seconds == doctest::Approx(0.2));
    CHECK(hit->text == "Target hit by others: Goblin 100% to 68%");
    CHECK_FALSE(hitAt(watch, at(0.6, 0.6812f, 171, true))); // Said once.
}

TEST_CASE("Target hit by others: the own hit, with its MP taken or its cast ending near it, says nothing") {
    SUBCASE("MP taken on the reading the bar fell") {
        TargetHitWatch watch;
        hitAt(watch, at(0.0, 0.54f, 167, true));
        hitAt(watch, at(0.1, 0.54f, 167, true));
        for (int i = 2; i < 12; ++i) {
            CHECK_FALSE(hitAt(watch, at(0.1 * i, 0.041f, 159, false)));
        }
    }
    SUBCASE("MP not read, the cast bar gone 0.2 s after the fall") {
        TargetHitWatch watch;
        hitAt(watch, at(0.0, 0.29f, -1, true));
        hitAt(watch, at(0.1, 0.29f, -1, true));
        CHECK_FALSE(hitAt(watch, at(0.2, 0.0f, -1, true)));
        CHECK_FALSE(hitAt(watch, at(0.3, 0.0f, -1, true)));
        CHECK_FALSE(hitAt(watch, at(0.4, 0.0f, -1, false)));
        for (int i = 5; i < 12; ++i) {
            CHECK_FALSE(hitAt(watch, at(0.1 * i, 0.0f, -1, false)));
        }
    }
    SUBCASE("MP taken just before the fall showed") {
        TargetHitWatch watch;
        hitAt(watch, at(0.0, 1.0f, 140, true));
        hitAt(watch, at(0.1, 1.0f, 132, false));
        for (int i = 2; i < 10; ++i) {
            CHECK_FALSE(hitAt(watch, at(0.1 * i, 0.42f, 132, false)));
        }
    }
}

TEST_CASE("Target hit by others: MP taken at the cast's start long before, and a cast bar held at its end, are no own "
          "hit") {
    SUBCASE("the MP the cast took when it began, 0.7 s before") {
        TargetHitWatch watch;
        hitAt(watch, at(0.0, 1.0f, 171, false));
        hitAt(watch, at(0.1, 1.0f, 169, true));
        hitAt(watch, at(0.8, 0.68f, 169, true));
        hitAt(watch, at(0.9, 0.68f, 169, true));
        hitAt(watch, at(1.0, 0.68f, 169, true));
        CHECK(hitAt(watch, at(1.1, 0.68f, 169, true)));
    }
    SUBCASE("the cast bar at its end, the own hit 0.6 s after another's") {
        // Clip 2026-11-15_20-53-34 again: 68% -> 7% with the bar full and no MP taken; the own Wind Strike after.
        TargetHitWatch watch;
        hitAt(watch, at(0.0, 0.6812f, 173, true));
        hitAt(watch, at(0.1, 0.6812f, 173, true));
        hitAt(watch, at(0.2, 0.0746f, 173, true));
        hitAt(watch, at(0.3, 0.0746f, 173, true));
        hitAt(watch, at(0.4, 0.0746f, 173, true));
        CHECK(hitAt(watch, at(0.5, 0.0744f, 173, true)));
        CHECK_FALSE(hitAt(watch, at(0.8, 0.0f, 165, false))); // The own.
        CHECK_FALSE(hitAt(watch, at(0.9, 0.0f, 165, false)));
        CHECK_FALSE(hitAt(watch, at(1.5, 0.0f, 165, false)));
    }
}

TEST_CASE("Target hit by others: a misread, a rise, an unread bar, a target's first reading and another target say "
          "nothing") {
    TargetHitWatch watch;
    CHECK_FALSE(hitAt(watch, at(0.0, 0.5f, 100, false))); // The first only starts it.
    CHECK_FALSE(hitAt(watch, at(0.1, 0.5f, 100, false)));
    // A misread on one reading.
    CHECK_FALSE(hitAt(watch, at(0.2, 0.2f, 100, false)));
    for (int i = 3; i < 8; ++i) {
        CHECK_FALSE(hitAt(watch, at(0.1 * i, 0.5f, 100, false)));
    }
    // Regeneration.
    CHECK_FALSE(hitAt(watch, at(0.8, 0.55f, 100, false)));
    for (int i = 9; i < 14; ++i) {
        CHECK_FALSE(hitAt(watch, at(0.1 * i, 0.55f, 100, false)));
    }
    // Not read (a window over the bar), a drift under kMinFall.
    for (int i = 14; i < 18; ++i) {
        CHECK_FALSE(hitAt(watch, at(0.1 * i, -1.0f, 100, false)));
    }
    for (int i = 18; i < 24; ++i) {
        CHECK_FALSE(hitAt(watch, at(0.1 * i, 0.545f, 100, false)));
    }
    // A new target, much lower: only a start.
    for (int i = 24; i < 30; ++i) {
        CHECK_FALSE(hitAt(watch, at(0.1 * i, 0.3f, 100, false, "Imp")));
    }
    // A fall seen as the target changes is dropped.
    hitAt(watch, at(3.0, 0.1f, 100, false, "Imp"));
    hitAt(watch, at(3.1, 0.1f, 100, false, "Imp"));
    for (int i = 32; i < 40; ++i) {
        CHECK_FALSE(hitAt(watch, at(0.1 * i, 0.1f, 100, false, "Elder Imp")));
    }
    // No target window: nothing read, and a fall after it comes back still counts.
    TargetHitWatch::Reading none = at(4.0, -1.0f, 100, false, "Elder Imp");
    none.hasTarget = false;
    CHECK_FALSE(hitAt(watch, none));
    hitAt(watch, at(4.1, 0.05f, 100, false, "Elder Imp"));
    hitAt(watch, at(4.2, 0.05f, 100, false, "Elder Imp"));
    hitAt(watch, at(4.3, 0.05f, 100, false, "Elder Imp"));
    CHECK(hitAt(watch, at(4.4, 0.05f, 100, false, "Elder Imp")));
}

TEST_CASE("Target hit by others: falls within kOurs of one another are one hit") {
    TargetHitWatch watch;
    hitAt(watch, at(0.0, 1.0f, 100, false));
    hitAt(watch, at(0.1, 1.0f, 100, false));
    CHECK_FALSE(hitAt(watch, at(0.2, 0.9f, 100, false)));
    CHECK_FALSE(hitAt(watch, at(0.3, 0.9f, 100, false)));
    CHECK_FALSE(hitAt(watch, at(0.4, 0.8f, 100, false)));
    CHECK_FALSE(hitAt(watch, at(0.5, 0.8f, 100, false)));
    CHECK_FALSE(hitAt(watch, at(0.6, 0.8f, 100, false)));
    const std::optional<TargetHitWatch::Hit> hit = watch.read(at(0.7, 0.8f, 100, false));
    REQUIRE(hit.has_value());
    CHECK(hit->from == doctest::Approx(1.0f));
    CHECK(hit->to == doctest::Approx(0.8f));
    CHECK(hit->seconds == doctest::Approx(0.2));
    CHECK_FALSE(hitAt(watch, at(0.8, 0.8f, 100, false)));
}
