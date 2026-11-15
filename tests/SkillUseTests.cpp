#include "core/SkillUse.h"

#include <doctest/doctest.h>

#include <string>
#include <vector>

using namespace observex;
using observex::statetree::Variables;

namespace {

// A reading: these skills seen (each found; its cooldown left in percent, 0: ready; pressed: its pressed look), the cast
// bar showing `casting` ("": none; "?": a cast not named), and MP in points (below 0: its number not read).
struct Seen {
    std::vector<std::pair<std::string, float>> skills;
    std::string casting;
    float mp = -1.0f;
    std::string pressed;
};

Variables reading(const Seen& seen) {
    Variables variables;
    for (const auto& [name, cooldown] : seen.skills) {
        variables.skills.push_back({name, true, cooldown, name == seen.pressed});
    }
    variables.castShown = !seen.casting.empty();
    variables.casting = seen.casting == "?" ? std::string() : seen.casting;
    variables.castingKnown = seen.casting != "?";
    if (seen.mp >= 0.0f) {
        variables.mpPointsKnown = true;
        variables.mpPoints = seen.mp;
        variables.mpMax = 200.0f;
    }
    return variables;
}

// Use Skill stepped as the app does: on `now`, with the reading of image `image` shown at `shown`.
struct Run {
    SkillUser user;
    std::vector<UsableSkill> skills;
    Variables variables;
    std::vector<std::string> logs;

    SkillUser::Output step(double now, std::uint64_t image, double shown, const Seen& seen, bool canPress = true) {
        variables = reading(seen);
        SkillUser::Input in;
        in.now = now;
        in.canPress = canPress;
        in.image = image;
        in.shown = shown;
        in.variables = &variables;
        in.skills = skills;
        SkillUser::Output out = user.step(in);
        if (!out.log.empty()) {
            logs.push_back(out.log);
        }
        return out;
    }
};

UsableSkill skill(const char* name, const char* key, int mp = 0, bool cooldown = true) {
    return {name, key, true, cooldown, mp};
}

} // namespace

TEST_CASE("Use Skill presses the first ready skill, top to bottom, and waits while none is") {
    Run run;
    run.skills = {skill("Wind Strike", "F2", 10), skill("Ice Bolt", "F3", 12)};
    run.user.start();
    // Neither ready: it waits, saying so once.
    CHECK(run.step(0.0, 1, 0.0, {{{"Wind Strike", 50.0f}, {"Ice Bolt", 20.0f}}}).press == -1);
    CHECK(run.step(0.1, 2, 0.1, {{{"Wind Strike", 40.0f}, {"Ice Bolt", 10.0f}}}).press == -1);
    REQUIRE(run.logs.size() == 1);
    CHECK(run.logs[0] == "Use Skill: Wind Strike, Ice Bolt not ready, the task waits");
    // Ice Bolt ready first: pressed, Wind Strike named as not ready.
    const SkillUser::Output out = run.step(0.2, 3, 0.2, {{{"Wind Strike", 30.0f}, {"Ice Bolt", 0.0f}}});
    CHECK(out.press == 1);
    CHECK_FALSE(out.done);
    CHECK(run.user.chosen() == "Ice Bolt");
    CHECK(run.logs.back() == "Use Skill Ice Bolt: pressed F3 (not ready: Wind Strike)");

    // Both ready: the top one. Not while another press is held.
    Run both;
    both.skills = run.skills;
    both.user.start();
    CHECK(both.step(0.0, 1, 0.0, {{{"Wind Strike", 0.0f}, {"Ice Bolt", 0.0f}}}, false).press == -1);
    CHECK(both.step(0.01, 1, 0.0, {{{"Wind Strike", 0.0f}, {"Ice Bolt", 0.0f}}}).press == 0);

    // A ready one whose shortcut can't be pressed is passed over.
    Run keyless;
    keyless.skills = {{"Wind Strike", "F?", false, true, 10}, skill("Ice Bolt", "F3")};
    keyless.user.start();
    CHECK(keyless.step(0.0, 1, 0.0, {{{"Wind Strike", 0.0f}, {"Ice Bolt", 0.0f}}}).press == 1);
}

