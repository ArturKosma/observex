#include "core/Mirror.h"

#include <doctest/doctest.h>

#include <vector>

using namespace observex;

TEST_CASE("mirrorDisagreement is 0 when both views saw the same") {
    const std::vector<Detection> seen{{10, 10, 40, 80, 0.9f}, {200, 50, 30, 60, 0.5f}};
    CHECK(mirrorDisagreement(seen, seen) == 0.0f);
    CHECK(mirrorDisagreement({}, {}) == 0.0f);
}

TEST_CASE("mirrorDisagreement counts a thing in both views twice and a missing one once, then halves") {
    const std::vector<Detection> plain{{10, 10, 40, 80, 0.9f}};
    // The same thing, shifted a little, seen 50% sure: 0.4 from each side.
    CHECK(mirrorDisagreement(plain, std::vector<Detection>{{12, 10, 40, 80, 0.5f}}) == doctest::Approx(0.4f));
    // Not seen in the mirror image: 0.9 from one side only.
    CHECK(mirrorDisagreement(plain, {}) == doctest::Approx(0.45f));
    CHECK(mirrorDisagreement({}, plain) == doctest::Approx(0.45f));
}

TEST_CASE("mirrorDisagreement leaves out things under minSure but still pairs with them") {
    const std::vector<Detection> plain{{10, 10, 40, 80, 0.9f}};
    // Seen 20% sure mirrored: only the plain thing counts, against 0.2.
    CHECK(mirrorDisagreement(plain, std::vector<Detection>{{10, 10, 40, 80, 0.2f}}) == doctest::Approx(0.35f));
    // Both under minSure: nothing counts.
    CHECK(mirrorDisagreement(std::vector<Detection>{{10, 10, 40, 80, 0.25f}},
                             std::vector<Detection>{{300, 10, 40, 80, 0.2f}}) == 0.0f);
}

TEST_CASE("mirrorDisagreement pairs with the most overlapping thing, at least minOverlap") {
    const std::vector<Detection> plain{{0, 0, 100, 100, 0.8f}};
    // Overlaps 0.81 and 0.2: pairs with the first (0.1 from the plain side); the second is too far apart to pair
    // and counts 0.3 on its own; the first pairs back (0.1).
    const std::vector<Detection> mirrored{{0, 0, 90, 90, 0.7f}, {50, 0, 100, 50, 0.3f}};
    CHECK(mirrorDisagreement(plain, mirrored) == doctest::Approx((0.1f + 0.1f + 0.3f) * 0.5f));
}

TEST_CASE("mirrorDisagreement compares how sure each view was of the same kind") {
    // Kinds: 0 monster, 1 player. Monster 90% (player 5%) against player 90% (monster 5%): 0.85 from each side.
    const std::vector<Detection> monster{{10, 10, 40, 80, 0.9f, 0, 1, 0.05f}};
    const std::vector<Detection> player{{10, 10, 40, 80, 0.9f, 1, 0, 0.05f}};
    CHECK(mirrorDisagreement(monster, player) == doctest::Approx(0.85f));
    // Torn the same way in both views, but with the order of its two kinds swapped: only 0.05 apart each way.
    const std::vector<Detection> torn{{10, 10, 40, 80, 0.5f, 0, 1, 0.45f}};
    const std::vector<Detection> swapped{{10, 10, 40, 80, 0.5f, 1, 0, 0.45f}};
    CHECK(mirrorDisagreement(torn, swapped) == doctest::Approx(0.05f));
    // Neither of its two surest kinds is the other's: against 0.
    const std::vector<Detection> npc{{10, 10, 40, 80, 0.9f, 2, 3, 0.05f}};
    CHECK(mirrorDisagreement(monster, npc) == doctest::Approx(0.9f));
}

TEST_CASE("closestKinds finds the thing the model is least sure which kind it is") {
    const std::vector<Detection> seen{{10, 10, 40, 80, 0.9f, 0, 1, 0.05f},  // 0.85 apart.
                                      {200, 50, 30, 60, 0.55f, 1, 3, 0.4f}, // 0.15 apart: unsure.
                                      {400, 50, 30, 60, 0.2f, 0, 1, 0.19f}}; // Under minSure: left out.
    CHECK(closestKinds(seen, 0.3f) == doctest::Approx(0.15f));
    CHECK(unsureOfKind(closestKinds(seen, 0.3f)));
    CHECK_FALSE(unsureOfKind(closestKinds(std::vector<Detection>{seen[0]}, 0.3f)));
    // Over two looks, the closer of the two.
    CHECK(closestKinds(std::vector<Detection>{seen[0]}, std::vector<Detection>{seen[1]}, 0.3f) ==
          doctest::Approx(0.15f));
    // A model without kinds is never unsure which kind.
    const std::vector<Detection> plain{{10, 10, 40, 80, 0.4f}};
    CHECK(closestKinds(plain, 0.3f) == -1.0f);
    CHECK_FALSE(unsureOfKind(closestKinds(plain, plain, 0.3f)));
    CHECK_FALSE(kindsClose(plain[0]));
}

TEST_CASE("the model cannot tell the kind of a character it is under 60% sure of, or one it says it cannot tell") {
    // 73% sure it is a character, but of no one kind: monster 55%, player 40%.
    const Detection far{10, 10, 12, 20, eitherKind(0.55f, 0.4f), 0, 1, 0.4f, 0.55f};
    CHECK(cantTellKind(far));
    CHECK(far.sure > 0.7f);
    const Detection near{10, 10, 40, 80, 0.95f, 0, 1, 0.05f}; // Monster 95%.
    CHECK_FALSE(cantTellKind(near));
    // Its kind the "can't tell" kind (here the fifth), however sure.
    const Detection unknown{10, 10, 12, 20, 0.9f, 4, 0, 0.1f};
    CHECK(cantTellKind(unknown, 4));
    CHECK_FALSE(cantTellKind(unknown)); // A model that does not know it: just a kind.
    CHECK_FALSE(cantTellKind(near, 4));
    // Exactly at the line it can tell.
    CHECK_FALSE(cantTellKind(Detection{10, 10, 12, 20, 0.6f, 0, 1, 0.1f}));
    // A model without kinds never.
    CHECK_FALSE(cantTellKind(Detection{10, 10, 40, 80, 0.3f}));
}

TEST_CASE("a model with kinds is sure it found something by its two surest kinds, of the kind by the surest") {
    // NPC 40%, monster 35%: 61% sure it is one of them.
    CHECK(eitherKind(0.4f, 0.35f) == doctest::Approx(0.61f));
    CHECK(eitherKind(0.9f, -1.0f) == doctest::Approx(0.9f)); // Without kinds, how sure it is.
    const Detection torn{10, 10, 40, 80, eitherKind(0.4f, 0.35f), 2, 0, 0.35f, 0.4f};
    CHECK(kindSureOf(torn) == doctest::Approx(0.4f));
    CHECK(kindsClose(torn)); // 0.05 apart, not 0.26.
    CHECK(closestKinds(std::vector<Detection>{torn}, 0.5f) == doctest::Approx(0.05f)); // 61% counts.
    // The mirror compares how sure each look was of the kind: 0.4 against 0.4, not 0.61.
    const Detection same{10, 10, 40, 80, 0.4f, 2};
    CHECK(mirrorDisagreement(std::vector<Detection>{torn}, std::vector<Detection>{same}) == doctest::Approx(0.0f));
}
