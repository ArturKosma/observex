#include "core/StateTree.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <cstring>
#include <optional>
#include <string>
#include <vector>

using namespace observex;
using namespace observex::statetree;

namespace {

// The rows as "name/depth" strings, to compare whole trees at once.
std::vector<std::string> shown(const Tree& tree) {
    std::vector<std::string> rows;
    for (const Row& row : tree.rows()) {
        rows.push_back(tree.find(row.state)->name + "/" + std::to_string(row.depth));
    }
    return rows;
}

Variables withHp(float hp) {
    Variables variables;
    variables.value[static_cast<int>(Variable::Hp)] = hp;
    variables.known[static_cast<int>(Variable::Hp)] = true;
    return variables;
}

Condition hpBelow(float value, Join join = Join::And) {
    return {join, {Variable::Hp, 0.0f}, Compare::Less, {Variable::None, value}};
}

void setGauge(GameStats& stats, const char* piece, const char* name, float full, bool seen = true,
              const char* text = "") {
    GameGauge& gauge = stats.gauges[static_cast<std::size_t>(stats.count++)];
    std::copy_n(piece, std::min(std::strlen(piece), gauge.piece.size() - 1), gauge.piece.begin());
    std::copy_n(name, std::min(std::strlen(name), gauge.name.size() - 1), gauge.name.begin());
    std::copy_n(text, std::min(std::strlen(text), gauge.text.size() - 1), gauge.text.begin());
    gauge.full = full;
    gauge.seen = seen;
}

// A target, as the stats reading keeps it: its window shows, with `name` last read on it ("": none read yet).
void setTarget(GameStats& stats, const char* name) {
    stats.hasTarget = true;
    std::copy_n(name, std::min(std::strlen(name), stats.targetName.size() - 1), stats.targetName.begin());
}

Condition ofKind(Kind kind, std::string text, Compare compare = Compare::Equal, SkillIs is = SkillIs::Ready) {
    Condition condition;
    condition.kind = kind;
    condition.text = std::move(text);
    condition.compare = compare;
    condition.is = is;
    return condition;
}

} // namespace

TEST_CASE("a new tree is just Root, which stays") {
    Tree tree;
    CHECK(shown(tree) == std::vector<std::string>{"Root/0"});
    CHECK_FALSE(tree.remove(tree.root()));
    CHECK(tree.find(0) == nullptr);
}

TEST_CASE("states go in as last children, listed depth first") {
    Tree tree;
    const int a = tree.add(tree.root(), "A");
    tree.add(tree.root(), "B");
    const int a1 = tree.add(a, "A1");
    CHECK(tree.find(a1)->parent == a);
    CHECK(shown(tree) == std::vector<std::string>{"Root/0", "A/1", "A1/2", "B/1"});
    CHECK(tree.add(99, "Nowhere") == 0);
    CHECK(tree.within(a1, tree.root()));
    CHECK(tree.within(a1, a1));
    CHECK_FALSE(tree.within(a, a1));
}

TEST_CASE("removing a state takes the states under it, and the transitions to them") {
    Tree tree;
    const int a = tree.add(tree.root(), "A");
    const int a1 = tree.add(a, "A1");
    const int a2 = tree.add(a1, "A2");
    const int b = tree.add(tree.root(), "B");
    tree.find(b)->transitions = {{a2, {}}, {b, {}}};
    CHECK(tree.remove(a));
    CHECK(tree.find(a) == nullptr);
    CHECK(tree.find(a1) == nullptr);
    CHECK(tree.find(a2) == nullptr);
    CHECK(shown(tree) == std::vector<std::string>{"Root/0", "B/1"});
    REQUIRE(tree.find(b)->transitions.size() == 1);
    CHECK(tree.find(b)->transitions[0].target == b);
    CHECK_FALSE(tree.remove(a)); // Gone already.
    CHECK(tree.add(tree.root(), "C") != a); // Ids are not reused.
}

TEST_CASE("a state moves with all under it, before another or last, among its siblings or under another parent") {
    Tree tree;
    const int a = tree.add(tree.root(), "A");
    const int a1 = tree.add(a, "A1");
    const int b = tree.add(tree.root(), "B");
    const int c = tree.add(tree.root(), "C");
    tree.find(c)->transitions = {{a1, {}}};

    CHECK(tree.move(c, tree.root(), a)); // Up, first.
    CHECK(shown(tree) == std::vector<std::string>{"Root/0", "C/1", "A/1", "A1/2", "B/1"});
    CHECK(tree.move(c, tree.root(), 0)); // Down, last.
    CHECK(shown(tree) == std::vector<std::string>{"Root/0", "A/1", "A1/2", "B/1", "C/1"});
    CHECK(tree.move(a, tree.root(), c)); // Down, with its child, before C.
    CHECK(shown(tree) == std::vector<std::string>{"Root/0", "B/1", "A/1", "A1/2", "C/1"});
    CHECK(tree.move(b, a, a1)); // Under another parent.
    CHECK(tree.find(b)->parent == a);
    CHECK(shown(tree) == std::vector<std::string>{"Root/0", "A/1", "B/2", "A1/2", "C/1"});
    CHECK(tree.move(a1, tree.root(), a)); // Out, up a depth.
    CHECK(shown(tree) == std::vector<std::string>{"Root/0", "A1/1", "A/1", "B/2", "C/1"});
    CHECK(tree.find(c)->transitions[0].target == a1); // Transitions keep their states.

    CHECK(tree.move(a, tree.root(), a)); // Just before itself: where it is.
    CHECK(shown(tree) == std::vector<std::string>{"Root/0", "A1/1", "A/1", "B/2", "C/1"});

    CHECK_FALSE(tree.move(tree.root(), a, 0)); // Root stays.
    CHECK_FALSE(tree.move(a, a, 0));           // Not under itself,
    CHECK_FALSE(tree.move(a, b, 0));           // nor under a state under it.
    CHECK_FALSE(tree.move(a, tree.root(), b)); // B is not Root's child.
    CHECK_FALSE(tree.move(99, tree.root(), 0));
    CHECK_FALSE(tree.move(a, 99, 0));
    CHECK(shown(tree) == std::vector<std::string>{"Root/0", "A1/1", "A/1", "B/2", "C/1"});

    // Written in its new order, it reads back the same.
    const std::optional<Tree> read = readTree(writeTree(tree), nullptr);
    REQUIRE(read);
    CHECK(shown(*read) == shown(tree));
}