TEST_CASE("Use Skill is done once its cast ended") {
    Run run;
    run.skills = {skill("Wind Strike", "F2")}; // No MP cost set: the cast alone says.
    run.user.start();
    CHECK(run.step(0.0, 1, 0.0, {{{"Wind Strike", 0.0f}}}).press == 0);
    CHECK_FALSE(run.step(0.10, 2, 0.08, {{{"Wind Strike", 0.0f}}, "", -1.0f, "Wind Strike"}).done); // Its pressed look.
    CHECK_FALSE(run.step(0.60, 3, 0.55, {{{"Wind Strike", 98.0f}}, "Wind Strike"}).done);          // Casting.
    CHECK_FALSE(run.step(1.60, 4, 1.55, {{{"Wind Strike", 80.0f}}, "Wind Strike"}).done);
    const SkillUser::Output out = run.step(2.10, 5, 2.05, {{{"Wind Strike", 70.0f}}, ""}); // The bar gone.
    CHECK(out.done);
    CHECK(run.logs.back().starts_with("Use Skill Wind Strike: done, its cast ended"));
    CHECK(run.step(2.20, 6, 2.15, {{{"Wind Strike", 60.0f}}}).done); // Stays done.

    // A cast bar not named counts as its own.
    Run unnamed;
    unnamed.skills = {skill("Wind Strike", "F2")};
    unnamed.user.start();
    unnamed.step(0.0, 1, 0.0, {{{"Wind Strike", 0.0f}}});
    CHECK_FALSE(unnamed.step(0.5, 2, 0.5, {{{"Wind Strike", 0.0f}}, "?"}).done);
    CHECK(unnamed.step(1.5, 3, 1.5, {{{"Wind Strike", 0.0f}}, ""}).done);
}

TEST_CASE("Use Skill is done once its MP cost was taken, regeneration before it aside") {
    Run run;
    run.skills = {skill("Wind Strike", "F2", 10)};
    run.user.start();
    CHECK(run.step(0.0, 1, 0.0, {{{"Wind Strike", 0.0f}}, "", 100.0f}).press == 0);
    CHECK_FALSE(run.step(0.25, 2, 0.25, {{{"Wind Strike", 0.0f}}, "", 101.0f, "Wind Strike"}).done);
    CHECK_FALSE(run.step(0.7, 3, 0.7, {{{"Wind Strike", 99.0f}}, "Wind Strike", 102.0f}).done); // Regenerating.
    CHECK_FALSE(run.step(1.0, 4, 1.0, {{{"Wind Strike", 95.0f}}, "Wind Strike", 96.0f}).done);  // 6 off the most: no.
    const SkillUser::Output out = run.step(1.3, 5, 1.3, {{{"Wind Strike", 90.0f}}, "Wind Strike", 93.0f});
    CHECK(out.done); // 9 off 102: 3/4 of its cost and more, though its cast goes on.
    CHECK(run.logs.back() == "Use Skill Wind Strike: done, its MP taken (102 -> 93, it costs 10)");
}

TEST_CASE("Use Skill presses again while the game shows no sign, and gives up a while after its first press") {
    Run run;
    run.skills = {skill("Wind Strike", "F2", 10)};
    run.user.start();
    const Seen nothing{{{"Wind Strike", 0.0f}}, "", 100.0f};
    CHECK(run.step(0.0, 1, 0.0, nothing).press == 0);
    CHECK(run.step(0.2, 2, 0.2, nothing).press == -1);
    // A reading shown kRepress after the press and still nothing: again, said once.
    CHECK(run.step(0.35, 3, 0.32, nothing).press == 0);
    CHECK(run.logs.back() ==
          "Use Skill Wind Strike: pressed F2 again, no sign the game took it; pressing it every 0.3 s until it does");
    CHECK(run.step(0.5, 4, 0.5, nothing).press == -1); // Its time counts from the new press.
    CHECK(run.step(0.7, 5, 0.68, nothing).press == 0); // Press 3, not said.
    CHECK(run.logs.size() == 2);
    // On and on, on every reading kRepress after the last press, until kGiveUpSeconds after the first.
    std::uint64_t image = 6;
    int presses = 3;
    SkillUser::Output out;
    for (double now = 0.8; now < 6.0 && !out.done; now += 0.1) {
        out = run.step(now, image++, now - 0.02, nothing);
        presses += out.press == 0 ? 1 : 0;
    }
    CHECK(out.done);
    CHECK(out.press == -1);
    CHECK(presses == 13); // One every 0.4 s with readings 0.1 s apart.
    CHECK(run.logs.size() == 3);
    CHECK(run.logs.back() == "Use Skill Wind Strike: gave up, pressed 13 times in 5.08 s and the game never showed it taken");

    // No reading since the press: no judging, so no blind presses.
    Run stalled;
    stalled.skills = {skill("Wind Strike", "F2", 10)};
    stalled.user.start();
    stalled.step(0.0, 1, 0.0, nothing);
    CHECK(stalled.step(3.0, 1, 0.0, nothing).press == -1);
    CHECK_FALSE(stalled.step(3.0, 1, 0.0, nothing).done);
    CHECK(stalled.step(20.5, 1, 0.0, nothing).done); // At most kMostSeconds.
}

