#include "core/TargetSelect.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

using namespace observex;

namespace {

constexpr double kDt = 1.0 / 144.0; // A UI frame.

// A monster with its middle at x, y (the screen image and the desktop alike here).
SelectableMonster monster(std::uint32_t id, int x, int y, int w = 100, int h = 120) {
    return {id, {x - w / 2, y - h / 2, x + w / 2, y + h / 2}, x, y, w, h};
}

// The plate over the monster asked about, as a reading of an image shown at `shown` says it.
SelectSeen reading(std::uint64_t image, double shown, std::uint32_t look, bool plate, bool selected,
                   const char* plateName = "", bool hasTarget = false, const char* targetName = "") {
    SelectSeen seen;
    seen.image = image;
    seen.shown = shown;
    seen.plate.look = look;
    seen.plate.seen = plate;
    seen.plate.selected = selected;
    std::copy_n(plateName, std::min(std::strlen(plateName), seen.plate.name.size() - 1), seen.plate.name.begin());
    seen.hasTarget = hasTarget;
    seen.targetName = targetName;
    return seen;
}

// Select Target and a cursor it moves, one UI frame a step.
struct Run {
    TargetSelector selector;
    std::vector<SelectableMonster> monsters;
    double now = 10.0;
    int x = 0;
    int y = 0;
    int clicks = 0;
    std::string log;

    TargetSelector::Output step(const SelectSeen& seen = {}) {
        TargetSelector::Input in;
        in.now = now;
        in.dt = kDt;
        in.cursorX = x;
        in.cursorY = y;
        in.monsters = monsters;
        in.seen = seen;
        const TargetSelector::Output out = selector.step(in);
        x += out.dx;
        y += out.dy;
        clicks += out.click ? 1 : 0;
        if (!out.log.empty()) {
            log += out.log + "\n";
        }
        now += kDt;
        return out;
    }
    // Steps for `seconds`, the same reading each; true if it was done on one.
    bool steps(double seconds, const SelectSeen& seen = {}) {
        for (double t = 0.0; t < seconds; t += kDt) {
            if (step(seen).done) {
                return true;
            }
        }
        return false;
    }
    // Steps until the cursor is on the chosen monster's middle; the seconds it took.
    double arrive() {
        const double from = now;
        for (int i = 0; i < 1000; ++i) {
            const TargetSelector::Output out = step();
            if (out.dx == 0 && out.dy == 0 && selector.chosen() != 0) {
                break;
            }
        }
        return now - from;
    }
};

} // namespace

TEST_CASE("Select Target goes for the biggest monster, quickly and smoothly, and asks for its plate") {
    Run run;
    run.monsters = {monster(1, 300, 300, 40, 40), monster(2, 1000, 600)};
    const TargetSelector::Output first = run.step();
    CHECK(run.selector.chosen() == 2);
    CHECK(first.look.id == 2);
    CHECK(first.look.over == run.monsters[1].box);
    CHECK(run.log.find("going for monster 2") != std::string::npos);
    // Never faster than the top speed, never speeding up faster than allowed: smooth.
    double speed = std::hypot(first.dx, first.dy) / kDt;
    int lastX = run.x;
    int lastY = run.y;
    double took = 0.0;
    double arriving = 0.0; // The speed on the last step that moved.
    for (int i = 0; i < 1000; ++i) {
        const TargetSelector::Output out = run.step();
        const double moved = std::hypot(run.x - lastX, run.y - lastY) / kDt;
        CHECK(moved <= TargetSelector::kTopSpeed + 1.0 / kDt);
        CHECK(moved - speed <= TargetSelector::kAcceleration * kDt + 1.5 / kDt);
        speed = moved;
        if (moved > 0.0) {
            arriving = moved;
        }
        lastX = run.x;
        lastY = run.y;
        if (out.dx == 0 && out.dy == 0) {
            took = (i + 1) * kDt;
            break;
        }
    }
    CHECK(std::abs(run.x - 1000) <= 1);
    CHECK(std::abs(run.y - 600) <= 1);
    CHECK(took > 0.0);
    CHECK(took < 0.6); // 1166 px: quickly,
    CHECK(arriving < 1200.0); // and braked by the middle, not stopped at speed.
    CHECK(run.clicks == 0); // No plate seen yet: no click.

    // It follows the monster as it walks.
    run.monsters[1] = monster(2, 1100, 650);
    run.arrive();
    CHECK(std::abs(run.x - 1100) <= 1);
    CHECK(std::abs(run.y - 650) <= 1);
}

