#include "heuristics/Tracker.h"

#include <doctest/doctest.h>

#include <array>
#include <cmath>
#include <utility>
#include <vector>

using namespace observex;

namespace {

constexpr double kLookMs = 10.0; // About 100 looks a second, as with a game on the same GPU.

Detection box(float x, float y, float sure = 0.8f, float size = 60.0f) { return {x, y, size, size * 1.5f, sure}; }

Detection ofKind(std::uint8_t kind, float sure = 0.8f) {
    Detection found = box(100, 100, sure);
    found.kind = kind;
    return found;
}

// Feeds one look and returns what the tracker shows.
std::vector<Tracked> look(Tracker& tracker, double timeMs, std::vector<Detection> found) {
    std::array<Tracked, 16> out{};
    const int count =
        tracker.update(timeMs, found.data(), static_cast<int>(found.size()), out.data(), static_cast<int>(out.size()));
    return {out.begin(), out.begin() + count};
}

} // namespace

TEST_CASE("a new thing shows once it is found on three looks in a row") {
    Tracker tracker;
    CHECK(look(tracker, 0 * kLookMs, {box(100, 100)}).empty());
    CHECK(look(tracker, 1 * kLookMs, {box(101, 100)}).empty());
    const std::vector<Tracked> shown = look(tracker, 2 * kLookMs, {box(102, 100)});
    REQUIRE(shown.size() == 1);
    CHECK(shown[0].x == doctest::Approx(102.0 + 0.1 * Tracker::kLeadMs)); // Ahead at its speed (a pixel a look).
    CHECK_FALSE(shown[0].held);
}

TEST_CASE("a new thing starts only from a sure look, then is kept while less sure") {
    Tracker tracker;
    for (int i = 0; i < 5; ++i) { // Half-believed: never starts.
        CHECK(look(tracker, i * kLookMs, {box(100, 100, 0.4f)}).empty());
    }
    CHECK(look(tracker, 5 * kLookMs, {box(100, 100, 0.6f)}).empty()); // Sure enough: starts.
    CHECK(look(tracker, 6 * kLookMs, {box(100, 100, 0.4f)}).empty());
    const std::vector<Tracked> shown = look(tracker, 7 * kLookMs, {box(100, 100, 0.4f)});
    REQUIRE(shown.size() == 1); // Three looks in a row, the first of them sure.
    CHECK(shown[0].sure < 0.6f);
}

TEST_CASE("one- and two-look flickers never show") {
    Tracker tracker;
    CHECK(look(tracker, 0 * kLookMs, {box(100, 100)}).empty());
    CHECK(look(tracker, 1 * kLookMs, {}).empty());
    CHECK(look(tracker, 2 * kLookMs, {box(500, 100)}).empty());
    CHECK(look(tracker, 3 * kLookMs, {box(500, 100)}).empty());
    CHECK(look(tracker, 4 * kLookMs, {}).empty());
    CHECK(look(tracker, 5 * kLookMs, {}).empty());
}

TEST_CASE("a thing keeps its identity, and is held through a gap where its speed takes it") {
    Tracker tracker;
    std::vector<Tracked> shown;
    for (int i = 0; i < 10; ++i) { // Moving right 5 pixels a look: 0.5 pixels per millisecond.
        shown = look(tracker, i * kLookMs, {box(100.0f + 5.0f * static_cast<float>(i), 100)});
    }
    REQUIRE(shown.size() == 1);
    const std::uint32_t id = shown[0].id;
    CHECK(shown[0].speedX == doctest::Approx(0.5f));

    shown = look(tracker, 10 * kLookMs, {}); // Not found for one look.
    REQUIRE(shown.size() == 1);
    CHECK(shown[0].id == id);
    CHECK(shown[0].held);
    CHECK(shown[0].x == doctest::Approx(150.0 + 0.5 * Tracker::kLeadMs));

    shown = look(tracker, 11 * kLookMs, {box(155, 100)});
    REQUIRE(shown.size() == 1);
    CHECK(shown[0].id == id);
    CHECK_FALSE(shown[0].held);
}

