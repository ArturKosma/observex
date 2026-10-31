#include "core/StatWindow.h"

#include <doctest/doctest.h>

using namespace observex;

TEST_CASE("an empty window summarises to zeros") {
    StatWindow window;
    const StatWindow::Summary summary = window.take();
    CHECK(summary.count == 0u);
    CHECK(summary.average == 0.0f);
    CHECK(summary.max == 0.0f);
}

TEST_CASE("take gives count, average and max, then starts over") {
    StatWindow window;
    window.add(1.0f);
    window.add(2.0f);
    window.add(6.0f);

    const StatWindow::Summary summary = window.take();
    CHECK(summary.count == 3u);
    CHECK(summary.average == doctest::Approx(3.0f));
    CHECK(summary.max == 6.0f);

    window.add(0.5f);
    const StatWindow::Summary next = window.take();
    CHECK(next.count == 1u);
    CHECK(next.max == 0.5f);
}