TEST_CASE("Select Target clicks once the game shows it hovered, and is done once it shows it as the new target") {
    Run run;
    run.monsters = {monster(7, 800, 500)};
    run.arrive();
    const double onIt = run.now;
    // A reading of an image shown before the cursor came onto it: not hovered by this cursor yet.
    run.steps(0.1, reading(1, onIt - 1.0, 7, true, false));
    CHECK(run.clicks == 0);
    // Its plate up with grey orbs: hovered. One click, then waiting for its sign.
    CHECK_FALSE(run.steps(0.5, reading(2, onIt + 0.01, 7, true, false)));
    CHECK(run.clicks == 1);
    const double clicked = run.now - 0.5;
    // Coloured orbs, but on an image shown before the click (and the target window of the one before): not yet.
    CHECK_FALSE(run.steps(0.1, reading(3, clicked - 0.05, 7, true, true, "Gremlin", true, "Imp")));
    // Coloured orbs and the target window with its name, shown after the click: done.
    CHECK(run.step(reading(4, clicked + 0.05, 7, true, true, "Gremlin", true, "Gremlin")).done);
    CHECK(run.clicks == 1);
    CHECK(run.log.find("Gremlin targeted") != std::string::npos);
    CHECK(run.log.find("1 click)") != std::string::npos);
}

TEST_CASE("Select Target is done at once on a monster already the target, and never clicks a target") {
    Run run;
    run.monsters = {monster(3, 600, 400)};
    run.arrive();
    // Coloured orbs, no target window yet: it waits, and never clicks (a click on the target attacks).
    CHECK_FALSE(run.steps(TargetSelector::kSelectedWait * 0.5, reading(1, run.now, 3, true, true)));
    CHECK(run.clicks == 0);
    CHECK(run.step(reading(2, run.now, 3, true, true, "", true, "Imp Elder")).done);
    CHECK(run.clicks == 0);
    CHECK(run.log.find("Imp Elder was the target already") != std::string::npos);

    // Coloured orbs and never a target window: taken as the target after a moment, never clicked, said in the log.
    Run other;
    other.monsters = {monster(3, 600, 400)};
    other.arrive();
    CHECK(other.steps(TargetSelector::kSelectedWait + 0.1, reading(1, other.now, 3, true, true)));
    CHECK(other.clicks == 0);
    CHECK(other.log.find("its plate shows monster 3 as the target, but no target window was seen") !=
          std::string::npos);
}

TEST_CASE("Select Target never takes a dead target: already the target, it goes for another at once") {
    // As the log of 2026-11-15 had it twice: chosen as it died, "the target already", and the brain stood there.
    Run run;
    run.monsters = {monster(3, 600, 400), monster(4, 900, 600, 60, 60)};
    run.arrive();
    REQUIRE(run.selector.chosen() == 3);
    SelectSeen dead = reading(1, run.now, 3, true, true, "Imp", true, "Imp");
    dead.targetHp = 0.0f;
    const TargetSelector::Output out = run.step(dead);
    CHECK_FALSE(out.done);
    CHECK(run.clicks == 0);
    CHECK(run.log.find("gave up on monster 3, it is the target already, but dead") != std::string::npos);
    run.step();
    CHECK(run.selector.chosen() == 4);

    // Its target HP not read: taken as before.
    Run unread;
    unread.monsters = {monster(3, 600, 400)};
    unread.arrive();
    CHECK(unread.step(reading(1, unread.now, 3, true, true, "Imp", true, "Imp")).done);

    // Just clicked: the target window may still show the dead one before a moment; then the new one alive: done.
    Run clicked;
    clicked.monsters = {monster(5, 700, 450)};
    clicked.arrive();
    clicked.step(reading(1, clicked.now, 5, true, false));
    REQUIRE(clicked.clicks == 1);
    const double at = clicked.now - kDt;
    SelectSeen before = reading(2, at + 0.05, 5, true, true, "Goblin", true, "Goblin");
    before.targetHp = 0.0f;
    CHECK_FALSE(clicked.steps(TargetSelector::kNamesWait * 0.5, before));
    SelectSeen alive = before;
    alive.image = 3;
    alive.targetHp = 1.0f;
    CHECK(clicked.step(alive).done);
    CHECK(clicked.log.find("Goblin targeted") != std::string::npos);

    // Shown dead on and on after the click: given up on.
    Run corpse;
    corpse.monsters = {monster(5, 700, 450)};
    corpse.arrive();
    corpse.step(reading(1, corpse.now, 5, true, false));
    SelectSeen stillDead = reading(2, corpse.now, 5, true, true, "Goblin", true, "Goblin");
    stillDead.targetHp = 0.0f;
    CHECK_FALSE(corpse.steps(TargetSelector::kNamesWait + 0.1, stillDead));
    CHECK(corpse.log.find("gave up on monster 5, the target window shows it dead") != std::string::npos);
}

