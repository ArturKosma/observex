#include "core/DropPick.h"
#include "core/GameStats.h"
#include "core/StateTree.h"

#include <doctest/doctest.h>

#include <cmath>
#include <string>
#include <vector>

using namespace observex;

namespace {

// A drop at screen (x, y) (the desktop the same), `distance` game units away, placed at (fixedX, fixedY) round the feet.
LootDrop drop(float x, float y, float distance, float fixedX, float fixedY) {
    LootDrop d;
    d.x = x;
    d.y = y;
    d.size = 20.0f;
    d.deskX = static_cast<int>(x);
    d.deskY = static_cast<int>(y);
    d.distance = distance;
    d.placed = true;
    d.fixedX = fixedX;
    d.fixedY = fixedY;
    return d;
}

// Move To Drop stepped as the app does, its cursor moved by what it says.
struct Picker {
    DropPicker picker;
    double now = 0.0;
    int cursorX = 100;
    int cursorY = 100;
    std::vector<LootDrop> drops;
    std::vector<DropPicker::Box> characters;
    int clicks = 0;
    std::string log;

    DropPicker::Output step(double dt = 0.016) {
        now += dt;
        DropPicker::Input in;
        in.now = now;
        in.dt = dt;
        in.cursorX = cursorX;
        in.cursorY = cursorY;
        in.drops = drops;
        in.reach = 80;
        in.own = true;
        in.ownX = 1280;
        in.ownY = 730;
        in.ownHeight = 130.0f;
        in.characters = characters;
        DropPicker::Output out = picker.step(in);
        cursorX += out.dx;
        cursorY += out.dy;
        clicks += out.click ? 1 : 0;
        log += out.log.empty() ? "" : out.log + "\n";
        return out;
    }
    // Steps until done or `seconds` pass; the output of the last step.
    DropPicker::Output run(double seconds) {
        DropPicker::Output out;
        for (const double end = now + seconds; now < end;) {
            out = step();
            if (out.done) {
                break;
            }
        }
        return out;
    }
};

} // namespace

TEST_CASE("Move To Drop is done at once with no drop, or the nearest within reach, saying nothing") {
    Picker p;
    p.picker.start();
    CHECK(p.step().done);
    p.picker.start();
    p.drops = {drop(1300, 760, 40.0f, 10.0f, 30.0f), drop(1800, 900, 500.0f, 400.0f, 300.0f)};
    const DropPicker::Output out = p.step();
    CHECK(out.done);
    CHECK_FALSE(out.click);
    CHECK(p.log.empty());
}

TEST_CASE("Move To Drop glides onto the nearest drop, clicks it, and is done once there") {
    Picker p;
    p.picker.start();
    p.drops = {drop(2000, 300, 900.0f, 600.0f, 700.0f), drop(1800, 900, 500.0f, 400.0f, 300.0f)};
    DropPicker::Output out;
    for (int i = 0; i < 200 && p.clicks == 0; ++i) {
        out = p.step();
        REQUIRE_FALSE(out.done);
    }
    CHECK(p.clicks == 1);
    CHECK(std::abs(p.cursorX - 1800) <= DropPicker::kOnPoint);
    CHECK(std::abs(p.cursorY - 900) <= DropPicker::kOnPoint);
    // Walking there over 1.5 s: it comes nearer on the screen (the camera follows the own character) and in
    // distance, no second click, done once within reach.
    constexpr int kSteps = 90;
    for (int k = 1; k <= kSteps && !out.done; ++k) {
        const float t = static_cast<float>(k) / kSteps;
        p.drops = {drop(2000, 300, 900.0f, 600.0f, 700.0f),
                   drop(1800.0f - 480.0f * t, 900.0f - 150.0f * t, 500.0f - 470.0f * t, 400.0f - 380.0f * t,
                        300.0f - 270.0f * t)};
        out = p.step();
    }
    CHECK(out.done);
    CHECK_FALSE(out.gaveUp);
    CHECK(p.clicks == 1);
    CHECK(p.log.find("Move To Drop: going for a drop 500 units away (the nearest of 2)") == 0);
    CHECK(p.log.find("Move To Drop: there, the drop 80 units away") != std::string::npos);
}

TEST_CASE("Move To Drop is done once the drop is gone (picked up by the click, or under the own character)") {
    Picker p;
    p.picker.start();
    p.drops = {drop(1800, 900, 500.0f, 400.0f, 300.0f)};
    while (p.clicks == 0) {
        REQUIRE_FALSE(p.step().done);
    }
    p.drops.clear();
    const DropPicker::Output early = p.step();
    CHECK_FALSE(early.done); // One look without it is no proof.
    const DropPicker::Output out = p.run(DropPicker::kGoneSeconds + 0.1);
    CHECK(out.done);
    CHECK_FALSE(out.gaveUp);
    CHECK(p.log.find("the drop is gone") != std::string::npos);
}