TEST_CASE("a transition moves before another of its state's, last, or to another state, keeping what it is") {
    Tree tree;
    const int a = tree.add(tree.root(), "A");
    const int b = tree.add(tree.root(), "B");
    const int c = tree.add(tree.root(), "C");
    Transition toB{b, {hpBelow(30.0f)}, When::TasksDone};
    tree.find(a)->transitions = {{a, {}}, toB, {c, {}}};
    // The targets of `state`'s transitions, in order.
    const auto targets = [&](int state) {
        std::vector<int> ids;
        for (const Transition& transition : tree.find(state)->transitions) {
            ids.push_back(transition.target);
        }
        return ids;
    };

    CHECK(tree.moveTransition(a, 1, a, 0)); // Up, first.
    CHECK(targets(a) == std::vector<int>{b, a, c});
    CHECK(tree.moveTransition(a, 0, a, 3)); // Down, last.
    CHECK(targets(a) == std::vector<int>{a, c, b});
    CHECK(tree.moveTransition(a, 0, a, 2)); // Down one: before the one after the next.
    CHECK(targets(a) == std::vector<int>{c, a, b});
    CHECK(tree.moveTransition(a, 1, a, 1)); // Just before itself,
    CHECK(tree.moveTransition(a, 1, a, 2)); // or after it: where it is.
    CHECK(targets(a) == std::vector<int>{c, a, b});

    CHECK(tree.moveTransition(a, 2, c, 0)); // To another state, with its When and conditions.
    CHECK(targets(a) == std::vector<int>{c, a});
    REQUIRE(tree.find(c)->transitions.size() == 1);
    CHECK(tree.find(c)->transitions[0].target == b);
    CHECK(tree.find(c)->transitions[0].when == When::TasksDone);
    REQUIRE(tree.find(c)->transitions[0].conditions.size() == 1);
    CHECK(tree.find(c)->transitions[0].conditions[0].right.number == 30.0f);
    CHECK(tree.moveTransition(a, 0, tree.root(), 0)); // To Root too.
    CHECK(targets(tree.root()) == std::vector<int>{c});
    CHECK(tree.moveTransition(c, 0, a, 0)); // Back, first of another's.
    CHECK(targets(a) == std::vector<int>{b, a});
    CHECK(tree.find(c)->transitions.empty());

    CHECK_FALSE(tree.moveTransition(a, 2, a, 0));  // No such transition,
    CHECK_FALSE(tree.moveTransition(c, 0, a, 0));
    CHECK_FALSE(tree.moveTransition(a, 0, b, 1));  // nor such a place (B has none),
    CHECK_FALSE(tree.moveTransition(99, 0, a, 0)); // nor such states.
    CHECK_FALSE(tree.moveTransition(a, 0, 99, 0));
    CHECK(targets(a) == std::vector<int>{b, a});

    // Written in its new order, it reads back the same.
    const std::optional<Tree> read = readTree(writeTree(tree), nullptr);
    REQUIRE(read);
    CHECK(writeTree(*read) == writeTree(tree));
}

TEST_CASE("unused names count up from 2") {
    Tree tree;
    CHECK(tree.unusedName("New state") == "New state");
    tree.add(tree.root(), "New state");
    CHECK(tree.unusedName("New state") == "New state 2");
    tree.add(tree.root(), "New state 2");
    CHECK(tree.unusedName("New state") == "New state 3");
}

TEST_CASE("the variables are the bars read, in percent; a bar not seen is not known") {
    GameStats stats;
    setGauge(stats, "Status", "HP", 0.25f);
    setGauge(stats, "Status", "MP", -1.0f); // Under a window.
    setGauge(stats, "Status", "XP", 0.9026f);
    setGauge(stats, "Target", "HP", 1.0f, false);
    const Variables variables = variablesOf(stats);
    CHECK(variables.known[static_cast<int>(Variable::Hp)]);
    CHECK(variables.value[static_cast<int>(Variable::Hp)] == doctest::Approx(25.0f));
    CHECK_FALSE(variables.known[static_cast<int>(Variable::Mp)]);
    CHECK(variables.value[static_cast<int>(Variable::Xp)] == doctest::Approx(90.26f));
    CHECK_FALSE(variables.known[static_cast<int>(Variable::TargetHp)]);
}

TEST_CASE("conditions compare numbers and variables, joined left to right") {
    const Variables hp40 = withHp(40.0f);
    CHECK(holds(hpBelow(50.0f), hp40));
    CHECK_FALSE(holds(hpBelow(40.0f), hp40));
    CHECK(holds({Join::And, {Variable::Hp, 0}, Compare::LessOrEqual, {Variable::None, 40.0f}}, hp40));
    CHECK(holds({Join::And, {Variable::None, 7.0f}, Compare::Equal, {Variable::None, 7.0f}}, hp40));
    CHECK_FALSE(holds(hpBelow(50.0f), Variables{})); // HP not known.
    CHECK_FALSE(holds({Join::And, {Variable::Mp, 0}, Compare::NotEqual, {Variable::None, 1.0f}}, hp40)); // MP not known.
    CHECK(pass({}, hp40));
    CHECK(pass({hpBelow(10.0f), hpBelow(50.0f, Join::Or)}, hp40));            // false OR true
    CHECK_FALSE(pass({hpBelow(50.0f), hpBelow(10.0f, Join::And)}, hp40));     // true AND false
    CHECK_FALSE(pass({hpBelow(50.0f), hpBelow(10.0f), hpBelow(60.0f, Join::Or)}, Variables{}));
    CHECK(pass({hpBelow(50.0f), hpBelow(10.0f), hpBelow(60.0f, Join::Or)}, hp40)); // (true AND false) OR true
}

TEST_CASE("the target, its name, the cast and the added skills are read from their gauges") {
    GameStats stats;
    setGauge(stats, "Status", "HP", 0.5f);
    setGauge(stats, kCastingPiece, kCastingPiece, 0.41f, true, "Wind Strike");
    setGauge(stats, kSkillsPiece, "Wind Strike", 0.75f, true, "5");
    setGauge(stats, kSkillsPiece, "Heal", 0.0f);
    setGauge(stats, kSkillsPiece, "Shield", -1.0f, false); // Not found on screen.
    setGauge(stats, kTargetPiece, "HP", 0.09f);
    setTarget(stats, "Ashen Wolf");
    const Variables variables = variablesOf(stats);
    CHECK(variables.hasTarget);
    CHECK(variables.targetKnown);
    CHECK(variables.target == "Ashen Wolf");
    CHECK(variables.value[static_cast<int>(Variable::Cast)] == doctest::Approx(41.0f));
    CHECK(variables.castingKnown);
    CHECK(variables.casting == "Wind Strike");
    REQUIRE(variables.skills.size() == 3);
    CHECK(variables.skill("wind strike")->cooldown == doctest::Approx(75.0f));
    CHECK_FALSE(variables.skill("Shield")->found);
    CHECK(variables.skill("Nothing") == nullptr);

    // No target window, no cast bar: no target, nothing cast (and that is known).
    GameStats none;
    setGauge(none, kCastingPiece, kCastingPiece, -1.0f, false);
    setGauge(none, kTargetPiece, "HP", -1.0f, false);
    const Variables empty = variablesOf(none);
    CHECK_FALSE(empty.hasTarget);
    CHECK_FALSE(empty.targetKnown);
    CHECK(empty.castingKnown);
    CHECK(empty.casting.empty());
    CHECK(empty.known[static_cast<int>(Variable::Cast)]);
    CHECK(empty.value[static_cast<int>(Variable::Cast)] == 0.0f);

    // A target whose name is not read yet: there is one, its name not known.
    GameStats unnamed;
    setTarget(unnamed, "");
    CHECK(variablesOf(unnamed).hasTarget);
    CHECK_FALSE(variablesOf(unnamed).targetKnown);
}