TEST_CASE("a thing is held at most as long as it was seen, and never longer than kHoldMs") {
    Tracker tracker;
    for (int i = 0; i < 3; ++i) { // Seen for 20 ms.
        look(tracker, i * kLookMs, {box(100, 100)});
    }
    CHECK(look(tracker, 3 * kLookMs, {}).size() == 1);
    CHECK(look(tracker, 4 * kLookMs, {}).size() == 1); // 20 ms unseen.
    CHECK(look(tracker, 5 * kLookMs, {}).empty());

    Tracker steady;
    for (int i = 0; i < 100; ++i) { // Seen for a second.
        look(steady, i * kLookMs, {box(100, 100)});
    }
    const double last = 99 * kLookMs;
    CHECK(look(steady, last + Tracker::kHoldMs, {}).size() == 1);
    CHECK(look(steady, last + Tracker::kHoldMs + 1.0, {}).empty());
}

TEST_CASE("a fast camera turn keeps things: they are matched where they are heading") {
    Tracker tracker;
    std::vector<Tracked> shown;
    for (int i = 0; i < 8; ++i) { // 80 pixels a look: more than the 60-pixel-wide box, so they do not even overlap.
        shown = look(tracker, i * kLookMs, {box(100.0f + 80.0f * static_cast<float>(i), 300)});
    }
    REQUIRE(shown.size() == 1);
    CHECK(shown[0].x == doctest::Approx(660.0 + 8.0 * Tracker::kLeadMs));
    CHECK(shown[0].id == 1u);
}

TEST_CASE("a box is handed out where its thing is by the time it is on the screen, and keeps up with a turn") {
    // Still, then a camera turn of 4 pixels a millisecond (40 a look), then still again.
    Tracker tracker;
    std::vector<Tracked> shown;
    float x = 100.0f;
    int i = 0;
    for (; i < 10; ++i) {
        shown = look(tracker, i * kLookMs, {box(x, 300)});
    }
    REQUIRE(shown.size() == 1);
    CHECK(shown[0].x == 100.0f); // Still: exactly where it was found.
    CHECK(shown[0].speedX == 0.0f);

    for (int turn = 0; turn < 2; ++turn, ++i) {
        x += 40.0f;
        shown = look(tracker, i * kLookMs, {box(x, 300)});
    }
    // Two looks into the turn its speed is the turn's (the speed followed 30% of each step until 2026-11-11: 2 a
    // millisecond by then), so the box is where the thing is 12 ms on.
    REQUIRE(shown.size() == 1);
    CHECK(shown[0].speedX == doctest::Approx(4.0f));
    CHECK(shown[0].x == doctest::Approx(x + 4.0 * Tracker::kLeadMs));

    for (int still = 0; still < 3; ++still, ++i) {
        shown = look(tracker, i * kLookMs, {box(x, 300)});
    }
    // Stopped: a look or two ahead of it, then exactly on it again.
    REQUIRE(shown.size() == 1);
    CHECK(shown[0].speedX == 0.0f);
    CHECK(shown[0].x == x);
}

TEST_CASE("a box's speed is a line through its last looks, so uneven steps of a steady turn even out") {
    // A turn of 3 pixels a millisecond the game shows unevenly: 45 and 15 pixels on looks 10 ms apart.
    Tracker tracker;
    std::vector<Tracked> shown;
    float x = 100.0f;
    for (int i = 0; i < 20; ++i) {
        x += i % 2 == 0 ? 45.0f : 15.0f;
        shown = look(tracker, i * kLookMs, {box(x, 300)});
    }
    REQUIRE(shown.size() == 1);
    CHECK(shown[0].speedX > 2.5f);
    CHECK(shown[0].speedX < 3.5f);
}

TEST_CASE("two things side by side keep their own identities") {
    Tracker tracker;
    std::vector<Tracked> shown;
    for (int i = 0; i < 5; ++i) {
        // Found in either order.
        if (i % 2 == 0) {
            shown = look(tracker, i * kLookMs, {box(100, 100, 0.9f), box(170, 100, 0.6f)});
        } else {
            shown = look(tracker, i * kLookMs, {box(171, 100, 0.6f), box(101, 100, 0.9f)});
        }
    }
    REQUIRE(shown.size() == 2);
    CHECK(shown[0].x == 100.0f); // Surest first.
    CHECK(shown[1].x == 170.0f);
    const std::uint32_t left = shown[0].id;
    const std::uint32_t right = shown[1].id;
    CHECK(left != right);
    shown = look(tracker, 5 * kLookMs, {box(172, 100, 0.6f)}); // The left one dips for a look.
    REQUIRE(shown.size() == 2);
    for (const Tracked& thing : shown) {
        CHECK(thing.id == (thing.x > 150.0f ? right : left));
    }
}