TEST_CASE("Move To Drop clicks again when it comes no nearer, and gives up after kClicks") {
    Picker p;
    p.picker.start();
    p.drops = {drop(1800, 900, 500.0f, 400.0f, 300.0f)};
    const DropPicker::Output out = p.run(5.0);
    CHECK(out.done);
    CHECK(out.gaveUp);
    CHECK(p.clicks == DropPicker::kClicks);
    CHECK(out.givenUp.distance == 500.0f);
    CHECK(p.log.find("clicked again (click 2)") != std::string::npos);
    CHECK(p.log.find("Move To Drop: gave up on the drop, 3 clicks and it came no nearer") != std::string::npos);
}

TEST_CASE("Move To Drop gives up on a drop not reached kGiveUp after choosing it") {
    Picker p;
    p.picker.start();
    float distance = 1000.0f;
    p.drops = {drop(1800, 900, distance, 400.0f, 900.0f)};
    DropPicker::Output out;
    for (int i = 0; i < 2000; ++i) {
        // Nearer by over a tenth a second: never a second click, never there.
        distance -= 2.0f;
        p.drops = {drop(1800, 900, distance, 400.0f, 900.0f)};
        out = p.step();
        if (out.done) {
            break;
        }
    }
    CHECK(out.gaveUp);
    CHECK(p.now >= DropPicker::kGiveUp);
    CHECK(p.log.find("not there 6 s after choosing it") != std::string::npos);
}

TEST_CASE("Move To Drop clicks the ground short of a drop a character stands over") {
    Picker p;
    p.picker.start();
    p.drops = {drop(1800, 900, 500.0f, 400.0f, 300.0f)};
    p.characters = {{1770, 820, 1830, 910}}; // Over the drop.
    while (p.clicks == 0) {
        REQUIRE_FALSE(p.step().done);
    }
    // Clear of the box, on the way back to the own feet (1280, 730).
    const bool clear = p.cursorX < 1770 || p.cursorY < 820;
    CHECK(clear);
    CHECK(p.cursorX < 1800);
    CHECK(p.cursorY < 900);
    // Covered all the way: given up on.
    Picker q;
    q.picker.start();
    q.drops = {drop(1800, 900, 500.0f, 400.0f, 300.0f)};
    q.characters = {{1200, 600, 1900, 1000}};
    const DropPicker::Output out = q.step();
    CHECK(out.gaveUp);
    CHECK(q.clicks == 0);
}

TEST_CASE("Move To Drop with no distance judges by the screen against the own character's height") {
    Picker p;
    p.picker.start();
    LootDrop nearby = drop(1300, 760, -1.0f, 0.0f, 0.0f);
    nearby.placed = false; // 36 px from the feet, a 130 px tall character: about 13 units.
    p.drops = {nearby};
    CHECK(p.step().done);
    CHECK(p.clicks == 0);
}

TEST_CASE("DropBook holds the count kHoldSeconds after the drops vanish") {
    DropBook book;
    std::vector<LootDrop> drops = {drop(1300, 760, 40.0f, 10.0f, 30.0f), drop(1800, 900, 500.0f, 400.0f, 300.0f)};
    DropBook::Output out = book.step({0.0, 1, drops, true, 80});
    CHECK(out.count == 2);
    CHECK(out.nearest == 40.0f);
    out = book.step({0.1, 2, {}, true, 80});
    CHECK(out.count == 2); // Held.
    CHECK(out.nearest < 0.0f);
    CHECK(out.counted.empty());
    out = book.step({DropBook::kHoldSeconds + 0.05, 3, {}, true, 80});
    CHECK(out.count == 0);
    // A new count replaces the held one at once.
    out = book.step({2.0, 4, std::vector<LootDrop>{drops[1]}, false, 80});
    CHECK(out.count == 1);
}

TEST_CASE("DropBook gives up on a drop within reach for kStuckSeconds while looting, also after a camera turn") {
    DropBook book;
    const LootDrop ring = drop(1300, 760, 40.0f, 10.0f, 30.0f);
    std::uint64_t look = 0;
    DropBook::Output out;
    for (double t = 0.0; t < DropBook::kStuckSeconds - 0.2; t += 0.1) {
        // The camera turns: elsewhere on the screen, the same place round the feet.
        LootDrop seen = ring;
        seen.x += static_cast<float>(t * 50.0);
        out = book.step({t, ++look, std::vector<LootDrop>{seen}, true, 80});
        REQUIRE(out.count == 1);
    }
    out = book.step({DropBook::kStuckSeconds + 0.05, ++look, std::vector<LootDrop>{ring}, true, 80});
    CHECK(out.count == 0);
    CHECK(out.counted.empty());
    CHECK(out.log.find("Looting: gave up on a drop 40 units away") == 0);
    // Left out while given up on, counted again after.
    out = book.step({10.0, ++look, std::vector<LootDrop>{ring}, true, 80});
    CHECK(out.count == 0);
    out = book.step({DropBook::kStuckSeconds + DropBook::kGiveUpSeconds + 1.0, ++look, std::vector<LootDrop>{ring}, true, 80});
    CHECK(out.count == 1);
}