TEST_CASE("target, name and skill conditions") {
    GameStats stats;
    setGauge(stats, kCastingPiece, kCastingPiece, 0.5f, true, "Wind Strike");
    setGauge(stats, kSkillsPiece, "Wind Strike", 0.75f);
    setGauge(stats, kSkillsPiece, "Heal", 0.0f);
    setGauge(stats, kSkillsPiece, "Shield", -1.0f, false);
    setGauge(stats, kTargetPiece, "HP", 0.09f);
    setTarget(stats, "Ashen Wolf");
    const Variables variables = variablesOf(stats);
    CHECK(holds(ofKind(Kind::HasTarget, ""), variables));
    CHECK_FALSE(holds(ofKind(Kind::HasTarget, "", Compare::NotEqual), variables));
    CHECK_FALSE(holds(ofKind(Kind::HasTarget, ""), Variables{}));
    CHECK(holds(ofKind(Kind::HasTarget, "", Compare::NotEqual), Variables{}));
    // A part of the name, any case, says True (the user's "Ashen" for "Ashen Wolf").
    CHECK(holds(ofKind(Kind::TargetName, "Ashen"), variables));
    CHECK(holds(ofKind(Kind::TargetName, "wolf"), variables));
    CHECK_FALSE(holds(ofKind(Kind::TargetName, "Bear"), variables));
    CHECK(holds(ofKind(Kind::TargetName, "Bear", Compare::NotEqual), variables));
    CHECK_FALSE(holds(ofKind(Kind::TargetName, "Ashen", Compare::NotEqual), variables));
    CHECK_FALSE(holds(ofKind(Kind::TargetName, "Ashen"), Variables{})); // No name read: both fail.
    CHECK_FALSE(holds(ofKind(Kind::TargetName, "Ashen", Compare::NotEqual), Variables{}));
    // Skills.
    CHECK(holds(ofKind(Kind::Skill, "Heal"), variables));
    CHECK_FALSE(holds(ofKind(Kind::Skill, "Wind Strike"), variables));
    CHECK(holds(ofKind(Kind::Skill, "wind strike", Compare::Equal, SkillIs::CoolingDown), variables));
    CHECK(holds(ofKind(Kind::Skill, "Wind Strike", Compare::Equal, SkillIs::Cast), variables));
    CHECK_FALSE(holds(ofKind(Kind::Skill, "Heal", Compare::Equal, SkillIs::Cast), variables));
    CHECK_FALSE(holds(ofKind(Kind::Skill, "Shield"), variables)); // Not found: neither ready
    CHECK_FALSE(holds(ofKind(Kind::Skill, "Shield", Compare::Equal, SkillIs::CoolingDown), variables)); // nor not.
    // A skill's cooldown and the cast as numbers.
    CHECK(holds({Join::And, {Variable::Cooldown, 0.0f, "Wind Strike"}, Compare::Greater, {Variable::None, 50.0f, {}}},
                variables));
    CHECK_FALSE(holds({Join::And, {Variable::Cooldown, 0.0f, "Shield"}, Compare::Less, {Variable::None, 50.0f, {}}},
                      variables));
    CHECK(holds({Join::And, {Variable::Cast, 0.0f, {}}, Compare::Equal, {Variable::None, 50.0f, {}}}, variables));
    CHECK(valueOf(Operand{Variable::Cooldown, 0.0f, "Heal"}, variables) == 0.0f);
    CHECK_FALSE(valueOf(Operand{Variable::Cooldown, 0.0f, "Shield"}, variables).has_value());
}

TEST_CASE("the runner starts at Root, and goes where the current state's transition passes") {
    Tree tree;
    const int fight = tree.add(tree.root(), "Fight");
    const int attack = tree.add(fight, "Attack");
    const int rest = tree.add(tree.root(), "Rest");
    tree.find(tree.root())->transitions = {{attack, {}}};
    tree.find(attack)->transitions = {{rest, {hpBelow(30.0f)}}};
    tree.find(rest)->transitions = {{fight, {{Join::And, {Variable::Hp, 0}, Compare::Greater, {Variable::None, 90}}}}};
    Runner runner;
    CHECK(runner.current(tree) == tree.root());
    CHECK(runner.tick(tree, withHp(80.0f))); // Root's transition: always.
    CHECK(runner.current(tree) == attack);
    CHECK_FALSE(runner.tick(tree, withHp(80.0f))); // Attack's only now, not Root's.
    CHECK(runner.tick(tree, withHp(20.0f)));
    CHECK(runner.current(tree) == rest);
    CHECK_FALSE(runner.tick(tree, withHp(50.0f)));
    CHECK(runner.tick(tree, withHp(95.0f)));
    CHECK(runner.current(tree) == attack); // Entering Fight goes on down to its first child that can be entered.
    CHECK_FALSE(runner.tick(tree, withHp(95.0f))); // Neither Attack nor Fight goes, and Fight picks Attack again.
    runner.reset();
    CHECK(runner.current(tree) == tree.root());
}

TEST_CASE("the runner skips transitions with no target, and is at Root again when its state goes") {
    Tree tree;
    const int rest = tree.add(tree.root(), "Rest");
    const int flee = tree.add(tree.root(), "Flee");
    tree.find(tree.root())->transitions = {{0, {}}, {flee, {}}};
    Runner runner;
    CHECK(runner.tick(tree, {}));
    CHECK(runner.current(tree) == flee);
    tree.remove(flee);
    CHECK(runner.current(tree) == tree.root()); // Its state gone.
    tree.find(tree.root())->transitions = {{rest, {}}};
    tree.find(rest)->transitions = {{rest, {}}}; // To itself: goes, and stays.
    CHECK(runner.tick(tree, {}));
    CHECK(runner.tick(tree, {}));
    CHECK(runner.current(tree) == rest);
}

TEST_CASE("a state's tasks run one at a time, in order, from the first each time it is entered") {
    Tree tree;
    const int fight = tree.add(tree.root(), "Fight");
    tree.find(fight)->tasks = {{TaskKind::SelectTarget}, {TaskKind::UseSkill, {"Wind Strike"}}};
    tree.find(tree.root())->transitions = {{fight, {}}};
    Runner runner;
    CHECK(runner.task(tree) == -1); // Root has none.
    const std::uint64_t first = runner.taskStarts();
    CHECK(runner.tick(tree, withHp(80.0f)));
    CHECK(runner.task(tree) == 0); // Entered: the first.
    CHECK(runner.taskStarts() == first + 1);
    CHECK_FALSE(runner.tick(tree, withHp(80.0f)));
    CHECK(runner.task(tree) == 0); // Until it is done,
    CHECK(runner.taskStarts() == first + 1);
    runner.finishTask();
    CHECK(runner.task(tree) == 1); // then the next.
    CHECK(runner.taskStarts() == first + 2);
    runner.finishTask();
    CHECK(runner.task(tree) == -1); // All done: none runs,
    CHECK_FALSE(runner.tick(tree, withHp(80.0f)));
    CHECK(runner.task(tree) == -1); // until it is entered again: a transition to itself too.
    tree.find(fight)->transitions = {{fight, {hpBelow(30.0f)}}};
    CHECK(runner.tick(tree, withHp(20.0f)));
    CHECK(runner.task(tree) == 0);
    CHECK(runner.taskStarts() == first + 4);
    runner.finishTask();
    tree.find(fight)->tasks.resize(1); // Its tasks cut under the one running: none runs.
    CHECK(runner.task(tree) == -1);
    runner.reset();
    CHECK(runner.current(tree) == tree.root());
    CHECK(runner.task(tree) == -1);
    CHECK(runner.taskStarts() == first + 6);
}

TEST_CASE("Parallel tasks are skipped by the tasks one after another and run while their state is active") {
    Tree tree;
    const int combat = tree.add(tree.root(), "Combat");
    const int attack = tree.add(combat, "Attack Spell");
    const int rest = tree.add(tree.root(), "Rest");
    Task print{TaskKind::DebugPrint};
    print.parallel = true;
    tree.find(tree.root())->tasks = {print};
    tree.find(combat)->tasks = {print};
    tree.find(attack)->tasks = {{TaskKind::SelectTarget}, print, {TaskKind::UseSkill, {"Wind Strike"}}};
    tree.find(rest)->tasks = {print}; // Only Parallel ones: done at once.
    tree.find(tree.root())->transitions = {{combat, {}}};
    Runner runner;
    // At Root: Root's own (it is active while current), and no task one after another.
    CHECK(Runner::parallelTasks(tree, runner.current(tree)) == std::vector<TaskAt>{{tree.root(), 0}});
    CHECK(runner.task(tree) == -1);
    CHECK(runner.tick(tree, {}));
    REQUIRE(runner.current(tree) == attack); // Combat's first child.
    // Combat's runs while its child does (Root's no longer: Root is active only while current), Attack Spell's too.
    CHECK(Runner::parallelTasks(tree, attack) == std::vector<TaskAt>{{combat, 0}, {attack, 1}});
    CHECK(runner.task(tree) == 0); // Select Target,
    runner.finishTask();
    CHECK(runner.task(tree) == 2); // then Use Skill: the Parallel one between is skipped,
    runner.finishTask();
    CHECK(runner.task(tree) == -1); // then all done.
    // A state with only Parallel tasks is done at once: its When Tasks Done transition goes on the first reading.
    tree.find(attack)->transitions = {{rest, {}, When::TasksDone}};
    CHECK(runner.tick(tree, {}));
    REQUIRE(runner.current(tree) == rest);
    CHECK(runner.task(tree) == -1);
    tree.find(rest)->transitions = {{combat, {}, When::TasksDone}};
    CHECK(runner.tick(tree, {}));
    CHECK(runner.current(tree) == attack);
    CHECK(Runner::parallelTasks(tree, rest) == std::vector<TaskAt>{{rest, 0}});
}