TEST_CASE("a much bigger box nearby is another thing, unless the followed one lies inside it") {
    Tracker tracker;
    for (int i = 0; i < 3; ++i) {
        look(tracker, i * kLookMs, {box(100, 100, 0.8f, 60.0f)});
    }
    // 200 x 300, its centre half its side away; only half of the small box lies inside it.
    const std::vector<Tracked> shown = look(tracker, 3 * kLookMs, {box(130, 100, 0.8f, 200.0f)});
    REQUIRE(shown.size() == 1); // The small one, held; the big one is new and not shown yet.
    CHECK(shown[0].width == 60.0f);
    CHECK(shown[0].held);
}

TEST_CASE("a box that lies inside a followed one is the same thing (cut by the screen edge)") {
    Tracker tracker;
    std::vector<Tracked> shown;
    for (int i = 0; i < 3; ++i) {
        shown = look(tracker, i * kLookMs, {box(100, 100, 0.8f, 60.0f)}); // 60 x 90.
    }
    REQUIRE(shown.size() == 1);
    const std::uint32_t id = shown[0].id;
    shown = look(tracker, 3 * kLookMs, {Detection{100, 100, 60, 30, 0.8f}}); // Only its top third is found.
    REQUIRE(shown.size() == 1);
    CHECK(shown[0].id == id);
    CHECK_FALSE(shown[0].held);
    CHECK(shown[0].height == 30.0f);
}

TEST_CASE("one held through a gap may turn up farther away the longer it was not seen") {
    Tracker tracker;
    for (int i = 0; i < 30; ++i) {
        look(tracker, i * kLookMs, {box(100, 100)});
    }
    const double last = 29 * kLookMs;
    // 1.2 of the larger side (90) away: too far right after it was seen, near enough after 100 ms unseen.
    CHECK(look(tracker, last + 10.0, {box(100 + 0.6f * 90.0f, 100 + 0.96f * 90.0f)}).front().held);
    Tracker gap;
    for (int i = 0; i < 30; ++i) {
        look(gap, i * kLookMs, {box(100, 100)});
    }
    look(gap, last + 50.0, {});
    const std::vector<Tracked> shown = look(gap, last + 100.0, {box(100 + 0.6f * 90.0f, 100 + 0.96f * 90.0f)});
    REQUIRE(shown.size() == 1);
    CHECK_FALSE(shown[0].held);
}

TEST_CASE("a thing keeps its identity through a gap the game stood still in, and the jump after it does not fling it") {
    Tracker tracker;
    std::vector<Tracked> shown;
    for (int i = 0; i < 6; ++i) { // A camera turn, 5 pixels a millisecond.
        shown = look(tracker, i * kLookMs, {box(100.0f + 50.0f * static_cast<float>(i), 300, 0.8f, 100.0f)});
    }
    REQUIRE(shown.size() == 1);
    const std::uint32_t id = shown[0].id;

    // No new image for 50 ms (the game stood still), and then the thing is only 16 pixels on, not 250.
    const double gapMs = 5 * kLookMs + 50.0;
    shown = look(tracker, gapMs, {box(366, 300, 0.8f, 100.0f)});
    REQUIRE(shown.size() == 1);
    CHECK(shown[0].id == id);
    CHECK_FALSE(shown[0].held);
    CHECK(shown[0].x == doctest::Approx(366.0f)); // Its line starts again: it jumped off it.

    // The camera catches up 120 pixels in 4 ms and stops: handed out where it is, not 30 pixels a millisecond on.
    shown = look(tracker, gapMs + 4.0, {box(486, 300, 0.8f, 100.0f)});
    REQUIRE(shown.size() == 1);
    CHECK(shown[0].id == id);
    CHECK(shown[0].speedX == 0.0f);
    CHECK(shown[0].x == doctest::Approx(486.0f));
    shown = look(tracker, gapMs + 8.0, {box(486, 300, 0.8f, 100.0f)});
    REQUIRE(shown.size() == 1);
    CHECK(std::abs(shown[0].x - 486.0f) < 0.01f);
}

