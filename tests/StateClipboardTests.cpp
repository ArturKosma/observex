#include "core/StateClipboard.h"

#include <doctest/doctest.h>

#include <string>
#include <vector>

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

Condition hpBelow(float value) {
    return {Join::And, {Variable::Hp, 0.0f, {}}, Compare::Less, {Variable::None, value, {}}};
}

// The name of the state `transition` goes to; "" for none.
std::string targetName(const Tree& tree, const Transition& transition) {
    const State* target = tree.find(transition.target);
    return target != nullptr ? target->name : std::string();
}

} // namespace

TEST_CASE("a copied transition pastes into another state, its target, When and conditions kept") {
    Tree tree;
    const int combat = tree.add(tree.root(), "Combat");
    const int rest = tree.add(tree.root(), "Rest");
    Transition toRest;
    toRest.target = rest;
    toRest.when = When::TasksDone;
    toRest.conditions = {hpBelow(30.0f)};
    tree.find(combat)->transitions = {toRest};

    Clipboard clipboard;
    CHECK(clipboard.what() == Clipboard::What::Nothing);
    CHECK_FALSE(clipboard.pasteTransition(tree, rest, 0)); // Nothing copied yet.
    clipboard.copyTransition(tree, tree.find(combat)->transitions[0]);
    CHECK(clipboard.what() == Clipboard::What::Transition);

    // Changing the original after the copy changes nothing in it.
    tree.find(combat)->transitions[0].conditions.clear();

    REQUIRE(clipboard.pasteTransition(tree, tree.root(), 0));
    const Transition& pasted = tree.find(tree.root())->transitions.at(0);
    CHECK(pasted.target == rest);
    CHECK(pasted.when == When::TasksDone);
    REQUIRE(pasted.conditions.size() == 1);
    CHECK(pasted.conditions[0].right.number == 30.0f);

    // As often as wanted, in place: before the one there.
    Transition other;
    other.target = combat;
    tree.find(rest)->transitions = {other};
    REQUIRE(clipboard.pasteTransition(tree, rest, 0));
    REQUIRE(tree.find(rest)->transitions.size() == 2);
    CHECK(tree.find(rest)->transitions[0].target == rest);
    CHECK(tree.find(rest)->transitions[1].target == combat);

    CHECK_FALSE(clipboard.pasteTransition(tree, rest, 3)); // Past the end.
    CHECK_FALSE(clipboard.pasteTransition(tree, 999, 0));  // No such state.
    CHECK_FALSE(clipboard.pasteCondition(tree.find(rest)->enter, 0)); // Not a condition copied.
}

TEST_CASE("a copied transition's target is the same state renamed or moved, or by its path once read again") {
    Tree tree;
    const int combat = tree.add(tree.root(), "Combat");
    const int rest = tree.add(tree.root(), "Rest");
    Transition toRest;
    toRest.target = rest;
    Clipboard clipboard;
    clipboard.copyTransition(tree, toRest);

    // Renamed and moved under Combat since: still that state, by its id.
    tree.find(rest)->name = "Resting";
    tree.move(rest, combat, 0);
    REQUIRE(clipboard.pasteTransition(tree, tree.root(), 0));
    CHECK(tree.find(tree.root())->transitions[0].target == rest);

    // Another profile: ids mean other states there, so by its path ("Rest" as it was copied).
    Tree other;
    const int otherRest = other.add(other.root(), "Rest");
    const int taken = other.add(other.root(), "Kill");
    REQUIRE(taken == rest); // That id is another state there.
    clipboard.forgetIds();
    REQUIRE(clipboard.pasteTransition(other, taken, 0));
    CHECK(other.find(taken)->transitions[0].target == otherRest);

    // No state at that path: none chosen.
    Tree empty;
    const int lone = empty.add(empty.root(), "Lone");
    REQUIRE(clipboard.pasteTransition(empty, lone, 0));
    CHECK(empty.find(lone)->transitions[0].target == 0);
}

TEST_CASE("a copied transition to a state deleted since goes to the state at its path, else to none") {
    Tree tree;
    const int rest = tree.add(tree.root(), "Rest");
    Transition toRest;
    toRest.target = rest;
    Clipboard clipboard;
    clipboard.copyTransition(tree, toRest);
    tree.remove(rest);
    REQUIRE(clipboard.pasteTransition(tree, tree.root(), 0));
    CHECK(tree.find(tree.root())->transitions[0].target == 0);

    const int newRest = tree.add(tree.root(), "Rest");
    REQUIRE(clipboard.pasteTransition(tree, newRest, 0));
    CHECK(tree.find(newRest)->transitions[0].target == newRest);
}