TEST_CASE("Debug Print writes where the brain is") {
    Tree tree;
    const int combat = tree.add(tree.root(), "Combat");
    const int attack = tree.add(combat, "Attack Spell");
    Task print{TaskKind::DebugPrint};
    print.parallel = true;
    tree.find(attack)->tasks = {{TaskKind::UseSkill, {"Wind Strike"}}, print};
    CHECK(std::string(nameOf(TaskKind::DebugPrint)) == "Debug Print");
    CHECK(whereText(tree, attack, 0) == "Combat > Attack Spell, running Use Skill");
    CHECK(whereText(tree, attack, -1) == "Combat > Attack Spell, its tasks done");
    CHECK(whereText(tree, combat, -1) == "Combat"); // No tasks.
    CHECK(whereText(tree, tree.root(), -1) == "Root");
}

TEST_CASE("a Parallel task is written with parallel before its kind, and only kinds that can run alongside read so") {
    CHECK(canRunParallel(TaskKind::DebugPrint));
    CHECK_FALSE(canRunParallel(TaskKind::UseSkill));
    CHECK_FALSE(canRunParallel(TaskKind::SelectTarget));
    CHECK_FALSE(canRunParallel(TaskKind::ClickMouse));
    Tree tree;
    const int combat = tree.add(tree.root(), "Combat");
    Task print{TaskKind::DebugPrint};
    print.parallel = true;
    tree.find(combat)->tasks = {print, {TaskKind::DebugPrint}, {TaskKind::UseSkill, {"Wind Strike"}}};
    const std::string text = writeTree(tree);
    CHECK(text.find("task 2 parallel debug-print\ntask 2 debug-print\ntask 2 use-skill Wind Strike\n") !=
          std::string::npos);
    std::string error;
    const std::optional<Tree> read = readTree(text, &error);
    REQUIRE_MESSAGE(read.has_value(), error);
    CHECK(writeTree(*read) == text);
    const State* readCombat = read->find(2);
    REQUIRE(readCombat != nullptr);
    REQUIRE(readCombat->tasks.size() == 3);
    CHECK(readCombat->tasks[0].parallel);
    CHECK_FALSE(readCombat->tasks[1].parallel);
    CHECK(readCombat->tasks[1].kind == TaskKind::DebugPrint);
    // A kind that cannot run alongside reads as one after another.
    const std::optional<Tree> odd = readTree(
        "observex-brain 1\nstate 1 0 Root\nstate 2 1 Combat\ntask 2 parallel use-skill Wind Strike\n", &error);
    REQUIRE_MESSAGE(odd.has_value(), error);
    REQUIRE(odd->find(2)->tasks.size() == 1);
    CHECK_FALSE(odd->find(2)->tasks[0].parallel);
    CHECK(odd->find(2)->tasks[0].skills == std::vector<std::string>{"Wind Strike"});
}

TEST_CASE("Select Target is written as select-target, and Lock Mouse Onto's old word reads as it") {
    CHECK(std::string(nameOf(TaskKind::SelectTarget)) == "Select Target");
    Tree tree;
    const int fight = tree.add(tree.root(), "Fight");
    Task click;
    click.kind = TaskKind::ClickMouse;
    click.button = ClickButton::Right;
    tree.find(fight)->tasks = {{TaskKind::SelectTarget}, click};
    const std::string text = writeTree(tree);
    CHECK(text.find("task 2 select-target nearest-monster\n") != std::string::npos);
    CHECK(text.find("task 2 click-mouse right\n") != std::string::npos);
    std::string error;
    const std::optional<Tree> read = readTree(text, &error);
    REQUIRE_MESSAGE(read.has_value(), error);
    CHECK(writeTree(*read) == text);

    // A profile saved before the rename (the user's Basic Mage).
    const std::optional<Tree> old = readTree("observex-brain 1\nstate 1 0 Root\nstate 2 1 Combat\n"
                                             "task 2 lock-mouse nearest-monster\ntask 2 use-skill Wind Strike\n",
                                             &error);
    REQUIRE_MESSAGE(old.has_value(), error);
    const State* combat = old->find(2);
    REQUIRE(combat != nullptr);
    REQUIRE(combat->tasks.size() == 2);
    CHECK(combat->tasks[0].kind == TaskKind::SelectTarget);
    CHECK(combat->tasks[0].target == Target::NearestMonster);
    CHECK(combat->tasks[1].kind == TaskKind::UseSkill);
    CHECK(combat->tasks[1].skills == std::vector<std::string>{"Wind Strike"});
    CHECK(writeTree(*old).find("task 2 select-target nearest-monster\n") != std::string::npos); // Saved anew.
}

TEST_CASE("a tree written and read back is the same tree") {
    Tree tree;
    tree.find(tree.root())->name = "Farm brain";
    const int fight = tree.add(tree.root(), "Fight the mob");
    const int attack = tree.add(fight, "Attack");
    const int gone = tree.add(tree.root(), "Gone");
    const int rest = tree.add(tree.root(), "Rest");
    tree.remove(gone); // Ids with a gap.
    tree.find(attack)->transitions = {
        {rest, {hpBelow(30.5f), {Join::Or, {Variable::TargetHp, 0}, Compare::GreaterOrEqual, {Variable::Mp, 0}}}},
        {0, {}}};
    tree.find(rest)->transitions = {{fight, {{Join::And, {Variable::None, -2.25f}, Compare::NotEqual, {Variable::Xp, 0}}}}};
    tree.find(attack)->tasks = {{TaskKind::UseSkill, {"Wind Strike"}}, {TaskKind::UseSkill, {}}};
    const std::string text = writeTree(tree);
    std::string error;
    const std::optional<Tree> read = readTree(text, &error);
    REQUIRE_MESSAGE(read.has_value(), error);
    CHECK(shown(*read) == shown(tree));
    CHECK(writeTree(*readTree(writeTree(*read), nullptr)) == writeTree(*read));
    // The transitions, by name.
    const auto byName = [](const Tree& t, const char* name) {
        for (const Row& row : t.rows()) {
            if (t.find(row.state)->name == name) {
                return t.find(row.state);
            }
        }
        return static_cast<const State*>(nullptr);
    };
    const State* readAttack = byName(*read, "Attack");
    REQUIRE(readAttack->tasks.size() == 2);
    CHECK(readAttack->tasks[0].kind == TaskKind::UseSkill);
    CHECK(readAttack->tasks[0].skills == std::vector<std::string>{"Wind Strike"});
    CHECK(readAttack->tasks[1].skills.empty()); // None chosen yet.
    REQUIRE(readAttack->transitions.size() == 2);
    CHECK(read->find(readAttack->transitions[0].target)->name == "Rest");
    CHECK(readAttack->transitions[1].target == 0);
    const Condition& second = readAttack->transitions[0].conditions[1];
    CHECK(second.join == Join::Or);
    CHECK(second.left.variable == Variable::TargetHp);
    CHECK(second.compare == Compare::GreaterOrEqual);
    CHECK(second.right.variable == Variable::Mp);
    CHECK(readAttack->transitions[0].conditions[0].right.number == 30.5f);
    const Condition& rested = byName(*read, "Rest")->transitions[0].conditions[0];
    CHECK(rested.left.number == -2.25f);
    CHECK(rested.compare == Compare::NotEqual);
}