namespace {

// Four small things standing (30 x 45, ids by where they stand) and the player's own character, larger, between them.
constexpr std::array<float, 4> kSmallX = {100.0f, 300.0f, 500.0f, 700.0f};

std::vector<Detection> standing(float shift, int skip = -1) {
    std::vector<Detection> found = {box(400, 400, 0.95f, 120.0f)}; // The player's own: the camera turns around it.
    for (int i = 0; i < static_cast<int>(kSmallX.size()); ++i) {
        if (i != skip) {
            found.push_back(box(kSmallX[static_cast<std::size_t>(i)] + shift, 200, 0.8f, 30.0f));
        }
    }
    return found;
}

std::uint32_t idAt(const std::vector<Tracked>& shown, float x) {
    for (const Tracked& thing : shown) {
        if (std::abs(thing.x - x) < 1.0f && thing.width == 30.0f) {
            return thing.id;
        }
    }
    return 0;
}

} // namespace

TEST_CASE("a camera jump keeps the things that moved alike, their lines started again") {
    Tracker tracker;
    std::vector<Tracked> shown;
    for (int i = 0; i < 5; ++i) {
        shown = look(tracker, i * kLookMs, standing(0.0f));
    }
    std::array<std::uint32_t, 4> ids{};
    for (std::size_t i = 0; i < ids.size(); ++i) {
        ids[i] = idAt(shown, kSmallX[i]);
        REQUIRE(ids[i] != 0u);
    }
    // The game stood still, then the camera jumped: the small things are 150 pixels on (over three of their sides
    // from where they were heading), the player's own character where it was.
    shown = look(tracker, 5 * kLookMs, standing(150.0f));
    REQUIRE(shown.size() == 5);
    for (std::size_t i = 0; i < ids.size(); ++i) {
        CHECK(idAt(shown, kSmallX[i] + 150.0f) == ids[i]); // Where it was found: its line starts again, standing.
    }
    for (const Tracked& thing : shown) {
        CHECK_FALSE(thing.held);
        CHECK(thing.speedX == 0.0f);
    }
}

TEST_CASE("one thing found far from where it was heading, while the others stand, is another thing") {
    Tracker tracker;
    std::vector<Tracked> shown;
    for (int i = 0; i < 5; ++i) {
        shown = look(tracker, i * kLookMs, standing(0.0f));
    }
    const std::uint32_t id = idAt(shown, kSmallX[0]);
    std::vector<Detection> found = standing(0.0f, 0);
    found.push_back(box(kSmallX[0] + 150.0f, 200, 0.8f, 30.0f));
    shown = look(tracker, 5 * kLookMs, found);
    REQUIRE(shown.size() == 5); // The new one does not show yet; the old one is held where it was.
    CHECK(idAt(shown, kSmallX[0]) == id);
    CHECK(idAt(shown, kSmallX[0] + 150.0f) == 0u);
}

TEST_CASE("a small thing left behind where it was heading is paired where the others' shift takes it") {
    // The game moves the scene unevenly: on one look things are 50 pixels further on than their speed said. Larger
    // things are still near enough (within their larger side), a small one (30 pixels) is not.
    Tracker tracker;
    std::vector<Tracked> shown;
    const auto scene = [](float shift) {
        std::vector<Detection> found;
        for (int i = 0; i < 3; ++i) {
            found.push_back(box(100.0f + 300.0f * static_cast<float>(i) + shift, 400, 0.9f, 80.0f));
        }
        found.push_back(box(150.0f + shift, 200, 0.8f, 20.0f));
        return found;
    };
    for (int i = 0; i < 5; ++i) {
        shown = look(tracker, i * kLookMs, scene(0.0f));
    }
    std::uint32_t small = 0;
    for (const Tracked& thing : shown) {
        small = thing.width == 20.0f ? thing.id : small;
    }
    REQUIRE(small != 0u);
    shown = look(tracker, 5 * kLookMs, scene(50.0f));
    REQUIRE(shown.size() == 4);
    for (const Tracked& thing : shown) {
        CHECK_FALSE(thing.held);
        if (thing.width == 20.0f) {
            CHECK(thing.id == small);
            CHECK(thing.speedX > 0.0f); // Not a jump (kJumpPixels): its line goes on through it.
        }
    }
}