TEST_CASE("Select Target clicks again with no sign, gives up after its clicks, and goes for another") {
    Run run;
    run.monsters = {monster(1, 900, 500), monster(2, 300, 300, 60, 60)};
    run.arrive();
    CHECK(run.selector.chosen() == 1);
    const double onIt = run.now;
    // Hovered, but never shown as the target.
    run.steps(TargetSelector::kClickWait * 0.5, reading(1, onIt + 0.01, 1, true, false));
    CHECK(run.clicks == 1);
    run.steps(TargetSelector::kClickWait * (TargetSelector::kClicks + 0.5), reading(2, onIt + 0.01, 1, true, false));
    CHECK(run.clicks == TargetSelector::kClicks);
    CHECK(run.log.find("clicked monster 1 again (click 2)") != std::string::npos);
    CHECK(run.log.find("gave up on monster 1") != std::string::npos);
    CHECK(run.selector.chosen() == 2);
    // Given up on: not chosen again a moment after, though it is the biggest and the only one left,
    run.monsters.erase(run.monsters.begin() + 1);
    run.steps(TargetSelector::kLoseSeconds + 0.1);
    CHECK(run.selector.chosen() == 0);
    CHECK(run.log.find("no monster to select") != std::string::npos);
    // but kRetryAfter after giving up on it,
    Run later = run;
    later.steps(TargetSelector::kRetryAfter);
    CHECK(later.selector.chosen() == 1);
    // and at once after a new start.
    run.selector.start();
    run.step();
    CHECK(run.selector.chosen() == 1);
}

TEST_CASE("Select Target waits when the plate's name and the target window's disagree, then takes it") {
    Run run;
    run.monsters = {monster(5, 700, 450)};
    run.arrive();
    run.step(reading(1, run.now, 5, true, false));
    REQUIRE(run.clicks == 1);
    const double clicked = run.now - kDt;
    const SelectSeen differ = reading(2, clicked + 0.05, 5, true, true, "Gremlin", true, "Goblin");
    CHECK_FALSE(run.steps(TargetSelector::kNamesWait * 0.9, differ));
    CHECK(run.steps(TargetSelector::kNamesWait * 0.2, differ));
    CHECK(run.log.find("its plate reads 'Gremlin' and the target window 'Goblin'") != std::string::npos);
    CHECK(run.clicks == 1);

    // One letter read wrong is the same name.
    Run other;
    other.monsters = {monster(5, 700, 450)};
    other.arrive();
    other.step(reading(1, other.now, 5, true, false));
    CHECK(other.step(reading(2, other.now, 5, true, true, "Grernlin", true, "Gremlin")).done);
}

TEST_CASE("Select Target keeps a monster lost a moment, and chooses another once it is lost longer") {
    Run run;
    run.monsters = {monster(1, 800, 500), monster(2, 200, 200, 50, 50)};
    run.arrive();
    CHECK(run.selector.chosen() == 1);
    const SelectableMonster kept = run.monsters[0];
    run.monsters.erase(run.monsters.begin());
    run.steps(TargetSelector::kLoseSeconds * 0.5);
    CHECK(run.selector.chosen() == 1); // Behind another a moment: still the one,
    run.monsters.insert(run.monsters.begin(), kept);
    run.step();
    CHECK(run.selector.chosen() == 1);
    run.monsters.erase(run.monsters.begin());
    run.steps(TargetSelector::kLoseSeconds * 1.5);
    CHECK(run.selector.chosen() == 2); // gone longer: another.
    CHECK(run.log.find("lost sight of monster 1") != std::string::npos);
}

TEST_CASE("Select Target gives up on a monster the game never shows hovered, and tries it again later") {
    // The user's case, 2026-11-15: its plate behind a tree, the cursor sat on it until the brain was stopped.
    Run run;
    run.monsters = {monster(4, 500, 500), monster(6, 1200, 300, 60, 60)};
    run.arrive();
    REQUIRE(run.selector.chosen() == 4);
    const double onIt = run.now;
    // No plate on the images shown since the cursor came onto it, under kHoverWait: still on it.
    run.steps(TargetSelector::kHoverWait * 0.8, reading(1, onIt + 0.01, 4, false, false));
    CHECK(run.selector.chosen() == 4);
    // None on an image shown kHoverWait after: given up on, the other chosen; never clicked.
    run.steps(0.05, reading(2, onIt + TargetSelector::kHoverWait + 0.01, 4, false, false));
    CHECK(run.selector.chosen() == 6);
    CHECK(run.clicks == 0);
    CHECK(run.log.find("gave up on monster 4, the cursor was on it") != std::string::npos);
    CHECK(run.log.find("never showed it hovered (no name plate over it, 0 orbs round it)") != std::string::npos);
    CHECK(run.log.find("going for monster 6") != std::string::npos);

    // The other gone, and the tracker finding the first again as a new id where it was: not chosen a moment after
    // giving up on it,
    run.monsters = {monster(14, 505, 500)};
    run.steps(TargetSelector::kLoseSeconds + 0.1);
    CHECK(run.selector.chosen() == 0);
    CHECK(run.log.find("no monster to select on screen but 1 given up on") != std::string::npos);
    // but kRetryAfter later (it may have come out from behind the tree).
    run.steps(TargetSelector::kRetryAfter);
    CHECK(run.selector.chosen() == 14);
}