TEST_CASE("the profile's skills are written and read back, and old typed tasks are left out") {
    Tree tree;
    tree.skills() = {{"Wind Strike", true, 2, {1, 2, 3, 4, 5, 6, 7, 8, 9, 250, 251, 252}}, {"Sit", false, 0, {}}};
    const std::string text = writeTree(tree);
    CHECK(text.find("skill cooldown 2 010203040506070809fafbfc Wind Strike\n") != std::string::npos);
    CHECK(text.find("skill no-cooldown 0 - Sit\n") != std::string::npos);
    std::string error;
    const std::optional<Tree> read = readTree(text, &error);
    REQUIRE_MESSAGE(read.has_value(), error);
    CHECK(read->skills() == tree.skills());
    REQUIRE(read->skill("wind strike") != nullptr); // Ignoring case.
    CHECK(read->skill("wind strike")->side == 2);
    CHECK_FALSE(read->skill("Sit")->cooldown);
    CHECK(read->skill("Heal") == nullptr);

    // A brain from before tasks had kinds: its typed tasks are left out, the rest read.
    const std::optional<Tree> old =
        readTree("observex-brain 1\nstate 1 0 Root\nstate 2 1 Fight\ntask 2 Fake task\ntask 2 use-skill Sit\n",
                 &error);
    REQUIRE_MESSAGE(old.has_value(), error);
    REQUIRE(old->find(2) != nullptr);
    REQUIRE(old->find(2)->tasks.size() == 1);
    CHECK(old->find(2)->tasks[0].skills == std::vector<std::string>{"Sit"});

    // A skill's pixels must be its side squared, three bytes each, in hex.
    CHECK_FALSE(readTree("observex-brain 1\nskill cooldown 2 0102 Wind\nstate 1 0 Root\n", &error).has_value());
    CHECK_FALSE(readTree("observex-brain 1\nskill cooldown 1 zz0102 Wind\nstate 1 0 Root\n", &error).has_value());
    CHECK_FALSE(readTree("observex-brain 1\nskill maybe 0 - Wind\nstate 1 0 Root\n", &error).has_value());
}

TEST_CASE("a skill's shortcut is written and read back, and a profile without shortcuts still reads") {
    Tree tree;
    tree.skills() = {{"Wind Strike", true, 0, {}, "F2"}, {"Sit", false, 0, {}, "A+K1"}, {"Heal", true, 0, {}, ""}};
    const std::string text = writeTree(tree);
    CHECK(text.find("skill cooldown key=F2 0 - Wind Strike\n") != std::string::npos);
    CHECK(text.find("skill no-cooldown key=A+K1 0 - Sit\n") != std::string::npos);
    CHECK(text.find("skill cooldown 0 - Heal\n") != std::string::npos); // Not read yet: no key word.
    std::string error;
    const std::optional<Tree> read = readTree(text, &error);
    REQUIRE_MESSAGE(read.has_value(), error);
    CHECK(read->skills() == tree.skills());
    CHECK(read->skill("Sit")->key == "A+K1");
    CHECK(read->skill("Heal")->key.empty());

    // Odd keys stay one word: no key written ("-"), letters not known yet, "=".
    tree.skills() = {{"A", true, 0, {}, "-"}, {"B", true, 0, {}, "F?"}, {"C", true, 0, {}, "="}};
    const std::optional<Tree> odd = readTree(writeTree(tree), &error);
    REQUIRE_MESSAGE(odd.has_value(), error);
    CHECK(odd->skills() == tree.skills());
}

TEST_CASE("a pressed skill is not ready, and the Nearby counts are variables") {
    GameStats stats;
    setGauge(stats, kSkillsPiece, "Wind Strike", 0.0f);
    stats.gauges[static_cast<std::size_t>(stats.count - 1)].pressed = true;
    setGauge(stats, kNearbyPiece, kCharactersGauge, -1.0f, true, "3");
    setGauge(stats, kNearbyPiece, kMonstersGauge, -1.0f, false); // The model knows no kinds.
    const Variables variables = variablesOf(stats);
    REQUIRE(variables.skill("Wind Strike") != nullptr);
    CHECK(variables.skill("Wind Strike")->pressed);
    Condition ready;
    ready.kind = Kind::Skill;
    ready.text = "Wind Strike";
    ready.is = SkillIs::Ready;
    CHECK_FALSE(holds(ready, variables));
    ready.is = SkillIs::CoolingDown;
    CHECK_FALSE(holds(ready, variables)); // Not cooling yet either.
    CHECK(variables.known[static_cast<int>(Variable::Characters)]);
    CHECK(variables.value[static_cast<int>(Variable::Characters)] == 3.0f);
    CHECK_FALSE(variables.known[static_cast<int>(Variable::Monsters)]);
    CHECK(isCount(Variable::Characters));
    CHECK_FALSE(isCount(Variable::Hp));
}

TEST_CASE("the target, name and skill conditions are written and read back") {
    Tree tree;
    const int rest = tree.add(tree.root(), "Rest");
    tree.find(rest)->transitions = {
        {rest,
         {ofKind(Kind::HasTarget, "", Compare::NotEqual), ofKind(Kind::TargetName, "Ashen Wolf"),
          ofKind(Kind::TargetName, "", Compare::NotEqual), ofKind(Kind::Skill, "Wind Strike", Compare::Equal, SkillIs::Cast),
          ofKind(Kind::Skill, "Heal", Compare::Equal, SkillIs::CoolingDown),
          {Join::Or, {Variable::Cooldown, 0.0f, "Wind Strike"}, Compare::LessOrEqual, {Variable::Cast, 0.0f, {}}}}}};
    const std::string text = writeTree(tree);
    CHECK(text.find("if and has-target no\n") != std::string::npos);
    CHECK(text.find("if and target-name == Ashen Wolf\n") != std::string::npos);
    CHECK(text.find("if and skill cast Wind Strike\n") != std::string::npos);
    CHECK(text.find("if or cooldown:Wind_Strike <= cast\n") != std::string::npos);
    std::string error;
    const std::optional<Tree> read = readTree(text, &error);
    REQUIRE_MESSAGE(read.has_value(), error);
    CHECK(writeTree(*read) == text);
    const std::vector<Condition>& conditions = read->find(read->rows()[1].state)->transitions[0].conditions;
    REQUIRE(conditions.size() == 6);
    CHECK(conditions[0].kind == Kind::HasTarget);
    CHECK(conditions[0].compare == Compare::NotEqual);
    CHECK(conditions[1].text == "Ashen Wolf");
    CHECK(conditions[2].text.empty());
    CHECK(conditions[3].is == SkillIs::Cast);
    CHECK(conditions[4].is == SkillIs::CoolingDown);
    CHECK(conditions[5].left.skill == "Wind Strike");
    CHECK(conditions[5].right.variable == Variable::Cast);
    CHECK_FALSE(readTree("observex-brain 1\nstate 1 0 Root\ngo 1 1\nif and skill ready\n", &error).has_value());
    CHECK(error.starts_with("Line 4: a skill condition is"));
    CHECK_FALSE(readTree("observex-brain 1\nstate 1 0 Root\ngo 1 1\nif and has-target maybe\n", &error).has_value());
    CHECK(error.starts_with("Line 4: a target condition is"));
}

