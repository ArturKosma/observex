#include "core/ClipGuesses.h"
#include "core/ClipLines.h"
#include "core/Costs.h"

#include <doctest/doctest.h>

#include <chrono>
#include <cstring>
#include <string>
#include <vector>

using namespace observex;

namespace {

Cost cost(const char* name, float each, float slowest, float perSecond) {
    Cost made;
    nameCost(made, name);
    made.eachMs = each;
    made.slowestMs = slowest;
    made.perSecond = perSecond;
    return made;
}

void spin(double ms) {
    const auto until = std::chrono::steady_clock::now() + std::chrono::duration<double, std::milli>(ms);
    while (std::chrono::steady_clock::now() < until) {
    }
}

} // namespace

TEST_CASE("a meter adds each lap to its step and each run's whole time, then starts over") {
    CostMeter meter({"First", "Second"});
    for (int run = 0; run < 2; ++run) {
        meter.begin();
        spin(2.0);
        meter.lap(0);
        spin(1.0);
        meter.lap(1);
        meter.end();
    }
    meter.begin(); // Begun, never ended: not a run; its lap counts.
    meter.lap(1);

    const CostPart part = meter.take(2.0);
    REQUIRE(part.stepCount == 2);
    CHECK(std::strcmp(part.steps[0].name.data(), "First") == 0);
    CHECK(std::strcmp(part.steps[1].name.data(), "Second") == 0);
    CHECK(part.run.perSecond == doctest::Approx(1.0f)); // 2 runs over 2 s.
    CHECK(part.steps[0].perSecond == doctest::Approx(1.0f));
    CHECK(part.steps[1].perSecond == doctest::Approx(1.5f));
    CHECK(part.steps[0].eachMs >= 2.0f);
    CHECK(part.run.eachMs >= part.steps[0].eachMs + 1.0f);
    CHECK(part.run.slowestMs >= part.run.eachMs);

    const CostPart next = meter.take(1.0);
    CHECK(next.run.perSecond == 0.0f);
    CHECK(next.run.eachMs == 0.0f);
    CHECK(next.steps[1].perSecond == 0.0f);
}

TEST_CASE("a meter keeps at most kMaxSteps steps and ignores steps it does not have") {
    CostMeter meter({"1", "2", "3", "4", "5", "6", "7", "8", "9", "10", "11", "12", "13", "14", "15"});
    meter.begin();
    meter.lap(-1);
    meter.lap(20);
    meter.end();
    const CostPart part = meter.take(1.0);
    CHECK(part.stepCount == CostPart::kMaxSteps);
    CHECK(part.run.perSecond == doctest::Approx(1.0f));
}

TEST_CASE("the total is each part's run added up, and busy is each x runs") {
    CostPart capture;
    capture.run = cost("Capture", 0.05f, 0.1f, 240.0f);
    CostPart characters;
    characters.run = cost("Characters", 3.5f, 5.0f, 200.0f);
    const std::vector<CostPart> parts{capture, characters};
    CHECK(costTotalMs(parts) == doctest::Approx(3.55f));
    CHECK(characters.run.busy() == doctest::Approx(0.7f));
}

TEST_CASE("a name too long for a cost is cut") {
    Cost long_;
    nameCost(long_, "A name much longer than thirty-one letters in all");
    CHECK(std::strlen(long_.name.data()) == long_.name.size() - 1);
}

TEST_CASE("cost lines read back, spaces in names and all, without the steps that did not run") {
    CostPart part;
    part.run = cost("UI and stats", 5.25f, 9.5f, 10.0f);
    part.steps[0] = cost("Grey image, read back", 1.125f, 2.0f, 10.0f);
    part.steps[1] = cost("Text read (OCR)", 0.0f, 0.0f, 0.0f); // Did not run.
    part.steps[2] = cost("Finding", 4.0f, 7.5f, 10.0f);
    part.stepCount = 3;
    std::string text;
    appendCost(text, 1234.5, part);
    CHECK(text == "cost 1234.5 UI_and_stats 5.250 9.500 10.0 Grey_image,_read_back 1.125 2.000 10.0 Finding 4.000 "
                  "7.500 10.0\n");

    double timeMs = 0.0;
    CostPart read;
    REQUIRE(readCost(std::string_view(text).substr(0, text.size() - 1), timeMs, read));
    CHECK(timeMs == doctest::Approx(1234.5));
    CHECK(std::strcmp(read.run.name.data(), "UI and stats") == 0);
    CHECK(read.run.eachMs == doctest::Approx(5.25f));
    CHECK(read.run.slowestMs == doctest::Approx(9.5f));
    CHECK(read.run.perSecond == doctest::Approx(10.0f));
    REQUIRE(read.stepCount == 2);
    CHECK(std::strcmp(read.steps[0].name.data(), "Grey image, read back") == 0);
    CHECK(std::strcmp(read.steps[1].name.data(), "Finding") == 0);
    CHECK(read.steps[1].eachMs == doctest::Approx(4.0f));

    CHECK_FALSE(readCost("cost 12.5", timeMs, read));
    CHECK_FALSE(readCost("note 12.5 cost", timeMs, read));
}

TEST_CASE("a clip's text keeps its cost lines, and a frame shows the look whose second holds it") {
    CostPart capture;
    capture.run = cost("Capture", 0.05f, 0.1f, 240.0f);
    CostPart app;
    app.run = cost("App frame", 1.5f, 3.0f, 144.0f);
    std::string text;
    appendFrame(text, 1, 10, 0.0);
    for (const double at : {-400.0, 600.0, 1600.0}) {
        appendCost(text, at, capture);
        appendCost(text, at, app);
    }
    const ClipText clip = readClipText(text);
    REQUIRE(clip.costs.size() == 6);

    const std::span<const ClipCostLine> early = recordedCosts(clip.costs, -1000.0);
    REQUIRE(early.size() == 2);
    CHECK(early[0].timeMs == doctest::Approx(-400.0));
    const std::span<const ClipCostLine> middle = recordedCosts(clip.costs, 100.0);
    REQUIRE(middle.size() == 2);
    CHECK(middle[0].timeMs == doctest::Approx(600.0));
    CHECK(std::strcmp(middle[1].part.run.name.data(), "App frame") == 0);
    const std::span<const ClipCostLine> exact = recordedCosts(clip.costs, 600.0);
    REQUIRE(exact.size() == 2);
    CHECK(exact[0].timeMs == doctest::Approx(600.0));
    const std::span<const ClipCostLine> late = recordedCosts(clip.costs, 5000.0); // After the last: the last.
    REQUIRE(late.size() == 2);
    CHECK(late[0].timeMs == doctest::Approx(1600.0));
    CHECK(recordedCosts({}, 100.0).empty());
}