TEST_CASE("a copied condition and task paste in place, as often as wanted") {
    Clipboard clipboard;
    clipboard.copyCondition(hpBelow(40.0f));
    CHECK(clipboard.what() == Clipboard::What::Condition);
    std::vector<Condition> conditions = {hpBelow(10.0f), hpBelow(20.0f)};
    REQUIRE(clipboard.pasteCondition(conditions, 1));
    REQUIRE(clipboard.pasteCondition(conditions, 3));
    REQUIRE(conditions.size() == 4);
    CHECK(conditions[0].right.number == 10.0f);
    CHECK(conditions[1].right.number == 40.0f);
    CHECK(conditions[2].right.number == 20.0f);
    CHECK(conditions[3].right.number == 40.0f);
    CHECK_FALSE(clipboard.pasteCondition(conditions, 5));
    std::vector<Task> tasks;
    CHECK_FALSE(clipboard.pasteTask(tasks, 0)); // A condition is copied, not a task.

    Task task;
    task.kind = TaskKind::UseSkill;
    task.skills = {"Wind Strike", "Ice Bolt"};
    clipboard.copyTask(task);
    CHECK(clipboard.what() == Clipboard::What::Task);
    REQUIRE(clipboard.pasteTask(tasks, 0));
    REQUIRE(tasks.size() == 1);
    CHECK(tasks[0].skills == std::vector<std::string>{"Wind Strike", "Ice Bolt"});
    CHECK_FALSE(clipboard.pasteCondition(conditions, 0)); // The task replaced the condition.
}

TEST_CASE("a copied state pastes with all under it, its transitions inside going to the copies") {
    Tree tree;
    const int combat = tree.add(tree.root(), "Combat");
    const int attack = tree.add(combat, "Attack");
    const int heal = tree.add(combat, "Heal");
    const int deep = tree.add(attack, "Deep");
    const int rest = tree.add(tree.root(), "Rest");
    tree.find(combat)->enter = {hpBelow(90.0f)};
    Task task;
    task.kind = TaskKind::SelectTarget;
    tree.find(attack)->tasks = {task};
    Transition toHeal; // Inside what is copied: goes to the copy.
    toHeal.target = heal;
    Transition toRest; // Outside: the same state.
    toRest.target = rest;
    Transition toSelf; // A repeat: the copy repeats itself.
    toSelf.target = attack;
    toSelf.when = When::TasksDone;
    tree.find(attack)->transitions = {toHeal, toRest, toSelf};
    tree.find(deep)->transitions = {toRest};

    Clipboard clipboard;
    CHECK_FALSE(clipboard.copyState(tree, tree.root())); // Root is not copied.
    CHECK_FALSE(clipboard.copyState(tree, 999));
    CHECK(clipboard.what() == Clipboard::What::Nothing);
    REQUIRE(clipboard.copyState(tree, combat));
    CHECK(clipboard.what() == Clipboard::What::State);

    const int pasted = clipboard.pasteState(tree, rest);
    REQUIRE(pasted != 0);
    CHECK(shown(tree) == std::vector<std::string>{"Root/0", "Combat/1", "Attack/2", "Deep/3", "Heal/2", "Rest/1",
                                                  "Combat 2/2", "Attack/3", "Deep/4", "Heal/3"});
    const State& copy = *tree.find(pasted);
    CHECK(copy.parent == rest);
    REQUIRE(copy.enter.size() == 1);
    CHECK(copy.enter[0].right.number == 90.0f);
    REQUIRE(copy.children.size() == 2);
    const State& attackCopy = *tree.find(copy.children[0]);
    const int healCopy = copy.children[1];
    CHECK(attackCopy.id != attack);
    REQUIRE(attackCopy.tasks.size() == 1);
    CHECK(attackCopy.tasks[0].kind == TaskKind::SelectTarget);
    REQUIRE(attackCopy.transitions.size() == 3);
    CHECK(attackCopy.transitions[0].target == healCopy);
    CHECK(attackCopy.transitions[1].target == rest);
    CHECK(attackCopy.transitions[2].target == attackCopy.id);
    CHECK(attackCopy.transitions[2].when == When::TasksDone);
    REQUIRE(attackCopy.children.size() == 1);
    CHECK(tree.find(attackCopy.children[0])->transitions.at(0).target == rest);
    // The original is as it was.
    CHECK(tree.find(attack)->transitions[0].target == heal);
    CHECK(tree.find(combat)->children == std::vector<int>{attack, heal});

    // Into itself too: the copy is what was copied, not what is there now.
    const int again = clipboard.pasteState(tree, deep);
    REQUIRE(again != 0);
    CHECK(tree.find(again)->name == "Combat 3");
    CHECK(tree.find(again)->parent == deep);
    CHECK(tree.find(again)->children.size() == 2);
    CHECK(clipboard.pasteState(tree, 999) == 0); // No such parent.
}

TEST_CASE("a copied state pastes into another profile, outside targets by their path") {
    Tree tree;
    const int combat = tree.add(tree.root(), "Combat");
    const int rest = tree.add(tree.root(), "Rest");
    const int flee = tree.add(tree.root(), "Flee");
    Transition toRest;
    toRest.target = rest;
    Transition toFlee;
    toFlee.target = flee;
    tree.find(combat)->transitions = {toRest, toFlee};
    Clipboard clipboard;
    REQUIRE(clipboard.copyState(tree, combat));
    clipboard.forgetIds(); // Another profile loaded.

    Tree other;
    const int otherRest = other.add(other.root(), "Rest");
    const int pasted = clipboard.pasteState(other, other.root());
    REQUIRE(pasted != 0);
    CHECK(other.find(pasted)->name == "Combat"); // Not taken there.
    REQUIRE(other.find(pasted)->transitions.size() == 2);
    CHECK(targetName(other, other.find(pasted)->transitions[0]) == "Rest");
    CHECK(other.find(pasted)->transitions[0].target == otherRest);
    CHECK(other.find(pasted)->transitions[1].target == 0); // No Flee there: none chosen.
}
