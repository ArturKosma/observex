#include "core/SkillRefresh.h"

#include <doctest/doctest.h>

#include <cmath>
#include <cstdlib>
#include <functional>
#include <string>
#include <utility>
#include <vector>

using namespace observex;

namespace {

RefreshSkill skillAt(const char* name, int x0, bool seen = true) {
    RefreshSkill skill;
    skill.name = name;
    skill.seen = seen;
    skill.at = {x0, 1300, x0 + 36, 1336};
    skill.x = x0 + 18; // Desktop and screen image alike here.
    skill.y = 1318;
    skill.side = 36;
    return skill;
}

// A screen for Refresh: the cursor moved by its steps, the skills on the hotbar, and what OCR reads round the cursor
// (`tooltip`, given the cursor and the time). A read asked is answered on the next step, off an image shown then.
struct World {
    int cursorX = 100;
    int cursorY = 100;
    std::vector<RefreshSkill> skills;
    std::function<std::string(int x, int y, double now)> tooltip;
};

struct Run {
    std::vector<std::pair<std::string, int>> learned;
    std::vector<std::string> logs;
    bool done = false;
    double took = 0.0;
};

Run runAll(World& world, double seconds = 30.0) {
    constexpr double kDt = 1.0 / 60.0;
    SkillRefresher refresher;
    refresher.start(world.cursorX, world.cursorY);
    Run run;
    double now = 0.0;
    bool pending = false;
    for (int i = 0; i < static_cast<int>(seconds / kDt) && refresher.running(); ++i) {
        now += kDt;
        SkillRefresher::Input in;
        in.now = now;
        in.dt = kDt;
        in.cursorX = world.cursorX;
        in.cursorY = world.cursorY;
        in.imageWidth = 2560;
        in.imageHeight = 1440;
        in.skills = world.skills;
        std::string text;
        if (pending) {
            in.answered = true;
            in.shown = now;
            text = world.tooltip(world.cursorX, world.cursorY, now);
            in.text = text;
            pending = false;
        }
        const SkillRefresher::Output out = refresher.step(in);
        world.cursorX += out.dx;
        world.cursorY += out.dy;
        pending = pending || out.read;
        if (out.learned) {
            run.learned.emplace_back(out.name, out.mp);
        }
        if (!out.log.empty()) {
            run.logs.push_back(out.log);
        }
        if (out.done) {
            run.done = true;
            run.took = now;
        }
    }
    return run;
}

bool near(int x, int y, const RefreshSkill& skill) { return std::abs(x - skill.x) <= 4 && std::abs(y - skill.y) <= 4; }

bool hasLog(const Run& run, const std::string& line) {
    for (const std::string& log : run.logs) {
        if (log == line) {
            return true;
        }
    }
    return false;
}

} // namespace

TEST_CASE("Refresh reads a skill's MP cost off its tooltip's words") {
    MpCostRead read = readMpCost("Wind Strike Lv. 1 Active Skill MP Consumption 10 Range 600 F2 F3", "Wind Strike");
    CHECK(read.name);
    CHECK(read.mp == 10);
    CHECK(readMpCost("Wind Strike MP: 11", "Wind Strike").mp == 11);
    CHECK(readMpCost("Lv. 1 MP Consumption : 22", "Wind Strike").mp == 22);
    CHECK(readMpCost("Ice Bolt Consumes 15 MP", "Ice Bolt").mp == 15);
    CHECK(readMpCost("Ice Bolt 15 MP", "Ice Bolt").mp == 15);
    // OCR's slips: a letter read wrong in the name, O for 0 in the number.
    read = readMpCost("Wind Strlke Lv. l MP Consumption 1O", "Wind Strike");
    CHECK(read.name);
    CHECK(read.mp == 10);
    // A name inside a word ("Sit/Stand"), no MP written: none.
    read = readMpCost("Sit/Stand F12", "Sit");
    CHECK(read.name);
    CHECK(read.mp == -1);
    // The Status bar's numbers are no cost, and another skill's tooltip is not this one's.
    CHECK(readMpCost("HP 238/256 MP 90/108", "Wind Strike").mp == -1);
    read = readMpCost("Ice Bolt MP Consumption 15", "Wind Strike");
    CHECK_FALSE(read.name);
    CHECK(read.mp == 15);
    CHECK(readMpCost("", "Wind Strike").mp == -1);
    // A word between "MP" and the number that is not about a cost: not its cost.
    CHECK(readMpCost("MP Range 600", "Wind Strike").mp == -1);
}

