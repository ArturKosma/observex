#include "core/TargetWatch.h"

#include <doctest/doctest.h>

#include <string>
#include <vector>

using namespace observex;

namespace {

// A character 100 x 200 with its middle at x, its top at 500.
WatchedThing thing(std::uint32_t id, int x) { return {id, {x - 50, 500, x + 50, 700}}; }

// The plate read for the look at `look`: over the head of the character with its middle at x (its orbs' row 470).
NamePlate plateOver(std::uint32_t look, int x, bool selected = true, int row = 470) {
    NamePlate plate;
    plate.look = look;
    plate.seen = true;
    plate.selected = selected;
    plate.at = {x - 60, row - 10, x + 60, row + 10};
    return plate;
}

// No plate read for the look at `look`.
NamePlate noPlate(std::uint32_t look) {
    NamePlate plate;
    plate.look = look;
    return plate;
}

// The watch, one stats reading (a new screen image) a step.
struct Watch {
    TargetWatch watch;
    std::vector<WatchedThing> things{thing(1, 300), thing(2, 800), thing(3, 1300)};
    std::uint64_t image = 0;
    std::string target = "Imp Elder";
    bool hasTarget = true;
    std::string log;

    TargetWatch::Output step(float hp = 1.0f, const NamePlate& plate = {}) {
        TargetWatch::Input in;
        in.image = ++image;
        in.hasTarget = hasTarget;
        in.target = target;
        in.targetHp = hp;
        in.plate = plate;
        in.things = things;
        const TargetWatch::Output out = watch.step(in);
        if (!out.log.empty()) {
            log += out.log + "\n";
        }
        return out;
    }
};

} // namespace

TEST_CASE("the target's box is found by looking at the boxes in turn until its plate is selected over one") {
    Watch w;
    TargetWatch::Output out = w.step();
    CHECK(out.look.id == 1);
    CHECK(out.look.over == w.things[0].box);
    CHECK(out.target == 0);
    out = w.step(1.0f, noPlate(1));
    CHECK(out.look.id == 2);
    out = w.step(1.0f, plateOver(2, 800, false)); // Grey orbs: only hovered.
    CHECK(out.look.id == 3);
    CHECK(out.target == 0);
    out = w.step(1.0f, plateOver(3, 1300));
    CHECK(out.target == 3);
    CHECK(out.look.id == 3); // From now on its plate is checked on every reading.
    CHECK(out.look.over == w.things[2].box);
    CHECK(w.log == "Target Imp Elder is box 3\n");
    out = w.step(1.0f, plateOver(3, 1300));
    CHECK(out.target == 3);
    CHECK(out.log.empty()); // Said once.
}

TEST_CASE("looking for the target goes round every box again once all were looked at") {
    Watch w;
    std::vector<std::uint32_t> looks;
    for (int i = 0; i < 5; ++i) {
        looks.push_back(w.step(1.0f, noPlate(looks.empty() ? 0 : looks.back())).look.id);
    }
    CHECK(looks == std::vector<std::uint32_t>{1, 2, 3, 1, 2});
    w.things.clear();
    CHECK(w.step().look.id == 0); // Nothing on the screen: nothing to look at.
}

TEST_CASE("a selected plate's box is the one nearest under it, whichever box was asked about") {
    Watch w;
    w.things = {thing(1, 300), thing(4, 360)}; // Side by side, overlapping.
    CHECK(w.step(1.0f, plateOver(1, 360)).target == 4);
    // A plate answering another asker's look (Select Target's) tells as much.
    Watch other;
    CHECK(other.step(1.0f, plateOver(2, 1300)).target == 3);
    // Not over any: too far up over its head, or lower than a third down it.
    Watch high;
    CHECK(high.step(1.0f, plateOver(1, 300, true, 300)).target == 0);
    Watch low;
    CHECK(low.step(1.0f, plateOver(1, 300, true, 580)).target == 0);
}

TEST_CASE("the target's death marks its box dead, once") {
    Watch w;
    REQUIRE(w.step(1.0f, plateOver(2, 800)).target == 2);
    CHECK_FALSE(w.step(0.4f, plateOver(2, 800)).died);
    TargetWatch::Output out = w.step(0.0f, plateOver(2, 800));
    CHECK_FALSE(out.died); // One empty reading is not yet a death.
    CHECK(out.dead == 0);
    out = w.step(0.0f, plateOver(2, 800));
    CHECK(out.died);
    CHECK(out.dead == 2);
    CHECK(out.log == "Target died: Imp Elder; box 2: marked dead");
    CHECK(out.look.id == 0); // Nothing more to look at.
    for (int i = 0; i < 5; ++i) {
        out = w.step(0.0f);
        CHECK_FALSE(out.died);
        CHECK(out.dead == 0);
        CHECK(out.target == 2);
    }
    // The tracker lost the corpse and follows it again as another: looked for, and marked too.
    w.things[1] = thing(7, 800);
    out = w.step(0.0f);
    CHECK(out.target == 0);
    CHECK(out.look.id == 1);
    out = w.step(0.0f, plateOver(1, 800)); // Asked about 1, read over 7.
    CHECK(out.dead == 7);
    CHECK(out.log == "Dead target Imp Elder is box 7: marked dead");
}