TEST_CASE("in a crowd, a few lost things that happen to have boxes about as far off are not paired so") {
    // Eight small things are lost at once and eight others turn up; three of the new ones lie 250 pixels right of and
    // below three of the lost (a camera jump would have moved most of them so), the others anywhere.
    Tracker tracker;
    std::vector<Detection> crowd;
    for (int i = 0; i < 8; ++i) {
        crowd.push_back(box(100.0f + 100.0f * static_cast<float>(i), 100, 0.8f, 30.0f));
    }
    std::vector<Tracked> shown;
    for (int i = 0; i < 5; ++i) {
        shown = look(tracker, i * kLookMs, crowd);
    }
    REQUIRE(shown.size() == 8);
    std::vector<Detection> others;
    for (int i = 0; i < 3; ++i) {
        others.push_back(box(350.0f + 100.0f * static_cast<float>(i), 350, 0.8f, 30.0f));
    }
    for (const auto& [x, y] : {std::pair{130.0f, 700.0f}, std::pair{370.0f, 820.0f}, std::pair{520.0f, 760.0f},
                               std::pair{810.0f, 900.0f}, std::pair{1010.0f, 640.0f}}) {
        others.push_back(box(x, y, 0.8f, 30.0f));
    }
    shown = look(tracker, 5 * kLookMs, others);
    REQUIRE(shown.size() == 8);
    for (const Tracked& thing : shown) {
        CHECK(thing.held); // All held where they were; the new ones are not shown yet.
    }
}

TEST_CASE("a held thing is foretold at most kMaxAheadMs along its speed") {
    Tracker tracker;
    for (int i = 0; i < 20; ++i) { // 2 pixels a millisecond, seen for 190 ms.
        look(tracker, i * kLookMs, {box(100.0f + 20.0f * static_cast<float>(i), 300)});
    }
    const std::vector<Tracked> shown = look(tracker, 19 * kLookMs + 100.0, {});
    REQUIRE(shown.size() == 1);
    CHECK(shown[0].held);
    CHECK(shown[0].x == doctest::Approx(480.0 + 2.0 * (Tracker::kMaxAheadMs + Tracker::kLeadMs)));
}

TEST_CASE("a thing moving fast on the screen is held at most kFastHoldMs, one standing longer") {
    Tracker standing;
    Tracker turning;
    for (int i = 0; i < 100; ++i) { // Seen for a second each; one in a camera turn, 2 pixels a millisecond.
        look(standing, i * kLookMs, {box(100, 100)});
        look(turning, i * kLookMs, {box(100.0f + 20.0f * static_cast<float>(i), 300)});
    }
    const double last = 99 * kLookMs;
    CHECK(look(turning, last + Tracker::kFastHoldMs, {}).size() == 1);
    CHECK(look(turning, last + Tracker::kFastHoldMs + 1.0, {}).empty());
    CHECK(look(standing, last + Tracker::kFastHoldMs + 1.0, {}).size() == 1);
}

TEST_CASE("a fast thing lost for a while does not take a weak box that turns up where it is held") {
    Tracker tracker;
    for (int i = 0; i < 20; ++i) { // A turn, 2 pixels a millisecond, seen for 190 ms.
        look(tracker, i * kLookMs, {box(100.0f + 20.0f * static_cast<float>(i), 300)});
    }
    // Lost (let go on the first look over kFastHoldMs); 120 ms later something the model is 45% sure of turns up next
    // to where it was held (480 + 2 * 20).
    CHECK(look(tracker, 19 * kLookMs + 110.0, {}).empty());
    CHECK(look(tracker, 19 * kLookMs + 120.0, {box(530, 300, 0.45f)}).empty());
}

TEST_CASE("sureness is smoothed: a one-look dip moves it only part of the way") {
    Tracker tracker;
    std::vector<Tracked> shown;
    for (int i = 0; i < 20; ++i) {
        shown = look(tracker, i * kLookMs, {box(100, 100, 0.9f)});
    }
    REQUIRE(shown.size() == 1);
    CHECK(shown[0].sure == doctest::Approx(0.9f));
    shown = look(tracker, 20 * kLookMs, {box(100, 100, 0.3f)});
    REQUIRE(shown.size() == 1);
    CHECK(shown[0].sure > 0.75f);
    CHECK(shown[0].sure < 0.9f);
}

TEST_CASE("with more things than fit, the surest are handed out") {
    Tracker tracker;
    std::vector<Detection> many;
    for (int i = 0; i < 20; ++i) {
        many.push_back(box(100.0f * static_cast<float>(i), 100, 0.61f + 0.02f * static_cast<float>(i)));
    }
    std::vector<Tracked> shown;
    for (int i = 0; i < 3; ++i) {
        shown = look(tracker, i * kLookMs, many);
    }
    REQUIRE(shown.size() == 16);
    CHECK(shown.front().x == 1900.0f);
    CHECK(shown.back().x == 400.0f);
}

