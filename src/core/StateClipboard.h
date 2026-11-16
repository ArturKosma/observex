#pragma once

#include "core/StateTree.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace observex::statetree {

// The State tile's Copy and Paste (the user asked, 2026-11-16: "right click on a transition or condition or any other
// thing and press copy, and then ... rmb on proper place and press paste, so that i can copy transition from one state
// to another"): one thing at a time, a state with all under it, a transition, a condition or a task. It keeps its own
// copy, so it pastes the same however the tree changes after, as often as wanted, also into another profile.
//
// A copied transition keeps the state it goes to by its id and by its path (statePath): pasted into the tree it was
// copied from, it goes to the state with that id (renamed or moved since, too), else to the state at that path (another
// profile, or the same one read from its file again: ids change then), else to none ("Choose a state"). A copied
// state's transitions to states copied with it go to their copies.
class Clipboard {
public:
    enum class What : std::uint8_t { Nothing, State, Transition, Condition, Task };
    What what() const { return what_; }

    // The tree was read again (another profile loaded, or the same one): the copied ids are not its states' any more,
    // so the states copied transitions go to are found by their paths only.
    void forgetIds() { idsKnown_ = false; }

    // `id` with every state under it, their names, enter conditions, transitions and tasks; false (nothing copied) for
    // Root or no such state.
    bool copyState(const Tree& tree, int id);
    void copyTransition(const Tree& tree, const Transition& transition);
    void copyCondition(const Condition& condition);
    void copyTask(const Task& task);

    // The copied state, with all under it, as `parent`'s last child (each a new id; its own name made unused in the
    // tree, as a new state's: "Fight 2"); its id, or 0 if nothing was pasted (no state copied, no such parent).
    int pasteState(Tree& tree, int parent) const;
    // The copied transition into `state`'s transitions just before the one at `before` (their count: as the last).
    // False, and nothing pasted, for no transition copied, no such state or a `before` past the end.
    bool pasteTransition(Tree& tree, int state, std::size_t before) const;
    // The copied condition or task likewise, into `conditions` (a transition's or a state's enter conditions) or
    // `tasks`.
    bool pasteCondition(std::vector<Condition>& conditions, std::size_t before) const;
    bool pasteTask(std::vector<Task>& tasks, std::size_t before) const;

private:
    // The state a copied transition went to (`id`, as copied) in `tree`, as said above; 0: none.
    int targetIn(const Tree& tree, int id) const;
    // The path of `id` in `tree` kept, for targetIn (once each; 0: none).
    void keepTarget(const Tree& tree, int id);

    What what_ = What::Nothing;
    std::vector<State> states_; // State: it first, then all under it, depth first, ids and parents as copied.
    Transition transition_;
    Condition condition_;
    Task task_;
    std::vector<std::pair<int, std::string>> targets_; // Each target's id as copied, and its statePath then.
    bool idsKnown_ = false; // The ids in targets_ are the tree's (no forgetIds() since the copy).
};

} // namespace observex::statetree