TEST_CASE("a target never read alive is not said to die, and unknown readings do not end the empty row") {
    Watch corpse; // Selected dead.
    for (int i = 0; i < 4; ++i) {
        CHECK_FALSE(corpse.step(0.0f).died);
    }
    Watch w;
    w.step(1.0f);
    CHECK_FALSE(w.step(0.0f).died);
    CHECK_FALSE(w.step(-1.0f).died); // A window over the bar.
    CHECK(w.step(0.0f).died);
    Watch low;
    low.step(0.003f); // Below empty is empty: never alive.
    CHECK_FALSE(low.step(0.0f).died);
    low.step(0.02f); // Above empty: alive.
    low.step(0.0f);
    CHECK(low.step(0.0f).died);
}

TEST_CASE("died before its box was known: looked for and marked when found, or given up after a while") {
    Watch w;
    w.step(1.0f);
    w.step(0.0f);
    TargetWatch::Output out = w.step(0.0f);
    REQUIRE(out.died);
    CHECK(out.dead == 0);
    CHECK(out.log == "Target died: Imp Elder (its box not known yet: looking for its plate)");
    CHECK(out.look.id != 0);
    out = w.step(0.0f, plateOver(out.look.id, 1300));
    CHECK(out.dead == 3);
    CHECK(out.log == "Dead target Imp Elder is box 3: marked dead");

    Watch lost;
    lost.step(1.0f);
    lost.step(0.0f);
    int readings = 0;
    for (out = lost.step(0.0f); out.look.id != 0; out = lost.step(0.0f, noPlate(out.look.id))) {
        ++readings;
        REQUIRE(readings <= TargetWatch::kSearchReadings);
    }
    CHECK(readings == TargetWatch::kSearchReadings);
    CHECK(lost.log.find("Dead target Imp Elder: its box not found\n") != std::string::npos);
}

TEST_CASE("its box is looked for again once its plate is not over it, or the box is gone") {
    Watch w;
    REQUIRE(w.step(1.0f, plateOver(2, 800)).target == 2);
    for (int i = 1; i < TargetWatch::kMisses; ++i) {
        CHECK(w.step(1.0f, noPlate(2)).target == 2);
        CHECK(w.step(1.0f, noPlate(5)).target == 2); // Another look's answer is no miss.
    }
    TargetWatch::Output out = w.step(1.0f, plateOver(2, 800, false)); // Grey: not the target any more.
    CHECK(out.target == 0);
    CHECK(out.look.id == 1);

    Watch gone;
    REQUIRE(gone.step(1.0f, plateOver(2, 800)).target == 2);
    gone.things.erase(gone.things.begin() + 1);
    CHECK(gone.step(1.0f).target == 0);
}

TEST_CASE("a new target, or the same name alive again after it died, starts afresh") {
    Watch w;
    REQUIRE(w.step(1.0f, plateOver(2, 800)).target == 2);
    w.target = "Goblin";
    TargetWatch::Output out = w.step(1.0f);
    CHECK(out.target == 0);
    CHECK(out.look.id == 1);

    Watch again;
    REQUIRE(again.step(1.0f, plateOver(2, 800)).target == 2);
    again.step(0.0f);
    REQUIRE(again.step(0.0f).dead == 2);
    out = again.step(1.0f, plateOver(3, 1300)); // Another Imp Elder selected.
    CHECK(out.target == 3);
    CHECK(out.look.id == 3);
    again.step(0.0f, plateOver(3, 1300));
    out = again.step(0.0f, plateOver(3, 1300));
    CHECK(out.died);
    CHECK(out.dead == 3);
}

TEST_CASE("without a target window nothing is looked at, and a window lost for a moment keeps its box") {
    Watch w;
    REQUIRE(w.step(1.0f, plateOver(2, 800)).target == 2);
    w.hasTarget = false;
    TargetWatch::Output out = w.step(-1.0f);
    CHECK(out.look.id == 0);
    CHECK(out.target == 2);
    w.hasTarget = true;
    CHECK(w.step(1.0f, plateOver(2, 800)).target == 2);

    Watch none;
    none.target = "";
    none.hasTarget = false;
    CHECK(none.step(-1.0f).look.id == 0);
    CHECK(none.log.empty());
}

TEST_CASE("the same image again changes nothing") {
    TargetWatch watch;
    const std::vector<WatchedThing> things{thing(1, 300), thing(2, 800)};
    TargetWatch::Input in;
    in.image = 9;
    in.hasTarget = true;
    in.target = "Imp Elder";
    in.targetHp = 1.0f;
    in.plate = plateOver(2, 800);
    in.things = things;
    const TargetWatch::Output first = watch.step(in);
    REQUIRE(first.target == 2);
    in.targetHp = 0.0f;
    const TargetWatch::Output again = watch.step(in);
    CHECK(again.look == first.look);
    CHECK(again.target == 2);
    CHECK(again.log.empty());
    CHECK_FALSE(again.died);
}