TEST_CASE("a thing keeps the kind it was found as lately, through a few looks of another") {
    Tracker tracker;
    std::vector<Tracked> shown;
    for (int i = 0; i < 50; ++i) { // Half a second as kind 1.
        shown = look(tracker, i * kLookMs, {ofKind(1)});
    }
    REQUIRE(shown.size() == 1);
    CHECK(shown[0].kind == 1);
    for (int i = 50; i < 55; ++i) { // 50 ms as kind 2, a bit surer: still kind 1.
        shown = look(tracker, i * kLookMs, {ofKind(2, 0.9f)});
    }
    REQUIRE(shown.size() == 1);
    CHECK(shown[0].kind == 1);
    for (int i = 55; i < 100; ++i) { // Half a second more as kind 2: now it is kind 2, the same thing.
        shown = look(tracker, i * kLookMs, {ofKind(2, 0.9f)});
    }
    REQUIRE(shown.size() == 1);
    CHECK(shown[0].kind == 2);
}

TEST_CASE("a new thing starts as the kind it was first found as") {
    Tracker tracker;
    std::vector<Tracked> shown;
    for (int i = 0; i < 3; ++i) {
        shown = look(tracker, i * kLookMs, {ofKind(3)});
    }
    REQUIRE(shown.size() == 1);
    CHECK(shown[0].kind == 3);
}

namespace {

// Found with kinds: its surest kind and how sure, and its second.
Detection withKinds(std::uint8_t kind, float kindSure, std::uint8_t secondKind, float second) {
    Detection found = box(100, 100, 0.8f);
    found.kind = kind;
    found.kindSure = kindSure;
    found.secondKind = secondKind;
    found.second = second;
    return found;
}

} // namespace

TEST_CASE("how sure of its kind it is, and its second kind, follow the model's lately, not its first look") {
    // A wolf first seen 58% sure it is a monster, then about 80% (a Lineage 2 clip, 2026-11-11): until then it showed
    // "can't tell" for as long as it was followed.
    Tracker tracker;
    std::vector<Tracked> shown = look(tracker, 0.0, {withKinds(0, 0.58f, 4, 0.02f)});
    for (int i = 1; i < 200; ++i) { // Two seconds of monster 80%, player 5%.
        shown = look(tracker, i * kLookMs, {withKinds(0, 0.8f, 1, 0.05f)});
    }
    REQUIRE(shown.size() == 1);
    CHECK(shown[0].kind == 0);
    CHECK(shown[0].kindSure == doctest::Approx(0.8f).epsilon(0.01));
    CHECK(shown[0].secondKind == 1);
    CHECK(shown[0].second == doctest::Approx(0.05f).epsilon(0.01));
    CHECK_FALSE(cantTellKind(shown[0]));

    // A tenth of a second of looks at 80% and it can tell (64%).
    Tracker again;
    shown = look(again, 0.0, {withKinds(0, 0.58f, 4, 0.02f)});
    CHECK(shown.empty());
    for (int i = 1; i <= 10; ++i) {
        shown = look(again, i * kLookMs, {withKinds(0, 0.8f, 1, 0.05f)});
    }
    REQUIRE(shown.size() == 1);
    CHECK_FALSE(cantTellKind(shown[0]));
}

TEST_CASE("a thing the model is torn between two kinds about shows it cannot tell") {
    // Monster 50% and NPC 45% on one look, the other way round on the next: about 47% sure of either.
    Tracker tracker;
    std::vector<Tracked> shown;
    for (int i = 0; i < 100; ++i) {
        shown = look(tracker, i * kLookMs,
                     {i % 2 == 0 ? withKinds(0, 0.5f, 2, 0.45f) : withKinds(2, 0.5f, 0, 0.45f)});
    }
    REQUIRE(shown.size() == 1);
    CHECK(shown[0].kindSure == doctest::Approx(0.475f).epsilon(0.02));
    CHECK(shown[0].second == doctest::Approx(0.475f).epsilon(0.02));
    CHECK(cantTellKind(shown[0]));
    CHECK(kindsClose(shown[0]));
}