TEST_CASE("the history keeps each change once, and undoes and redoes whole trees, ids and all") {
    Tree tree;
    History history;
    history.reset(tree);
    CHECK_FALSE(history.keep(tree)); // Nothing changed.
    Tree before = tree;
    CHECK_FALSE(history.undo(&before));

    const int fight = tree.add(tree.root(), "Fight");
    CHECK(history.keep(tree));
    CHECK_FALSE(history.keep(tree)); // Kept once.
    tree.find(fight)->transitions.push_back({fight, {hpBelow(30.0f)}});
    tree.find(fight)->tasks.push_back({TaskKind::UseSkill, {"Wind Strike"}});
    CHECK(history.keep(tree));
    CHECK(history.text() == writeTree(tree)); // What the file is to hold.

    CHECK(history.undo(&tree)); // The transition and the task go.
    CHECK(tree.find(fight) != nullptr);
    CHECK(tree.find(fight)->transitions.empty());
    CHECK(tree.find(fight)->tasks.empty());
    CHECK_FALSE(history.keep(tree)); // An undone tree is the one kept, not a new change.
    CHECK(history.undo(&tree)); // Fight goes.
    CHECK(shown(tree) == std::vector<std::string>{"Root/0"});
    CHECK_FALSE(history.undo(&tree));

    CHECK(history.redo(&tree));
    CHECK(history.redo(&tree));
    REQUIRE(tree.find(fight) != nullptr); // The same id: a selection still points at it.
    CHECK(tree.find(fight)->tasks.size() == 1);
    CHECK_FALSE(history.redo(&tree));

    CHECK(history.undo(&tree));
    tree.find(fight)->name = "Attack"; // A new change after an undo: what was undone cannot come back.
    CHECK(history.keep(tree));
    CHECK_FALSE(history.redo(&tree));
    CHECK(history.undo(&tree));
    CHECK(tree.find(fight)->name == "Fight");

    history.reset(tree); // Loaded: nothing to undo.
    CHECK_FALSE(history.undo(&tree));
}

TEST_CASE("the history drops its oldest steps past its limit") {
    Tree tree;
    History history;
    history.reset(tree);
    const int state = tree.add(tree.root(), "0");
    history.keep(tree);
    for (std::size_t i = 1; i <= History::kUndoSteps + 5; ++i) {
        tree.find(state)->name = std::to_string(i);
        history.keep(tree);
    }
    std::size_t undone = 0;
    while (history.undo(&tree)) {
        ++undone;
    }
    CHECK(undone == History::kUndoSteps);
    CHECK(tree.find(state)->name == "5");
}

TEST_CASE("text that is not a brain is refused, and says where") {
    std::string error;
    CHECK_FALSE(readTree("hello\n", &error).has_value());
    CHECK(error == "Line 1: not an observex brain");
    CHECK_FALSE(readTree("", &error).has_value());
    CHECK_FALSE(readTree("observex-brain 1\nstate 2 1 Orphan\n", &error).has_value());
    CHECK(error == "Line 2: a state before its parent");
    CHECK_FALSE(readTree("observex-brain 1\nstate 1 0 Root\nstate 2 0 Another\n", &error).has_value());
    CHECK(error == "Line 3: only Root, the first state, has no parent");
    CHECK_FALSE(readTree("observex-brain 1\nstate 1 0 Root\nstate 2 5 Lost\n", &error).has_value());
    CHECK(error == "Line 3: a state before its parent");
    CHECK_FALSE(readTree("observex-brain 1\nstate 1 0 Root\ngo 1 1\nif and hp ~ 5\n", &error).has_value());
    CHECK(error.starts_with("Line 4: a condition is"));
    CHECK_FALSE(readTree("observex-brain 1\nstate 1 0 Root\nif and hp < 5\n", &error).has_value());
    CHECK(error == "Line 3: a condition before any transition or enter line");
    const std::optional<Tree> crlf = readTree("# a note\r\nobservex-brain 1\r\nstate 7 0 Root\r\n", &error);
    REQUIRE(crlf.has_value());
    CHECK(shown(*crlf) == std::vector<std::string>{"Root/0"});
}

namespace {

// A reading with these skills seen: each found, its cooldown left in percent (0: ready).
Variables withSkills(std::initializer_list<std::pair<const char*, float>> skills) {
    Variables variables;
    for (const auto& [name, cooldown] : skills) {
        variables.skills.push_back({name, true, cooldown, false});
    }
    return variables;
}

Condition skillsReady(std::vector<std::string> skills, Need need = Need::AnySkill) {
    Condition condition;
    condition.kind = Kind::SkillsReady;
    condition.skills = std::move(skills);
    condition.need = need;
    return condition;
}

} // namespace

TEST_CASE("Is Skill Ready holds when any, or all, of its skills are ready") {
    const Variables variables = withSkills({{"Wind Strike", 0.0f}, {"Ice Bolt", 40.0f}, {"Heal", 0.0f}});
    CHECK(isReady(variables, "wind strike"));
    CHECK_FALSE(isReady(variables, "Ice Bolt"));
    CHECK_FALSE(isReady(variables, "Not added"));
    CHECK(holds(skillsReady({"Ice Bolt", "Wind Strike"}), variables));
    CHECK_FALSE(holds(skillsReady({"Ice Bolt", "Wind Strike"}, Need::AllSkills), variables));
    CHECK(holds(skillsReady({"Heal", "Wind Strike"}, Need::AllSkills), variables));
    CHECK_FALSE(holds(skillsReady({"Ice Bolt"}), variables));
    CHECK_FALSE(holds(skillsReady({}), variables)); // None chosen: never.
    CHECK_FALSE(holds(skillsReady({"Heal", "Not added"}, Need::AllSkills), variables)); // Not seen: not ready.
    Variables pressed = variables;
    pressed.skills[0].pressed = true; // Just pressed: not ready.
    CHECK_FALSE(holds(skillsReady({"Wind Strike"}), pressed));
    CHECK(std::string(nameOf(Need::AnySkill)) == "Any Skill");
    CHECK(std::string(nameOf(Need::AllSkills)) == "All Skills");
}

TEST_CASE("conditions are described in a few words") {
    CHECK(describe({}).empty());
    CHECK(describe({skillsReady({"Wind Strike"})}) == "Wind Strike ready");
    CHECK(describe({skillsReady({"Wind Strike", "Ice Bolt", "Heal"})}) == "Wind Strike, Ice Bolt or Heal ready");
    CHECK(describe({skillsReady({"Wind Strike", "Ice Bolt"}, Need::AllSkills), hpBelow(50.0f, Join::Or)}) ==
          "Wind Strike and Ice Bolt ready or HP < 50");
    CHECK(describe({ofKind(Kind::HasTarget, "", Compare::NotEqual)}) == "no target");
}

TEST_CASE("enter conditions: a state is entered only while they pass, and a parent enters its first child that can") {
    Tree tree;
    const int seek = tree.add(tree.root(), "Seek");
    const int combat = tree.add(tree.root(), "Combat");
    const int strike = tree.add(combat, "Wind Strike");
    const int melee = tree.add(combat, "Melee");
    tree.find(strike)->enter = {skillsReady({"Wind Strike"})};
    tree.find(tree.root())->transitions = {{seek, {}}};
    tree.find(seek)->transitions = {{combat, {}}};
    Runner runner;
    CHECK(runner.tick(tree, {}));
    CHECK(runner.current(tree) == seek); // Root picks no child itself: its transition goes.
    const std::uint64_t starts = runner.taskStarts();
    CHECK(runner.tick(tree, withSkills({{"Wind Strike", 0.0f}})));
    CHECK(runner.current(tree) == strike); // Combat's first child that can be entered,
    CHECK(runner.taskStarts() == starts + 1);
    CHECK(Runner::landing(tree, combat, withSkills({{"Wind Strike", 50.0f}})) == melee); // or the next.
    CHECK(runner.active(tree, combat)); // Its parent is active too,
    CHECK_FALSE(runner.active(tree, tree.root())); // Root only while current,
    CHECK_FALSE(runner.active(tree, seek));

    // A transition whose target's enter conditions fail does not go; the next one is tried.
    tree.find(melee)->enter = {hpBelow(10.0f)};
    CHECK(Runner::landing(tree, combat, withSkills({{"Wind Strike", 50.0f}})) == combat); // None can: Combat itself.
    tree.find(strike)->transitions = {{melee, {}}, {seek, {}}};
    CHECK(runner.tick(tree, withHp(50.0f)));
    CHECK(runner.current(tree) == seek);
}