TEST_CASE("Use Skill: an instant skill, a skill seen only pressed, and one with nothing to watch") {
    // Cooling with no cast bar: done shortly after, when no MP is watched.
    Run instant;
    instant.skills = {skill("Stun", "F5")};
    instant.user.start();
    instant.step(0.0, 1, 0.0, {{{"Stun", 0.0f}}});
    CHECK_FALSE(instant.step(0.3, 2, 0.3, {{{"Stun", 99.0f}}}).done);
    CHECK_FALSE(instant.step(0.5, 3, 0.5, {{{"Stun", 98.0f}}}).done);
    CHECK(instant.step(0.8, 4, 0.75, {{{"Stun", 97.0f}}}).done);
    CHECK(instant.logs.back() == "Use Skill Stun: done, its cooldown began with no cast bar (instant)");

    // With an MP cost and MP read: waits for it, at most kMpWait.
    Run costly;
    costly.skills = {skill("Stun", "F5", 8)};
    costly.user.start();
    costly.step(0.0, 1, 0.0, {{{"Stun", 0.0f}}, "", 50.0f});
    CHECK_FALSE(costly.step(0.3, 2, 0.3, {{{"Stun", 99.0f}}, "", 50.0f}).done);
    CHECK_FALSE(costly.step(1.0, 3, 1.0, {{{"Stun", 95.0f}}, "", 50.0f}).done);
    CHECK(costly.step(2.4, 4, 2.35, {{{"Stun", 90.0f}}, "", 50.0f}).done);
    CHECK(costly.logs.back() == "Use Skill Stun: done, its cooldown began but its MP was not seen taken");

    // Only its pressed look: no sign (the game flashes the slot also when it refuses the skill), so pressed again.
    Run looked;
    looked.skills = {skill("Wind Strike", "F2")};
    looked.user.start();
    looked.step(0.0, 1, 0.0, {{{"Wind Strike", 0.0f}}});
    CHECK(looked.step(0.2, 2, 0.2, {{{"Wind Strike", 0.0f}}, "", -1.0f, "Wind Strike"}).press == -1);
    const SkillUser::Output again = looked.step(0.4, 3, 0.35, {{{"Wind Strike", 0.0f}}, "", -1.0f, "Wind Strike"});
    CHECK(again.press == 0);
    CHECK_FALSE(again.done);

    // No cooldown and no MP cost (Sit): done once pressed, never pressed twice.
    Run sit;
    sit.skills = {skill("Sit", "F12", 0, false)};
    sit.user.start();
    const SkillUser::Output out = sit.step(0.0, 1, 0.0, {{{"Sit", 0.0f}}});
    CHECK(out.press == 0);
    CHECK(out.done);
    CHECK(sit.logs.back() == "Use Skill Sit: pressed F12, done (no cooldown or MP cost to watch)");
    CHECK(sit.step(1.5, 2, 1.5, {{{"Sit", 0.0f}}}).press == -1);
}

TEST_CASE("Use Skill keeps pressing a skill that reads ready while the game refuses it, then watches its cast") {
    // As the log of 2026-11-15 had it: Wind Strike ready on screen, the game ignoring F2 for 2 s, then casting it.
    Run run;
    run.skills = {skill("Wind Strike", "F2", 10)};
    run.user.start();
    const Seen refused{{{"Wind Strike", 0.0f}}, "", 171.0f};
    const Seen casting{{{"Wind Strike", 95.0f}}, "Wind Strike", 171.0f};
    const Seen cast{{{"Wind Strike", 40.0f}}, "", 162.0f};
    CHECK(run.step(0.0, 1, 0.0, refused).press == 0);
    int presses = 1;
    SkillUser::Output out;
    for (int i = 1; i <= 70 && !out.done; ++i) {
        const double now = i * 0.1;
        out = run.step(now, static_cast<std::uint64_t>(i + 1), now - 0.02, i <= 20 ? refused : i <= 60 ? casting : cast);
        presses += out.press == 0 ? 1 : 0;
        if (i > 20) {
            CHECK(out.press == -1); // Taken: never pressed while it casts.
        }
    }
    CHECK(out.done);
    CHECK(presses == 6); // At 0, 0.4, 0.8, 1.2, 1.6 and 2.0 s.
    REQUIRE(run.logs.size() == 3);
    CHECK(run.logs[1] ==
          "Use Skill Wind Strike: pressed F2 again, no sign the game took it; pressing it every 0.3 s until it does");
    CHECK(run.logs[2] == "Use Skill Wind Strike: done, its MP taken (171 -> 162, it costs 10, after 6 presses over 2.00 s)");
}
