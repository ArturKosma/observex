#include "core/ImageView.h"

#include <doctest/doctest.h>

using namespace observex;

namespace {
constexpr float kWidth = 2560.0f;
constexpr float kHeight = 1440.0f;
} // namespace

TEST_CASE("the wheel steps through the zoom levels from 100% to 800%, no further") {
    CHECK(steppedZoom(1.0f, 1) == 1.25f);
    CHECK(steppedZoom(1.25f, 2) == 2.0f);
    CHECK(steppedZoom(2.0f, -1) == 1.5f);
    CHECK(steppedZoom(1.0f, -1) == 1.0f); // 100% is the least.
    CHECK(steppedZoom(8.0f, 1) == 8.0f);  // 800% the most.
    CHECK(steppedZoom(1.0f, 50) == 8.0f);
    CHECK(steppedZoom(2.2f, 1) == 2.5f); // Between levels: from the one just under.
}

TEST_CASE("zooming keeps the pixel under the mouse where it was in the view") {
    const ImageView fitted;
    // The mouse at the image's centre: the view stays centred on it.
    const ImageView centred = zoomedAt(fitted, 2.0f, 1280.0f, 720.0f, kWidth, kHeight);
    CHECK(centred.zoom == 2.0f);
    CHECK(centred.left == doctest::Approx(640.0f));
    CHECK(centred.top == doctest::Approx(360.0f));
    // A pixel a quarter across: still a quarter across the view after.
    const ImageView quarter = zoomedAt(fitted, 4.0f, 640.0f, 360.0f, kWidth, kHeight);
    CHECK((640.0f - quarter.left) * 4.0f / kWidth == doctest::Approx(0.25f));
    CHECK((360.0f - quarter.top) * 4.0f / kHeight == doctest::Approx(0.25f));
    // And back out to 100%: the whole image again.
    CHECK(zoomedAt(quarter, 1.0f, 700.0f, 400.0f, kWidth, kHeight) == ImageView{});
}

TEST_CASE("the view never shows space outside the image") {
    // Zooming in at a corner keeps the corner at the view's corner.
    const ImageView corner = zoomedAt(ImageView{}, 2.0f, 0.0f, 0.0f, kWidth, kHeight);
    CHECK(corner.left == 0.0f);
    CHECK(corner.top == 0.0f);
    // Moved past the right and bottom edges: back to the last place that shows only image.
    const ImageView past = keptInside(ImageView{4.0f, 5000.0f, -30.0f}, kWidth, kHeight);
    CHECK(past.left == doctest::Approx(kWidth - kWidth / 4.0f));
    CHECK(past.top == 0.0f);
    // A zoom outside the levels is brought within them.
    CHECK(keptInside(ImageView{0.5f, 100.0f, 100.0f}, kWidth, kHeight) == ImageView{});
    CHECK(keptInside(ImageView{20.0f, 0.0f, 0.0f}, kWidth, kHeight).zoom == 8.0f);
}

TEST_CASE("a picked mark out of the view is brought into it, centred; one in view leaves the view as it is") {
    const ImageView view{4.0f, 0.0f, 0.0f}; // Shows pixels 0-640 across, 0-360 down.
    CHECK(showing(view, Box{100, 100, 40, 80}, kWidth, kHeight) == view);
    const ImageView moved = showing(view, Box{1500, 900, 40, 80}, kWidth, kHeight);
    CHECK(moved.zoom == 4.0f);
    CHECK(moved.left == doctest::Approx(1520.0f - 320.0f));
    CHECK(moved.top == doctest::Approx(940.0f - 180.0f));
    // Partly out of the view counts as out.
    CHECK(showing(view, Box{620, 100, 40, 80}, kWidth, kHeight) != view);
    // At an edge of the image: as close as the image allows.
    CHECK(showing(view, Box{2540, 1420, 20, 20}, kWidth, kHeight) ==
          ImageView{4.0f, kWidth - kWidth / 4.0f, kHeight - kHeight / 4.0f});
    // At 100% everything is in view.
    CHECK(showing(ImageView{}, Box{2540, 1420, 20, 20}, kWidth, kHeight) == ImageView{});
}
