#include "core/HpWatch.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <cstring>
#include <optional>

using namespace observex;

namespace {

// The Status HP gauge as read: `text` its number ("" none read), seen or not.
GameGauge hpGauge(const char* text, bool seen = true) {
    GameGauge gauge;
    std::copy_n("Status", 6, gauge.piece.begin());
    std::copy_n("HP", 2, gauge.name.begin());
    std::copy_n(text, std::min(std::strlen(text), gauge.text.size() - 1), gauge.text.begin());
    gauge.seen = seen;
    gauge.full = seen ? 0.5f : -1.0f;
    return gauge;
}

// Reads `text` at `seconds`; whether HP Lost was said, its points in *points.
bool lostAt(HpWatch& watch, const char* text, double seconds, int* points = nullptr) {
    const GameGauge gauge = hpGauge(text);
    const std::optional<HpWatch::Lost> lost = watch.read(&gauge, seconds);
    if (lost && points != nullptr) {
        *points = lost->points;
    }
    return lost.has_value();
}

} // namespace

TEST_CASE("HP lost is said once a lower number is read twice in a row, timed by the first") {
    HpWatch watch;
    CHECK_FALSE(lostAt(watch, "199/256", 0.0)); // The first only starts it,
    CHECK_FALSE(lostAt(watch, "199/256", 0.1)); // taken on the second.
    CHECK_FALSE(lostAt(watch, "187/256", 0.2)); // Lower, read once: not yet.
    const GameGauge gauge = hpGauge("187/256");
    const std::optional<HpWatch::Lost> lost = watch.read(&gauge, 0.3);
    REQUIRE(lost.has_value());
    CHECK(lost->points == 12);
    CHECK(lost->from == 199);
    CHECK(lost->to == 187);
    CHECK(lost->most == 256);
    CHECK(lost->seconds == doctest::Approx(0.2));
    CHECK(lost->text == "HP lost 12: 199/256 to 187/256");
    CHECK_FALSE(lostAt(watch, "187/256", 0.4)); // Said once.
}

TEST_CASE("HP that rose, a misread on one reading, an unread number and a lower most say nothing") {
    HpWatch watch;
    lostAt(watch, "150/256", 0.0);
    lostAt(watch, "150/256", 0.1);
    // Regeneration while sitting: it rises.
    CHECK_FALSE(lostAt(watch, "160/256", 0.2));
    CHECK_FALSE(lostAt(watch, "160/256", 0.3));
    // A digit misread on one reading, then the right number again.
    CHECK_FALSE(lostAt(watch, "100/256", 0.4));
    CHECK_FALSE(lostAt(watch, "160/256", 0.5));
    CHECK_FALSE(lostAt(watch, "160/256", 0.6));
    // Not read: the bar under a window, no number, not a number.
    const GameGauge hidden = hpGauge("", false);
    CHECK_FALSE(watch.read(&hidden, 0.7).has_value());
    CHECK_FALSE(watch.read(nullptr, 0.8).has_value());
    CHECK_FALSE(lostAt(watch, "", 0.9));
    CHECK_FALSE(lostAt(watch, "26.64%", 1.0));
    // A buff that raised the most wearing off: lower HP and a lower most is no damage.
    CHECK_FALSE(lostAt(watch, "140/230", 1.1));
    CHECK_FALSE(lostAt(watch, "140/230", 1.2));
    // From there, a hit counts again.
    int points = 0;
    CHECK_FALSE(lostAt(watch, "130/230", 1.3));
    CHECK(lostAt(watch, "130/230", 1.4, &points));
    CHECK(points == 10);
}

TEST_CASE("HP lost: unread for long starts it again") {
    HpWatch watch;
    lostAt(watch, "200/256", 0.0);
    lostAt(watch, "200/256", 0.1);
    // Another character after the character select screen, much lower: only a start.
    CHECK_FALSE(lostAt(watch, "50/120", 0.1 + HpWatch::kForgetSeconds + 1.0));
    CHECK_FALSE(lostAt(watch, "50/120", 0.2 + HpWatch::kForgetSeconds + 1.0));
    // Unread for a while but under kForgetSeconds: a fall is still a loss.
    int points = 0;
    CHECK_FALSE(lostAt(watch, "40/120", 10.0 + HpWatch::kForgetSeconds));
    CHECK(lostAt(watch, "40/120", 10.1 + HpWatch::kForgetSeconds, &points));
    CHECK(points == 10);
}
