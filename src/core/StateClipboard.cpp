#include "core/StateClipboard.h"

#include <algorithm>
#include <iterator>

namespace observex::statetree {

bool Clipboard::copyState(const Tree& tree, int id) {
    if (tree.find(id) == nullptr || id == tree.root()) {
        return false;
    }
    states_.clear();
    targets_.clear();
    for (const Row& row : tree.rows()) { // Depth first: every parent before its children, children in order.
        if (tree.within(row.state, id)) {
            states_.push_back(*tree.find(row.state));
        }
    }
    for (const State& state : states_) {
        for (const Transition& transition : state.transitions) {
            keepTarget(tree, transition.target);
        }
    }
    idsKnown_ = true;
    what_ = What::State;
    return true;
}

void Clipboard::copyTransition(const Tree& tree, const Transition& transition) {
    targets_.clear();
    transition_ = transition;
    keepTarget(tree, transition.target);
    idsKnown_ = true;
    what_ = What::Transition;
}

void Clipboard::copyCondition(const Condition& condition) {
    condition_ = condition;
    what_ = What::Condition;
}

void Clipboard::copyTask(const Task& task) {
    task_ = task;
    what_ = What::Task;
}

int Clipboard::pasteState(Tree& tree, int parent) const {
    if (what_ != What::State || states_.empty() || tree.find(parent) == nullptr) {
        return 0;
    }
    // Where its transitions to states not copied with it go, found before the copies are added (so none of them is
    // taken for one of those).
    std::vector<std::vector<int>> targets;
    for (const State& copied : states_) {
        std::vector<int>& to = targets.emplace_back();
        for (const Transition& transition : copied.transitions) {
            to.push_back(targetIn(tree, transition.target));
        }
    }
    std::vector<std::pair<int, int>> ids; // Each copied state's id, and its copy's.
    const auto copyOf = [&](int copied) {
        const auto it = std::ranges::find(ids, copied, &std::pair<int, int>::first);
        return it != ids.end() ? it->second : 0;
    };
    for (std::size_t i = 0; i < states_.size(); ++i) {
        const State& copied = states_[i];
        const int id = i == 0 ? tree.add(parent, tree.unusedName(copied.name))
                              : tree.add(copyOf(copied.parent), copied.name);
        if (id == 0) {
            break; // Not reached: each parent comes before its children.
        }
        ids.emplace_back(copied.id, id);
        State& pasted = *tree.find(id);
        pasted.enter = copied.enter;
        pasted.tasks = copied.tasks;
    }
    for (std::size_t i = 0; i < ids.size(); ++i) {
        State& pasted = *tree.find(ids[i].second);
        const State& copied = states_[i];
        for (std::size_t t = 0; t < copied.transitions.size(); ++t) {
            Transition transition = copied.transitions[t];
            const int inside = copyOf(transition.target);
            transition.target = inside != 0 ? inside : targets[i][t];
            pasted.transitions.push_back(std::move(transition));
        }
    }
    return ids.empty() ? 0 : ids.front().second;
}

bool Clipboard::pasteTransition(Tree& tree, int state, std::size_t before) const {
    State* into = tree.find(state);
    if (what_ != What::Transition || into == nullptr || before > into->transitions.size()) {
        return false;
    }
    Transition transition = transition_;
    transition.target = targetIn(tree, transition.target);
    into->transitions.insert(into->transitions.begin() + static_cast<std::ptrdiff_t>(before), std::move(transition));
    return true;
}

bool Clipboard::pasteCondition(std::vector<Condition>& conditions, std::size_t before) const {
    if (what_ != What::Condition || before > conditions.size()) {
        return false;
    }
    conditions.insert(conditions.begin() + static_cast<std::ptrdiff_t>(before), condition_);
    return true;
}

bool Clipboard::pasteTask(std::vector<Task>& tasks, std::size_t before) const {
    if (what_ != What::Task || before > tasks.size()) {
        return false;
    }
    tasks.insert(tasks.begin() + static_cast<std::ptrdiff_t>(before), task_);
    return true;
}

int Clipboard::targetIn(const Tree& tree, int id) const {
    if (id == 0) {
        return 0;
    }
    if (idsKnown_ && tree.find(id) != nullptr) {
        return id; // The same state (ids are never reused in a tree), renamed or moved since too.
    }
    const auto kept = std::ranges::find(targets_, id, &std::pair<int, std::string>::first);
    // Another profile, the same read again, or the state deleted since: by its path; 0 if none is there.
    return kept != targets_.end() ? stateAtPath(tree, kept->second) : 0;
}

void Clipboard::keepTarget(const Tree& tree, int id) {
    if (id == 0 || tree.find(id) == nullptr ||
        std::ranges::find(targets_, id, &std::pair<int, std::string>::first) != targets_.end()) {
        return;
    }
    targets_.emplace_back(id, statePath(tree, id));
}

} // namespace observex::statetree
