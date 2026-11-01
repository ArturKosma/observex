#include "core/Tracker.h"

#include <doctest/doctest.h>

#include <array>
#include <vector>

using namespace observex;

namespace {

constexpr double kLookMs = 10.0; // About 100 looks a second, as with a game on the same GPU.

Detection box(float x, float y, float sure = 0.8f, float size = 60.0f) { return {x, y, size, size * 1.5f, sure}; }

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
    CHECK(shown[0].x == 102.0f);
    CHECK_FALSE(shown[0].held);
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
    CHECK(shown[0].x == doctest::Approx(150.0f));

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
    CHECK(shown[0].x == 660.0f);
    CHECK(shown[0].id == 1u);
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
        many.push_back(box(100.0f * static_cast<float>(i), 100, 0.3f + 0.03f * static_cast<float>(i)));
    }
    std::vector<Tracked> shown;
    for (int i = 0; i < 3; ++i) {
        shown = look(tracker, i * kLookMs, many);
    }
    REQUIRE(shown.size() == 16);
    CHECK(shown.front().x == 1900.0f);
    CHECK(shown.back().x == 400.0f);
}
