#include "core/TargetSelect.h"

#include "core/MouseCamera.h"

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

// One `distance` away (own heights), the monster memory's `key`.
SelectableMonster measured(std::uint32_t id, int x, int y, float distance, std::uint32_t key = 0) {
    SelectableMonster m = monster(id, x, y);
    m.distance = distance;
    m.key = key;
    return m;
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
    std::vector<RememberedMonster> offScreen; // The memory's, off the screen (one forgotten is taken out).
    std::vector<StatsArea> ui;                // The game's interface.
    StatsArea screen{};
    bool canTurn = false;
    bool mouseFree = true;
    double now = 10.0;
    int x = 0;
    int y = 0;
    int clicks = 0;
    // The game's camera: turned by the counts while the right button is held, once it put the cursor in its middle
    // (a moment after the press); the cursor stays meanwhile.
    bool right = false;
    double pressedAt = 0.0;
    bool cameraMode = false;
    int turned = 0;
    int presses = 0;
    std::vector<std::uint32_t> gaveUpKeys;
    std::vector<std::uint32_t> forgotKeys;
    std::string log;

    TargetSelector::Output step(const SelectSeen& seen = {}) {
        TargetSelector::Input in;
        in.now = now;
        in.dt = kDt;
        in.cursorX = x;
        in.cursorY = y;
        in.monsters = monsters;
        in.seen = seen;
        in.offScreen = offScreen;
        in.canTurn = canTurn;
        in.cursorAtMiddle = cameraMode;
        in.mouseFree = mouseFree;
        in.ui = ui;
        in.screen = screen;
        const TargetSelector::Output out = selector.step(in);
        if (out.right && !right) {
            right = true;
            pressedAt = now;
            ++presses;
        } else if (!out.right && right) {
            right = false;
            cameraMode = false;
        }
        if (right) {
            turned += cameraMode ? out.dx : 0;
        } else {
            x += out.dx;
            y += out.dy;
        }
        if (right && now - pressedAt >= 0.03) {
            cameraMode = true;
        }
        if (out.gaveUpKey != 0) {
            gaveUpKeys.push_back(out.gaveUpKey);
        }
        if (out.forgetKey != 0) {
            forgotKeys.push_back(out.forgetKey);
            std::erase_if(offScreen, [&](const RememberedMonster& r) { return r.key == out.forgetKey; });
        }
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
    // No plate on the images shown since the cursor came onto it, under kHoverWait: still on it, not clicked.
    run.steps(TargetSelector::kHoverWait * 0.8, reading(1, onIt + 0.01, 4, false, false));
    CHECK(run.selector.chosen() == 4);
    CHECK(run.clicks == 0);
    // None on an image shown kHoverWait after: clicked once all the same (its plate may be hidden), the target window
    // to tell; no new target in it within kClickWait: given up on, the other chosen.
    run.steps(TargetSelector::kClickWait * 0.8, reading(2, onIt + TargetSelector::kHoverWait + 0.01, 4, false, false));
    CHECK(run.selector.chosen() == 4);
    CHECK(run.clicks == 1);
    CHECK(run.log.find("clicked monster 4 without seeing its name plate") != std::string::npos);
    run.steps(TargetSelector::kClickWait * 0.3, reading(3, onIt + TargetSelector::kHoverWait + 0.5, 4, false, false));
    CHECK(run.selector.chosen() == 6);
    CHECK(run.clicks == 1);
    CHECK(run.log.find("gave up on monster 4, the game never showed it hovered (no name plate over it, 0 orbs round "
                       "it), and the click made all the same brought no new target") != std::string::npos);
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

TEST_CASE("Select Target clicks once with its plate hidden, and is done when the target window shows the new target") {
    // The user's clips 2026-11-16_14-22-00 (tree leaves drawn over an Imp's plate) and 14-25-14 (a Goblin's plate under
    // the Status window): the game hovered them, the plate reader saw none, and they were never clicked.
    Run run;
    run.monsters = {monster(4, 500, 500)};
    run.arrive();
    const double onIt = run.now;
    run.steps(0.05, reading(1, onIt + TargetSelector::kHoverWait + 0.01, 4, false, false));
    REQUIRE(run.clicks == 1);
    CHECK(run.step(reading(2, run.now, 4, false, false, "", true, "Imp")).done);
    CHECK(run.clicks == 1);
    CHECK(run.log.find("done, Imp targeted") != std::string::npos);
    CHECK(run.log.find("seen by the target window, its name plate not seen") != std::string::npos);
}

TEST_CASE("Select Target never clicks a monster with its plate hidden while a living target is in the window") {
    // That target might be this one: a click on it attacks.
    Run run;
    run.monsters = {monster(4, 500, 500), monster(6, 1200, 300, 60, 60)};
    run.arrive();
    const double onIt = run.now;
    SelectSeen alive = reading(1, onIt + TargetSelector::kHoverWait + 0.01, 4, false, false, "", true, "Goblin");
    alive.targetHp = 0.7f;
    run.steps(0.05, alive);
    CHECK(run.clicks == 0);
    CHECK(run.selector.chosen() == 6);
    CHECK(run.log.find("not clicked all the same, a living target in the target window") != std::string::npos);
    // A dead one in the window: clicked.
    Run dead;
    dead.monsters = {monster(4, 500, 500)};
    dead.arrive();
    SelectSeen corpse = reading(1, dead.now + TargetSelector::kHoverWait + 0.01, 4, false, false, "", true, "Goblin");
    corpse.targetHp = 0.0f;
    dead.steps(0.05, corpse);
    CHECK(dead.clicks == 1);
}

TEST_CASE("Select Target moves the click point off the game's interface, and leaves out monsters wholly under it") {
    const std::vector<StatsArea> ui = {{2053, 4, 2555, 507}, {0, 0, 481, 105}}; // Minimap, Status (clip 14-25-14).
    // Wholly under the minimap (the clip's imp, 2278,405 55 x 68): left out.
    SelectableMonster imp = monster(1, 2305, 439, 55, 68);
    CHECK_FALSE(clearOfInterface(imp, ui));
    // Clear of it: its middle, unchanged.
    SelectableMonster goblin = monster(2, 94, 150, 30, 34);
    CHECK(clearOfInterface(goblin, ui));
    CHECK(goblin.x == 94);
    CHECK(goblin.y == 150);
    // Its middle under the minimap's bottom edge, its lower part clear: the click point moves down, within kInside.
    SelectableMonster half = monster(3, 2200, 500, 60, 100);
    REQUIRE(clearOfInterface(half, ui));
    CHECK(half.x == 2200);
    CHECK(half.y >= 507 + kUiMargin);
    CHECK(half.y - 500 <= TargetSelector::kInside * 100);
    CHECK_FALSE(onInterface(half.x, half.y, ui));
    // Select Target goes there, and is on it.
    Run run;
    run.monsters = {half};
    run.arrive();
    CHECK(std::abs(run.x - half.x) <= 1);
    CHECK(std::abs(run.y - half.y) <= 1);
    run.step(reading(1, run.now, 3, true, false));
    CHECK(run.clicks == 1);
}

TEST_CASE("Select Target says when the only monsters are under the game's interface") {
    TargetSelector selector;
    TargetSelector::Input in;
    in.now = 10.0;
    in.dt = kDt;
    in.underInterface = 1;
    const TargetSelector::Output first = selector.step(in);
    CHECK(first.log.find("no monster to select on screen (1 under the game's interface: no click reaches it)") !=
          std::string::npos);
    CHECK(selector.step(in).log.empty()); // Said once.
}

TEST_CASE("Select Target moves the cursor off the interface before pressing the right button to turn") {
    // The user's clip 2026-11-16_14-25-14: the cursor left on the minimap, the game never took the press three times.
    Run run;
    run.canTurn = true;
    run.screen = {0, 0, 2560, 1440};
    run.ui = {{2053, 4, 2555, 507}};
    run.x = 2307;
    run.y = 437;
    run.offScreen = {{22, 7, 4.0f, 2.0f}};
    run.step();
    CHECK_FALSE(run.right); // Not pressed on the minimap,
    CHECK(run.log.find("the cursor first off the game's interface") != std::string::npos);
    for (int i = 0; i < 1000 && !run.right; ++i) {
        run.step();
    }
    REQUIRE(run.right); // but once off it: at the point over the screen's middle.
    CHECK(std::abs(run.x - 1280) <= 2);
    CHECK(std::abs(run.y - (720 - static_cast<int>(TargetSelector::kClearAbove * 1440))) <= 2);
    CHECK_FALSE(onInterface(run.x, run.y, run.ui));
    CHECK(run.presses == 1);
    run.steps(1.2);
    CHECK(run.turned != 0); // The game took it and the camera turned.

    // Clear already: pressed at once where it is.
    Run clear;
    clear.canTurn = true;
    clear.screen = {0, 0, 2560, 1440};
    clear.ui = {{2053, 4, 2555, 507}};
    clear.x = 1500;
    clear.y = 900;
    clear.offScreen = {{22, 7, 4.0f, 2.0f}};
    clear.step();
    CHECK(clear.right);
    CHECK(clear.x == 1500);

    // The point over the middle under a window: the clear point nearest it.
    Run covered;
    covered.canTurn = true;
    covered.screen = {0, 0, 2560, 1440};
    covered.ui = {{2053, 4, 2555, 507}, {900, 300, 1700, 600}};
    covered.x = 2307;
    covered.y = 437;
    covered.offScreen = {{22, 7, 4.0f, 2.0f}};
    for (int i = 0; i < 1000 && !covered.right; ++i) {
        covered.step();
    }
    REQUIRE(covered.right);
    CHECK_FALSE(onInterface(covered.x, covered.y, covered.ui));
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

TEST_CASE("Select Target goes for the nearest monster by its distance, not the biggest") {
    Run run;
    SelectableMonster big = measured(1, 300, 300, 10.0f);
    big.w = 300;
    big.h = 360;
    SelectableMonster unknown = monster(3, 1500, 400, 400, 480); // No distance: after all that have one.
    run.monsters = {big, measured(2, 1000, 600, 4.0f), unknown};
    run.step();
    CHECK(run.selector.chosen() == 2);
    CHECK(run.log.find("going for monster 2 (190 away, the nearest of 3)") != std::string::npos);
    CHECK_FALSE(run.right);
}

TEST_CASE("Select Target tells the memory the key of one it gave up on") {
    Run run;
    run.monsters = {measured(4, 800, 500, 6.0f, 44)};
    run.steps(TargetSelector::kMonsterSeconds + 0.1);
    REQUIRE_FALSE(run.gaveUpKeys.empty());
    CHECK(run.gaveUpKeys.front() == 44);
}

TEST_CASE("Select Target is done without a target once there is nothing to select a moment") {
    {
        Run run; // None on the screen, none remembered: done kNoneWait later, not before.
        CHECK_FALSE(run.steps(TargetSelector::kNoneWait * 0.8));
        CHECK(run.steps(TargetSelector::kNoneWait * 0.4));
        CHECK(run.selector.chosen() == 0);
        CHECK(run.clicks == 0);
        CHECK(run.log.find("Select Target: done without a target, nothing to select for 0.5 s") != std::string::npos);
    }
    {
        Run run; // One showing up within kNoneWait: gone for, not done.
        CHECK_FALSE(run.steps(TargetSelector::kNoneWait * 0.8));
        run.monsters = {monster(1, 900, 500)};
        CHECK_FALSE(run.steps(TargetSelector::kNoneWait));
        CHECK(run.selector.chosen() == 1);
    }
    {
        Run run; // Remembered off the screen, but the turn not known: nothing to select either.
        run.offScreen = {{22, 7, 4.0f, 2.0f}};
        CHECK(run.steps(TargetSelector::kNoneWait + 0.1));
        CHECK_FALSE(run.right);
    }
    {
        Run run; // The only one on the screen given up on a moment ago: waits for it, tried again kRetryAfter later.
        run.monsters = {measured(4, 800, 500, 6.0f, 44)};
        run.steps(TargetSelector::kMonsterSeconds + 0.1);
        REQUIRE(run.log.find("gave up on monster 4") != std::string::npos);
        CHECK_FALSE(run.steps(TargetSelector::kRetryAfter * 0.9));
        CHECK(run.selector.chosen() == 0);
        CHECK_FALSE(run.steps(TargetSelector::kRetryAfter * 0.2));
        CHECK(run.selector.chosen() == 4);
    }
}

TEST_CASE("Select Target turns the camera to a nearer monster the memory keeps off the screen, then goes for it") {
    Run run;
    run.canTurn = true;
    run.x = 640;
    run.y = 360;
    run.monsters = {measured(1, 900, 500, 10.0f, 11)};
    run.offScreen = {{22, 7, 4.0f, 2.0f}}; // 115 degrees right, 4 own heights away.
    run.step();
    CHECK(run.right);
    CHECK(run.selector.chosen() == 0);
    CHECK(run.log.find("Select Target: turning right 95 degrees to a monster remembered 190 away, 115 degrees right "
                       "(the nearest on the screen is 470 away)") != std::string::npos);
    run.steps(1.2); // Turned and settled; not yet found.
    CHECK_FALSE(run.right);
    CHECK(run.presses == 1);
    // To 20 degrees short of it: it stands beside the own character, not behind it.
    CHECK(run.turned == static_cast<int>(std::lround((2.0 - TargetSelector::kFaceBearing) / MouseCamera::kCountRadians)));
    CHECK(run.x == 640); // The cursor stayed while the right button was held.
    CHECK(run.y == 360);
    CHECK(run.selector.chosen() == 0);
    // There now, the memory knows it by its key: it is the one.
    run.monsters = {measured(9, 1400, 700, 4.1f, 22)};
    run.step();
    CHECK(run.selector.chosen() == 9);
    CHECK(run.log.find("going for monster 9 (190 away, turned to it, remembered off the screen)") != std::string::npos);
    CHECK(run.forgotKeys.empty());
}

TEST_CASE("Select Target takes one nearer than the one turned to when it is on the screen after the turn") {
    // The user's clip 2026-11-16_13-29-15: remembered 860 away, 59 degrees left, the nearest on the screen 1290 away;
    // after the turn a goblin about 400 away was on the screen too.
    const auto turnTo = [](Run& run) {
        run.canTurn = true;
        run.monsters = {measured(1, 900, 300, 27.4f, 11)};
        run.offScreen = {{22, 7, 18.3f, -1.03f}};
        run.step();
        REQUIRE(run.right);
        run.monsters.clear();
        for (int i = 0; i < 1000 && run.right; ++i) { // Until the turn lets go of the right button,
            run.step();
        }
        run.steps(0.2); // then settled (CameraTurner::kSettle), well inside kFindWait.
        REQUIRE_FALSE(run.right);
        REQUIRE(run.forgotKeys.empty());
        REQUIRE(run.selector.chosen() == 0);
    };
    {
        Run run; // Both on the screen: the nearer.
        turnTo(run);
        run.monsters = {measured(9, 700, 300, 18.3f, 22), measured(5, 330, 650, 8.5f, 23)};
        run.step();
        CHECK(run.selector.chosen() == 5);
        CHECK(run.log.find("going for monster 5 (400 away, nearer than the one turned to, remembered 860 away)") !=
              std::string::npos);
        CHECK(run.forgotKeys.empty());
    }
    {
        Run run; // The one turned to not found yet: the nearer one on the screen at once.
        turnTo(run);
        run.monsters = {measured(5, 330, 650, 8.5f, 23)};
        run.step();
        CHECK(run.selector.chosen() == 5);
        CHECK(run.log.find("remembered 860 away and not found yet") != std::string::npos);
    }
    {
        Run run; // Others on the screen farther, or with no distance: the one turned to.
        turnTo(run);
        run.monsters = {measured(9, 700, 300, 18.3f, 22), measured(1, 1500, 200, 27.4f, 11), monster(3, 600, 900)};
        run.step();
        CHECK(run.selector.chosen() == 9);
        CHECK(run.log.find("turned to it, remembered off the screen") != std::string::npos);
    }
}

TEST_CASE("Select Target forgets one turned to and not there, and goes for one on the screen") {
    Run run;
    run.canTurn = true;
    run.monsters = {measured(1, 900, 500, 10.0f, 11)};
    run.offScreen = {{22, 7, 4.0f, -1.5f}}; // To the left.
    run.step();
    CHECK(run.right);
    run.steps(1.2 + TargetSelector::kFindWait);
    CHECK(run.turned < 0);
    REQUIRE(run.forgotKeys.size() == 1);
    CHECK(run.forgotKeys.front() == 22);
    CHECK(run.log.find("but it is not there") != std::string::npos);
    CHECK(run.selector.chosen() == 1);
}

TEST_CASE("Select Target turns to one off the screen only when it is worth it and the turn is known") {
    {
        Run run; // Nearer, but not by kTurnWorth.
        run.canTurn = true;
        run.monsters = {measured(1, 900, 500, 10.0f)};
        run.offScreen = {{22, 7, 9.0f, 2.0f}};
        run.step();
        CHECK_FALSE(run.right);
        CHECK(run.selector.chosen() == 1);
    }
    {
        Run run; // The camera's turn not known.
        run.monsters = {measured(1, 900, 500, 10.0f)};
        run.offScreen = {{22, 7, 2.0f, 2.0f}};
        run.step();
        CHECK_FALSE(run.right);
        CHECK(run.selector.chosen() == 1);
    }
    {
        Run run; // None on the screen: turned to.
        run.canTurn = true;
        run.offScreen = {{22, 7, 9.0f, 2.0f}};
        run.step();
        CHECK(run.right);
        CHECK(run.log.find("none to select on the screen") != std::string::npos);
    }
}

TEST_CASE("Select Target lets go of the right button when the mouse is wanted, and chooses again after") {
    Run run;
    run.canTurn = true;
    run.monsters = {measured(1, 900, 500, 10.0f)};
    run.offScreen = {{22, 7, 4.0f, 2.0f}};
    run.step();
    run.steps(0.3);
    REQUIRE(run.right);
    run.mouseFree = false;
    run.step();
    CHECK_FALSE(run.right);
    CHECK(run.log.find("the mouse is wanted") != std::string::npos);
    run.steps(0.5);
    CHECK(run.presses == 1);
    run.mouseFree = true;
    run.offScreen.clear(); // Not off the screen any more, say.
    run.steps(0.3);
    CHECK(run.selector.chosen() == 1);
}