TEST_CASE("DropBook's stuck watch counts only while looting, and giveUp leaves a drop out at once") {
    DropBook book;
    const LootDrop ring = drop(1300, 760, 40.0f, 10.0f, 30.0f);
    std::uint64_t look = 0;
    DropBook::Output out;
    for (double t = 0.0; t < 2.0 * DropBook::kStuckSeconds; t += 0.1) {
        out = book.step({t, ++look, std::vector<LootDrop>{ring}, false, 80});
    }
    CHECK(out.count == 1); // Not looting: never given up on.
    const LootDrop away = drop(1800, 900, 500.0f, 400.0f, 300.0f);
    out = book.step({20.0, ++look, std::vector<LootDrop>{ring, away}, false, 80});
    CHECK(out.count == 2);
    book.giveUp(away, 20.0);
    out = book.step({20.1, ++look, std::vector<LootDrop>{ring, away}, false, 80});
    CHECK(out.count == 1);
    CHECK(out.nearest == 40.0f);
}

TEST_CASE("Move To Drop is written with its reach, left out when it is the default") {
    using namespace observex::statetree;
    CHECK(std::string(nameOf(TaskKind::MoveToDrop)) == "Move To Drop");
    CHECK_FALSE(canRunParallel(TaskKind::MoveToDrop));
    Tree tree;
    const int loot = tree.add(tree.root(), "Loot");
    Task closer{TaskKind::MoveToDrop};
    Task farther{TaskKind::MoveToDrop};
    farther.reach = 150;
    tree.find(loot)->tasks = {closer, farther};
    const std::string text = writeTree(tree);
    CHECK(text.find("task 2 move-to-drop\ntask 2 move-to-drop 150\n") != std::string::npos);
    std::string error;
    const std::optional<Tree> read = readTree(text, &error);
    REQUIRE_MESSAGE(read.has_value(), error);
    CHECK(writeTree(*read) == text);
    REQUIRE(read->find(2)->tasks.size() == 2);
    CHECK(read->find(2)->tasks[0].reach == kDropReach);
    CHECK(read->find(2)->tasks[1].reach == 150);
    // Out of range: clamped.
    const std::optional<Tree> odd = readTree("observex-brain 1\nstate 1 0 Root\nstate 2 1 Loot\ntask 2 move-to-drop 5000\n",
                                             &error);
    REQUIRE_MESSAGE(odd.has_value(), error);
    CHECK(odd->find(2)->tasks[0].reach == kDropMaxReach);
}

TEST_CASE("Drops and Drop Distance are Nearby gauges, read as numbers and written as drops, drop-distance") {
    using namespace observex::statetree;
    GameStats stats;
    addDrops(stats, true, 2, 87.4f);
    Variables variables = variablesOf(stats);
    CHECK(variables.known[static_cast<int>(Variable::Drops)]);
    CHECK(variables.value[static_cast<int>(Variable::Drops)] == 2.0f);
    CHECK(variables.known[static_cast<int>(Variable::DropDistance)]);
    CHECK(variables.value[static_cast<int>(Variable::DropDistance)] == 87.0f);
    CHECK(isCount(Variable::Drops));
    CHECK(isCount(Variable::DropDistance));
    // None shows: Drop Distance not known, so every condition on it fails.
    GameStats none;
    addDrops(none, true, 0, -1.0f);
    variables = variablesOf(none);
    CHECK(variables.value[static_cast<int>(Variable::Drops)] == 0.0f);
    CHECK_FALSE(variables.known[static_cast<int>(Variable::DropDistance)]);
    // Added once.
    addDrops(none, true, 5, 10.0f);
    CHECK(variablesOf(none).value[static_cast<int>(Variable::Drops)] == 0.0f);
    // In a transition's condition, in the file.
    Tree tree;
    const int loot = tree.add(tree.root(), "Loot");
    Condition drops;
    drops.left = {Variable::Drops, 0.0f, {}};
    drops.compare = Compare::Greater;
    drops.right = {Variable::None, 0.0f, {}};
    Condition close;
    close.left = {Variable::DropDistance, 0.0f, {}};
    close.compare = Compare::Less;
    close.right = {Variable::None, 1500.0f, {}};
    tree.find(tree.root())->transitions.push_back({loot, {drops, close}});
    const std::string text = writeTree(tree);
    CHECK(text.find("if and drops > 0\nif and drop-distance < 1500\n") != std::string::npos);
    std::string error;
    const std::optional<Tree> read = readTree(text, &error);
    REQUIRE_MESSAGE(read.has_value(), error);
    CHECK(writeTree(*read) == text);
    stats = {};
    addDrops(stats, true, 1, 300.0f);
    CHECK(pass(read->find(read->root())->transitions[0].conditions, variablesOf(stats)));
}
