#include "core/CloseUp.h"

#include <doctest/doctest.h>

#include <array>
#include <vector>

using namespace observex;

namespace {

Detection box(float x, float y, float w, float h, float sure, std::uint8_t kind = 0) {
    Detection d{x, y, w, h, sure, kind};
    return d;
}

} // namespace

TEST_CASE("the close-up is half the screen round the own character, inside the screen") {
    const CloseUpArea area = closeUpArea(1280.0f, 780.0f, 2560, 1440, 2);
    CHECK(area.x == 640);
    CHECK(area.y == 420);
    CHECK(area.width == 1280);
    CHECK(area.height == 720);
    const CloseUpArea low = closeUpArea(1280.0f, 1400.0f, 2560, 1440, 2); // Moved up to fit.
    CHECK(low.y == 720);
    const CloseUpArea left = closeUpArea(100.0f, 700.0f, 2560, 1440, 2);
    CHECK(left.x == 0);
}

TEST_CASE("the close-up's finds replace the whole screen's inside it, the rest stays") {
    // The clip 2026-11-16_14-04-25: the whole screen saw the own character and the Imp as one box; the close-up sees
    // them apart. A far Imp (top right) and one near the close-up's side stay the whole screen's.
    const CloseUpArea area{640, 420, 1280, 720}; // Inner part: x 800-1760, y 510-1050.
    std::array<Detection, 8> whole{box(1230, 690, 130, 180, 0.88f, 3),  // Own + Imp, one box: inside, dropped.
                                   box(1450, 260, 50, 70, 0.86f),        // The far Imp: outside, kept.
                                   box(650, 900, 60, 80, 0.5f)};         // Near the side: outside the inner, kept.
    const std::vector<Detection> close{box(1232, 696, 92, 170, 0.92f, 3), // Own.
                                       box(1300, 710, 90, 120, 0.78f),    // The Imp.
                                       box(645, 905, 50, 70, 0.6f)};      // Cut by the side: left out.
    const int count = joinCloseUp(whole.data(), 3, close, area, 0.125f, static_cast<int>(whole.size()));
    REQUIRE(count == 4);
    CHECK(whole[0].sure == doctest::Approx(0.92f)); // Surest first.
    CHECK(whole[0].kind == 3);
    CHECK(whole[1].sure == doctest::Approx(0.86f));
    CHECK(whole[2].sure == doctest::Approx(0.78f));
    CHECK(whole[2].x == doctest::Approx(1300.0f));
    CHECK(whole[3].sure == doctest::Approx(0.5f));
    CHECK(whole[3].x == doctest::Approx(650.0f));
}

TEST_CASE("the close-up's finds go in only while there is room") {
    const CloseUpArea area{0, 0, 100, 100};
    std::array<Detection, 2> whole{box(500, 500, 10, 10, 0.4f)};
    const std::vector<Detection> close{box(40, 40, 10, 10, 0.9f), box(45, 45, 10, 10, 0.8f)};
    CHECK(joinCloseUp(whole.data(), 1, close, area, 0.125f, 2) == 2);
    CHECK(whole[0].sure == doctest::Approx(0.9f));
    CHECK(whole[1].sure == doctest::Approx(0.4f));
}