TEST_CASE("Select Target gives up when the game stops showing it hovered after a click") {
    Run run;
    run.monsters = {monster(1, 800, 500), monster(2, 300, 300, 50, 50)};
    run.arrive();
    const double onIt = run.now;
    run.step(reading(1, onIt + 0.01, 1, true, false));
    REQUIRE(run.clicks == 1);
    // Its plate gone (a tree), no new target in the window: neither clicked again nor done, and not for ever.
    run.steps(TargetSelector::kHoverWait * 0.5, reading(2, onIt + 0.2, 1, false, false));
    CHECK(run.selector.chosen() == 1);
    run.steps(TargetSelector::kHoverWait, reading(3, onIt + TargetSelector::kHoverWait + 0.1, 1, false, false));
    CHECK(run.selector.chosen() == 2);
    CHECK(run.clicks == 1);
    CHECK(run.log.find("gave up on monster 1, the game stopped showing it hovered") != std::string::npos);
}

TEST_CASE("Select Target is done by the target window when its plate is not seen after the click") {
    // Hovered, clicked; then the plate hidden and the target window says what changed.
    struct Case {
        const char* before;
        float beforeHp;
        bool beforeTarget;
        const char* after;
        float afterHp;
        bool done;
    };
    const Case cases[] = {
        {"", -1.0f, false, "Goblin", 1.0f, true},       // No target before.
        {"Imp", 0.4f, true, "Goblin", 1.0f, true},      // Another name.
        {"Goblin", 0.0f, true, "Goblin", 1.0f, true},   // The one before dead, another of its name now.
        {"Goblin", 0.6f, true, "Goblin", 0.6f, false},  // The same one, as far as the window tells.
        {"Imp", 0.4f, true, "", 1.0f, false},           // The new name not read yet.
    };
    for (const Case& c : cases) {
        CAPTURE(c.before);
        CAPTURE(c.after);
        Run run;
        run.monsters = {monster(5, 700, 450)};
        run.arrive();
        SelectSeen hovered = reading(1, run.now, 5, true, false, "", c.beforeTarget, c.before);
        hovered.targetHp = c.beforeHp;
        run.step(hovered);
        REQUIRE(run.clicks == 1);
        const double clicked = run.now - kDt;
        // A reading of an image shown before the click is no sign.
        SelectSeen early = reading(2, clicked - 0.01, 5, false, false, "", true, c.after);
        early.targetHp = c.afterHp;
        CHECK_FALSE(run.step(early).done);
        SelectSeen after = reading(3, clicked + 0.05, 5, false, false, "", true, c.after);
        after.targetHp = c.afterHp;
        CHECK(run.step(after).done == c.done);
        if (c.done) {
            CHECK(run.log.find("seen by the target window, its name plate not seen") != std::string::npos);
        }
    }
}

TEST_CASE("Select Target gives up on any monster not the target a while after choosing it") {
    // Nothing read from the screen at all: the cursor sits on it, nothing says hovered or not.
    Run run;
    run.monsters = {monster(1, 800, 500), monster(2, 300, 300, 50, 50)};
    CHECK_FALSE(run.steps(TargetSelector::kMonsterSeconds - 0.1));
    CHECK(run.selector.chosen() == 1);
    run.steps(0.2);
    CHECK(run.selector.chosen() == 2);
    CHECK(run.log.find("gave up on monster 1, not the target 5 s after choosing it (0 clicks, nothing read from the "
                       "screen)") != std::string::npos);
}

TEST_CASE("Select Target clicks a monster the tracker misses on some frames") {
    // Missing every other frame (held by the tracker): the cursor still counts as on it since it came, so a reading of
    // an image shown a moment ago still says hovered.
    Run run;
    const SelectableMonster m = monster(9, 600, 400);
    run.monsters = {m};
    run.arrive();
    run.steps(0.2);
    for (int i = 0; i < 40 && run.clicks == 0; ++i) {
        run.monsters.clear();
        if (i % 2 == 1) {
            run.monsters.push_back(m);
        }
        run.step(reading(1, run.now - 0.02, 9, true, false));
    }
    CHECK(run.clicks == 1);
}