TEST_CASE("a parent's transitions count while its child runs, the child's first") {
    Tree tree;
    const int seek = tree.add(tree.root(), "Seek");
    const int combat = tree.add(tree.root(), "Combat");
    const int strike = tree.add(combat, "Wind Strike");
    const int rest = tree.add(tree.root(), "Rest");
    tree.find(tree.root())->transitions = {{combat, {}}};
    tree.find(combat)->transitions = {{seek, {ofKind(Kind::HasTarget, "", Compare::NotEqual)}}};
    tree.find(strike)->transitions = {{rest, {hpBelow(30.0f)}}};
    Runner runner;
    Variables fighting = withHp(80.0f);
    fighting.hasTarget = true;
    CHECK(runner.tick(tree, fighting));
    CHECK(runner.current(tree) == strike);
    CHECK_FALSE(runner.tick(tree, fighting));
    const Variables both = withHp(20.0f); // No target, and low HP: the child's own goes first.
    CHECK(runner.tick(tree, both));
    CHECK(runner.current(tree) == rest);
    runner.reset();
    CHECK(runner.tick(tree, fighting));
    CHECK(runner.tick(tree, withHp(80.0f))); // The target gone: Combat's goes from Wind Strike.
    CHECK(runner.current(tree) == seek);
    // Root's transition is not tried from under it (Root -> Combat always passes).
    CHECK_FALSE(runner.tick(tree, withHp(80.0f)));
    CHECK(runner.current(tree) == seek);
}

TEST_CASE("once its tasks are done the parent picks again, moving only to another state") {
    Tree tree;
    const int combat = tree.add(tree.root(), "Combat");
    const int strike = tree.add(combat, "Wind Strike");
    const int melee = tree.add(combat, "Melee");
    tree.find(strike)->enter = {skillsReady({"Wind Strike"})};
    tree.find(strike)->tasks = {{TaskKind::UseSkill, {"Wind Strike"}}};
    tree.find(melee)->tasks = {{TaskKind::UseSkill, {"Attack"}}};
    tree.find(tree.root())->transitions = {{combat, {}}};
    const Variables ready = withSkills({{"Wind Strike", 0.0f}});
    const Variables cooling = withSkills({{"Wind Strike", 60.0f}});
    Runner runner;
    CHECK(runner.tick(tree, ready));
    CHECK(runner.current(tree) == strike);
    CHECK_FALSE(runner.tick(tree, cooling)); // Its task runs: no picking.
    runner.finishTask();
    CHECK(runner.tick(tree, cooling)); // Done, and Wind Strike is cooling: Melee.
    CHECK(runner.current(tree) == melee);
    CHECK(runner.task(tree) == 0);
    runner.finishTask();
    const std::uint64_t starts = runner.taskStarts();
    CHECK_FALSE(runner.tick(tree, cooling)); // Melee again: it stays, its task not run again.
    CHECK(runner.task(tree) == -1);
    CHECK(runner.taskStarts() == starts);
    CHECK(runner.tick(tree, ready)); // Wind Strike ready again.
    CHECK(runner.current(tree) == strike);
    CHECK(runner.task(tree) == 0);

    // A parent none of whose children could be entered picks on every reading, once its own tasks are done.
    tree.find(melee)->enter = {hpBelow(10.0f)};
    runner.finishTask();
    CHECK(runner.tick(tree, cooling));
    CHECK(runner.current(tree) == combat);
    CHECK_FALSE(runner.tick(tree, cooling));
    CHECK(runner.tick(tree, ready));
    CHECK(runner.current(tree) == strike);

    // Root's own children are not picked again: a state under Root whose tasks are done stays.
    Tree flat;
    const int rest = flat.add(flat.root(), "Rest");
    flat.add(flat.root(), "Other");
    flat.find(flat.root())->transitions = {{rest, {}}};
    Runner plain;
    CHECK(plain.tick(flat, {}));
    CHECK_FALSE(plain.tick(flat, {}));
    CHECK(plain.current(flat) == rest);
}

TEST_CASE("enter conditions, skill lists, Is Skill Ready and MP costs are written and read back") {
    Tree tree;
    tree.skills() = {{"Wind Strike", true, 0, {}, "F2", 10}, {"Sit", false, 0, {}, "F12", 0}};
    const int combat = tree.add(tree.root(), "Combat");
    const int strike = tree.add(combat, "Strike");
    tree.find(strike)->enter = {skillsReady({"Wind Strike", "Ice Bolt"}, Need::AllSkills), hpBelow(90.0f, Join::Or)};
    tree.find(strike)->tasks = {{TaskKind::UseSkill, {"Wind Strike", "Ice Bolt"}}};
    tree.find(combat)->transitions = {{strike, {skillsReady({"Ice Bolt"})}}};
    const std::string text = writeTree(tree);
    CHECK(text.find("skill cooldown key=F2 mp=10 0 - Wind Strike\n") != std::string::npos);
    CHECK(text.find("skill no-cooldown key=F12 0 - Sit\n") != std::string::npos); // No cost: no mp word.
    CHECK(text.find("task 3 use-skill Wind Strike|Ice Bolt\n") != std::string::npos);
    CHECK(text.find("enter 3\nif and skills-ready all Wind Strike|Ice Bolt\nif or hp < 90\n") != std::string::npos);
    CHECK(text.find("go 2 3\nif and skills-ready any Ice Bolt\n") != std::string::npos);
    std::string error;
    const std::optional<Tree> read = readTree(text, &error);
    REQUIRE_MESSAGE(read.has_value(), error);
    CHECK(writeTree(*read) == text);
    CHECK(read->skills() == tree.skills());
    CHECK(read->skill("Wind Strike")->mp == 10);
    const State& readStrike = *read->find(read->rows()[2].state);
    REQUIRE(readStrike.enter.size() == 2);
    CHECK(readStrike.enter[0].kind == Kind::SkillsReady);
    CHECK(readStrike.enter[0].need == Need::AllSkills);
    CHECK(readStrike.enter[0].skills == std::vector<std::string>{"Wind Strike", "Ice Bolt"});
    CHECK(readStrike.enter[1].join == Join::Or);
    CHECK(readStrike.tasks[0].skills == std::vector<std::string>{"Wind Strike", "Ice Bolt"});
    CHECK(readStrike.transitions.empty());
    CHECK_FALSE(readTree("observex-brain 1\nstate 1 0 Root\nenter 4\n", &error).has_value());
    CHECK(error == "Line 3: enter conditions of a state not there");
    CHECK_FALSE(readTree("observex-brain 1\nskill cooldown mp=x 0 - Wind\nstate 1 0 Root\n", &error).has_value());
    CHECK_FALSE(
        readTree("observex-brain 1\nstate 1 0 Root\ngo 1 1\nif and skills-ready some Wind\n", &error).has_value());
    CHECK(error.starts_with("Line 4: a skills condition is"));
}

TEST_CASE("MP in points comes from the number on the MP bar") {
    GameStats stats;
    setGauge(stats, "Status", "MP", 0.83f, true, "90/108");
    Variables variables = variablesOf(stats);
    CHECK(variables.mpPointsKnown);
    CHECK(variables.mpPoints == 90.0f);
    CHECK(variables.mpMax == 108.0f);
    GameStats unread;
    setGauge(unread, "Status", "MP", 0.83f, true, ""); // The bar, no number.
    CHECK_FALSE(variablesOf(unread).mpPointsKnown);
    CHECK(variablesOf(unread).known[static_cast<int>(Variable::Mp)]);
    GameStats casting;
    setGauge(casting, kCastingPiece, kCastingPiece, 0.4f, true, "Wind Strike");
    variables = variablesOf(casting);
    CHECK(variables.castShown);
    CHECK(variables.casting == "Wind Strike");
}