TEST_CASE("Refresh reads the area round a skill, mostly above it, kept on the screen") {
    const StatsArea area = tooltipArea({1000, 1300, 1036, 1336}, 2560, 1440);
    CHECK(area == StatsArea{1018 - 432, 1318 - 504, 1018 + 432, 1440});
    const StatsArea corner = tooltipArea({10, 10, 30, 30}, 2560, 1440); // Small: as a 24 px miniature.
    CHECK(corner == StatsArea{0, 0, 20 + 12 * 24, 20 + 5 * 24});
}

TEST_CASE("Refresh goes over every skill, learns the costs read twice alike, skips one not on the hotbar, goes back") {
    World world;
    world.skills = {skillAt("Wind Strike", 1000), skillAt("Heal", 1100, false), skillAt("Sit", 1040)};
    world.tooltip = [&](int x, int y, double) -> std::string {
        if (near(x, y, world.skills[0])) {
            return "Wind Strike Lv. 1 MP Consumption 10 Range 600 F2 F12";
        }
        if (near(x, y, world.skills[2])) {
            return "Sit/Stand F2 F12";
        }
        return "F2 F12";
    };
    const Run run = runAll(world);
    REQUIRE(run.done);
    REQUIRE(run.learned.size() == 1);
    CHECK(run.learned[0] == std::make_pair(std::string("Wind Strike"), 10));
    CHECK(hasLog(run, "Refresh Heal: not found on the hotbar, skipped"));
    CHECK(hasLog(run, "Refresh Sit: no MP cost on its tooltip, its cost kept"));
    CHECK(hasLog(run, "Refresh done: 2 of 3 skills read (not read: Heal)"));
    CHECK(world.cursorX == 100); // Back where it was.
    CHECK(world.cursorY == 100);
    CHECK(run.took < 5.0);
}

TEST_CASE("Refresh reads only once the tooltip had time to come up, and takes a cost only when two reads agree") {
    World world;
    world.skills = {skillAt("Wind Strike", 1000)};
    world.cursorX = world.skills[0].x; // On it from the start.
    world.cursorY = world.skills[0].y;
    double firstRead = -1.0;
    int reads = 0;
    world.tooltip = [&](int, int, double now) -> std::string {
        if (firstRead < 0.0) {
            firstRead = now;
        }
        ++reads; // 10, then 12 twice: 12.
        return reads == 1 ? "Wind Strike MP Consumption 10" : "Wind Strike MP Consumption 12";
    };
    const Run run = runAll(world);
    CHECK(firstRead >= SkillRefresher::kTooltipDelay);
    REQUIRE(run.learned.size() == 1);
    CHECK(run.learned[0].second == 12);
    CHECK(reads == 3);
}

TEST_CASE("Refresh gives a skill up when its tooltip never says its name, saying what was read") {
    World world;
    world.skills = {skillAt("Wind Strike", 1000)};
    world.tooltip = [](int, int, double) -> std::string { return "F2 F12"; };
    const Run run = runAll(world);
    CHECK(run.done);
    CHECK(run.learned.empty());
    CHECK(hasLog(run, "Refresh Wind Strike: its name not on its tooltip in 3 s (read: \"F2 F12\")"));
    CHECK(hasLog(run, "Refresh done: 0 of 1 skill read (not read: Wind Strike)"));
}

TEST_CASE("Refresh ignores a read off an image shown before the tooltip could be up") {
    SkillRefresher refresher;
    const std::vector<RefreshSkill> skills = {skillAt("Wind Strike", 1000)};
    refresher.start(skills[0].x, skills[0].y);
    SkillRefresher::Input in;
    in.dt = 1.0 / 60.0;
    in.cursorX = skills[0].x;
    in.cursorY = skills[0].y;
    in.imageWidth = 2560;
    in.imageHeight = 1440;
    in.skills = skills;
    bool asked = false;
    for (int i = 1; i <= 120; ++i) { // 2 s, every read answered off an image shown before the cursor came.
        in.now = i / 60.0;
        in.answered = asked;
        in.shown = 0.0;
        in.text = "Wind Strike MP Consumption 10";
        const SkillRefresher::Output out = refresher.step(in);
        CHECK_FALSE(out.learned);
        asked = out.read;
    }
}
