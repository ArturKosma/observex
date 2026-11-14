#include "core/StateTree.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <cstring>
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
    CHECK(runner.current(tree) == fight); // A state with children stays itself.
    CHECK_FALSE(runner.tick(tree, withHp(95.0f))); // Fight has none.
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
    tree.find(attack)->tasks = {{TaskKind::UseSkill, "Wind Strike"}, {TaskKind::UseSkill, ""}};
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
    CHECK(readAttack->tasks[0].skill == "Wind Strike");
    CHECK(readAttack->tasks[1].skill.empty()); // None chosen yet.
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
    CHECK(old->find(2)->tasks[0].skill == "Sit");

    // A skill's pixels must be its side squared, three bytes each, in hex.
    CHECK_FALSE(readTree("observex-brain 1\nskill cooldown 2 0102 Wind\nstate 1 0 Root\n", &error).has_value());
    CHECK_FALSE(readTree("observex-brain 1\nskill cooldown 1 zz0102 Wind\nstate 1 0 Root\n", &error).has_value());
    CHECK_FALSE(readTree("observex-brain 1\nskill maybe 0 - Wind\nstate 1 0 Root\n", &error).has_value());
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
    tree.find(fight)->tasks.push_back({TaskKind::UseSkill, "Wind Strike"});
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
    CHECK(error == "Line 3: a condition before any transition");
    const std::optional<Tree> crlf = readTree("# a note\r\nobservex-brain 1\r\nstate 7 0 Root\r\n", &error);
    REQUIRE(crlf.has_value());
    CHECK(shown(*crlf) == std::vector<std::string>{"Root/0"});
}