TEST_CASE("When Tasks Done transitions go only once the tasks are done, after every Any Time one") {
    Tree tree;
    const int seek = tree.add(tree.root(), "Seek");
    const int combat = tree.add(tree.root(), "Combat");
    const int strike = tree.add(combat, "Attack Spell");
    tree.find(strike)->tasks = {{TaskKind::UseSkill, {"Wind Strike"}}};
    tree.find(tree.root())->transitions = {{combat, {}}};
    // Combat leaves for Seek at any time once the target is gone; Attack Spell repeats itself once its cast is done.
    tree.find(combat)->transitions = {{seek, {ofKind(Kind::HasTarget, "", Compare::NotEqual)}}};
    tree.find(strike)->transitions = {{strike, {}, When::TasksDone}};
    Variables fighting = withHp(80.0f);
    fighting.hasTarget = true;
    Runner runner;
    CHECK(runner.tick(tree, fighting));
    CHECK(runner.current(tree) == strike);
    const std::uint64_t starts = runner.taskStarts();
    CHECK_FALSE(runner.tick(tree, fighting)); // Its task runs: the repeat waits, and the task is not started again.
    CHECK_FALSE(runner.tick(tree, fighting));
    CHECK(runner.taskStarts() == starts);
    runner.finishTask();
    CHECK(runner.tick(tree, fighting)); // Done: into itself again, its task from the first.
    CHECK(runner.current(tree) == strike);
    CHECK(runner.task(tree) == 0);
    CHECK(runner.taskStarts() == starts + 2); // The task's end, and its start again.
    CHECK(runner.tick(tree, withHp(80.0f))); // The target gone while the task runs: Combat's Any Time one goes.
    CHECK(runner.current(tree) == seek);

    // Both could go (its task done, and no target): the parent's Any Time one wins over the child's repeat.
    runner.reset();
    CHECK(runner.tick(tree, fighting));
    runner.finishTask();
    CHECK(runner.tick(tree, withHp(80.0f)));
    CHECK(runner.current(tree) == seek);

    // Any Time into itself is entered again on every reading, its task started afresh each time (what the repeat
    // avoids).
    tree.find(strike)->transitions.front().when = When::AnyTime;
    runner.reset();
    CHECK(runner.tick(tree, fighting));
    const std::uint64_t again = runner.taskStarts();
    CHECK(runner.tick(tree, fighting));
    CHECK(runner.tick(tree, fighting));
    CHECK(runner.taskStarts() == again + 2);
}

TEST_CASE("a state with no tasks is done at once, and a parent's When Tasks Done transition counts for its child") {
    Tree tree;
    const int rest = tree.add(tree.root(), "Rest");
    const int combat = tree.add(tree.root(), "Combat");
    const int strike = tree.add(combat, "Attack Spell");
    tree.find(strike)->tasks = {{TaskKind::UseSkill, {"Wind Strike"}}};
    tree.find(tree.root())->transitions = {{rest, {}, When::TasksDone}};
    tree.find(rest)->transitions = {{combat, {}, When::TasksDone}};
    tree.find(combat)->transitions = {{rest, {hpBelow(30.0f)}, When::TasksDone}};
    Runner runner;
    CHECK(runner.tick(tree, withHp(80.0f))); // Root has no tasks: done at once.
    CHECK(runner.current(tree) == rest);
    CHECK(runner.tick(tree, withHp(80.0f))); // Nor has Rest.
    CHECK(runner.current(tree) == strike);
    CHECK_FALSE(runner.tick(tree, withHp(20.0f))); // HP low, but Attack Spell's task still runs.
    runner.finishTask();
    CHECK_FALSE(runner.tick(tree, withHp(80.0f))); // Done, Combat's conditions fail, and picking again lands here.
    CHECK(runner.current(tree) == strike);
    CHECK(runner.tick(tree, withHp(20.0f))); // Done and HP low: Combat's goes.
    CHECK(runner.current(tree) == rest);
}

TEST_CASE("a transition's When is written and read back, and older files read as Any Time") {
    Tree tree;
    const int rest = tree.add(tree.root(), "Rest");
    tree.find(tree.root())->transitions = {{rest, {}}};
    tree.find(rest)->transitions = {{rest, {hpBelow(50.0f)}, When::TasksDone}};
    const std::string text = writeTree(tree);
    std::string why;
    const std::optional<Tree> back = readTree(text, &why);
    REQUIRE(back);
    const State& root = *back->find(back->root());
    REQUIRE(root.children.size() == 1);
    const State& restBack = *back->find(root.children.front());
    REQUIRE(root.transitions.size() == 1);
    REQUIRE(restBack.transitions.size() == 1);
    CHECK(root.transitions.front().when == When::AnyTime);
    CHECK(restBack.transitions.front().when == When::TasksDone);
    CHECK(restBack.transitions.front().target == restBack.id);
    CHECK(restBack.transitions.front().conditions.size() == 1);
    CHECK(writeTree(*back) == text);

    const std::optional<Tree> old = readTree("observex-brain 1\nstate 1 0 Root\nstate 2 1 Rest\ngo 1 2\n", &why);
    REQUIRE(old);
    CHECK(old->find(old->root())->transitions.front().when == When::AnyTime);
    CHECK_FALSE(readTree("observex-brain 1\nstate 1 0 Root\ngo 1 1 later\n", &why));
    CHECK(why == "Line 3: a transition is: go FROM TO [done]");
    CHECK(std::string(nameOf(When::AnyTime)) == "Any time");
    CHECK(std::string(nameOf(When::TasksDone)) == "Tasks done");
}

TEST_CASE("a state's path of names finds it again in a tree read back") {
    Tree tree;
    const int combat = tree.add(tree.root(), "Combat");
    const int strike = tree.add(combat, "Strike");
    const int rest = tree.add(tree.root(), "Rest");
    const int restStrike = tree.add(rest, "Strike"); // The same name under another parent.
    CHECK(statePath(tree, tree.root()).empty());
    CHECK(statePath(tree, combat) == "Combat");
    CHECK(statePath(tree, strike) == "Combat > Strike");
    CHECK(statePath(tree, restStrike) == "Rest > Strike");
    CHECK(statePath(tree, 999).empty());
    CHECK(stateAtPath(tree, "") == tree.root());
    CHECK(stateAtPath(tree, "Combat > Strike") == strike);
    CHECK(stateAtPath(tree, "Rest > Strike") == restStrike);
    CHECK(stateAtPath(tree, "Strike") == 0);
    CHECK(stateAtPath(tree, "Combat > Hit") == 0);

    // Read back (the ids may change), the same path finds the same state.
    std::string why;
    const std::optional<Tree> read = readTree(writeTree(tree), &why);
    REQUIRE(read);
    const int found = stateAtPath(*read, "Combat > Strike");
    REQUIRE(found != 0);
    CHECK(read->find(found)->name == "Strike");
    CHECK(read->find(read->find(found)->parent)->name == "Combat");

    // Active as shown: the state and the ones above it, Root only while it is the one.
    CHECK(Runner::activeFrom(tree, strike, strike));
    CHECK(Runner::activeFrom(tree, strike, combat));
    CHECK_FALSE(Runner::activeFrom(tree, strike, tree.root()));
    CHECK_FALSE(Runner::activeFrom(tree, strike, rest));
    CHECK(Runner::activeFrom(tree, tree.root(), tree.root()));
}
