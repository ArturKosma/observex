#include "core/StateTree.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstring>
#include <utility>

namespace observex::statetree {

namespace {

constexpr const char* kVariableNames[kVariables] = {"Number", "HP",       "MP",         "XP",      "Target HP",
                                                    "Cast",   "Cooldown", "Characters", "Monsters"};
constexpr const char* kVariableWords[kVariables] = {"",     "hp",       "mp",         "xp",      "target-hp",
                                                    "cast", "cooldown", "characters", "monsters"}; // In the file.
constexpr const char* kTaskNames[kTaskKinds] = {"Use Skill"};
constexpr const char* kTaskWords[kTaskKinds] = {"use-skill"}; // In the file.
constexpr char kHex[] = "0123456789abcdef";
constexpr const char* kSkillIsNames[kSkillIs] = {"Ready", "Cooling down", "Being cast"};
constexpr const char* kSkillIsWords[kSkillIs] = {"ready", "cooling", "cast"}; // In the file.
// ASCII: observex's font has no U+2264, U+2265 or U+2260 (they showed as "?"), and UE writes them so too.
constexpr const char* kSymbols[kCompares] = {"<", "<=", "=", "!=", ">=", ">"};
constexpr const char* kCompareWords[kCompares] = {"<", "<=", "==", "!=", ">=", ">"}; // In the file.
constexpr std::string_view kHeader = "observex-brain 1";

float valueOf(const Operand& operand, const Variables& variables, bool* known) {
    if (operand.variable == Variable::None) {
        return operand.number;
    }
    if (operand.variable == Variable::Cooldown) {
        const SkillSeen* skill = variables.skill(operand.skill);
        *known = *known && skill != nullptr && skill->found;
        return skill != nullptr ? skill->cooldown : 0.0f;
    }
    const auto i = static_cast<std::size_t>(operand.variable);
    *known = *known && variables.known[i];
    return variables.value[i];
}

bool sameLetter(char x, char y) {
    return std::tolower(static_cast<unsigned char>(x)) == std::tolower(static_cast<unsigned char>(y));
}

// Whether `a` and `b` are the same text, ignoring case (ASCII).
bool sameText(std::string_view a, std::string_view b) {
    return std::ranges::equal(a, b, sameLetter);
}

// Whether `text` has `part` in it, ignoring case (ASCII); any text has the empty one.
bool hasText(std::string_view text, std::string_view part) {
    return part.empty() || !std::ranges::search(text, part, sameLetter).empty();
}

// A skill's name in the file: its spaces as "_", so it stays one word.
std::string skillWord(std::string_view name) {
    std::string word(name);
    std::ranges::replace(word, ' ', '_');
    return word;
}

template <std::size_t N> std::string_view textOf(const std::array<char, N>& text) {
    return {text.data(), strnlen(text.data(), N)};
}

std::string operandText(const Operand& operand) {
    if (operand.variable == Variable::Cooldown) {
        return "cooldown:" + skillWord(operand.skill);
    }
    if (operand.variable != Variable::None) {
        return kVariableWords[static_cast<int>(operand.variable)];
    }
    char text[32];
    const auto result = std::to_chars(text, text + sizeof(text), operand.number); // Shortest that reads back the same.
    return std::string(text, result.ptr);
}

std::string_view trimmed(std::string_view text) {
    const std::size_t first = text.find_first_not_of(" \t");
    if (first == std::string_view::npos) {
        return {};
    }
    return text.substr(first, text.find_last_not_of(" \t") - first + 1);
}

// The next word of `line` from *at on, spaces skipped.
std::string_view nextWord(std::string_view line, std::size_t* at) {
    while (*at < line.size() && line[*at] == ' ') {
        ++*at;
    }
    const std::size_t start = *at;
    while (*at < line.size() && line[*at] != ' ') {
        ++*at;
    }
    return line.substr(start, *at - start);
}

bool toInt(std::string_view text, int* value) {
    const auto result = std::from_chars(text.data(), text.data() + text.size(), *value);
    return !text.empty() && result.ec == std::errc() && result.ptr == text.data() + text.size();
}

bool toOperand(std::string_view text, Operand* operand) {
    if (text.starts_with("cooldown:") && text.size() > 9) {
        std::string skill(text.substr(9));
        std::ranges::replace(skill, '_', ' ');
        *operand = {Variable::Cooldown, 0.0f, std::move(skill)};
        return true;
    }
    for (int v = 1; v < kVariables; ++v) {
        if (text == kVariableWords[v] && static_cast<Variable>(v) != Variable::Cooldown) {
            *operand = {static_cast<Variable>(v), 0.0f, {}};
            return true;
        }
    }
    float number = 0.0f;
    const auto result = std::from_chars(text.data(), text.data() + text.size(), number);
    if (text.empty() || result.ec != std::errc() || result.ptr != text.data() + text.size()) {
        return false;
    }
    *operand = {Variable::None, number, {}};
    return true;
}

} // namespace

const char* nameOf(Variable variable) {
    return kVariableNames[static_cast<int>(variable)];
}

const char* nameOf(SkillIs is) {
    return kSkillIsNames[static_cast<int>(is)];
}

const char* nameOf(TaskKind kind) {
    return kTaskNames[static_cast<int>(kind)];
}

const Skill* Tree::skill(std::string_view name) const {
    const auto it = std::ranges::find_if(skills_, [&](const Skill& skill) { return sameText(skill.name, name); });
    return it != skills_.end() ? &*it : nullptr;
}

const char* symbolOf(Compare compare) {
    return kSymbols[static_cast<int>(compare)];
}

Tree::Tree() {
    root_ = nextId_++;
    states_.push_back({.id = root_, .name = "Root"});
}

State* Tree::find(int id) {
    const auto it = std::ranges::find(states_, id, &State::id);
    return it != states_.end() && id != 0 ? &*it : nullptr;
}

const State* Tree::find(int id) const {
    return const_cast<Tree*>(this)->find(id);
}

int Tree::add(int parent, std::string name) {
    if (find(parent) == nullptr) {
        return 0;
    }
    const int id = nextId_++;
    states_.push_back({.id = id, .name = std::move(name), .parent = parent});
    find(parent)->children.push_back(id); // After the push: it may have moved the states.
    return id;
}

bool Tree::remove(int id) {
    const State* state = find(id);
    if (state == nullptr || id == root_) {
        return false;
    }
    std::vector<int> gone = {id};
    for (std::size_t i = 0; i < gone.size(); ++i) { // Everything under it, as the list grows.
        const std::vector<int> children = find(gone[i])->children;
        gone.insert(gone.end(), children.begin(), children.end());
    }
    const auto isGone = [&](int s) { return std::ranges::find(gone, s) != gone.end(); };
    std::erase(find(state->parent)->children, id);
    std::erase_if(states_, [&](const State& s) { return isGone(s.id); });
    for (State& s : states_) {
        std::erase_if(s.transitions, [&](const Transition& t) { return isGone(t.target); });
    }
    return true;
}

std::vector<Row> Tree::rows() const {
    std::vector<Row> rows;
    addRows(root_, 0, rows);
    return rows;
}

void Tree::addRows(int id, int depth, std::vector<Row>& rows) const {
    rows.push_back({id, depth});
    for (const int child : find(id)->children) {
        addRows(child, depth + 1, rows);
    }
}

std::string Tree::unusedName(std::string_view base) const {
    std::string name(base);
    for (int n = 2; std::ranges::find(states_, name, &State::name) != states_.end(); ++n) {
        name = std::string(base) + " " + std::to_string(n);
    }
    return name;
}

bool Tree::within(int id, int ancestor) const {
    for (const State* state = find(id); state != nullptr; state = find(state->parent)) {
        if (state->id == ancestor) {
            return true;
        }
    }
    return false;
}

Variables variablesOf(const GameStats& stats) {
    Variables variables;
    const auto read = [&](Variable variable, std::string_view piece, std::string_view name) {
        const GameGauge* gauge = stats.find(piece, name);
        if (gauge != nullptr && gauge->seen && gauge->full >= 0.0f) {
            const auto i = static_cast<std::size_t>(variable);
            variables.value[i] = gauge->full * 100.0f;
            variables.known[i] = true;
        }
    };
    read(Variable::Hp, "Status", "HP");
    read(Variable::Mp, "Status", "MP");
    read(Variable::Xp, "Status", "XP");
    read(Variable::TargetHp, kTargetPiece, "HP");
    // The cast bar: how far, and the skill cast; no bar on screen is no cast (when stats.txt has one to read).
    if (const GameGauge* cast = stats.find(kCastingPiece, kCastingPiece); cast != nullptr) {
        const auto i = static_cast<std::size_t>(Variable::Cast);
        variables.value[i] = cast->seen ? std::max(cast->full, 0.0f) * 100.0f : 0.0f;
        variables.known[i] = !cast->seen || cast->full >= 0.0f;
        variables.casting = cast->seen ? std::string(textOf(cast->text)) : std::string();
        variables.castingKnown = !cast->seen || !variables.casting.empty();
    }
    // The target, as the stats reading keeps it (the user asked, 2026-11-13: cached, there at once): whether its window
    // shows, and the name last read while it stayed up.
    variables.hasTarget = stats.hasTarget;
    variables.target = textOf(stats.targetName);
    variables.targetKnown = stats.hasTarget && !variables.target.empty();
    for (int g = 0; g < stats.count; ++g) {
        const GameGauge& gauge = stats.gauges[static_cast<std::size_t>(g)];
        if (textOf(gauge.piece) == kSkillsPiece) {
            const bool found = gauge.seen && gauge.full >= 0.0f;
            variables.skills.push_back(
                {std::string(textOf(gauge.name)), found, found ? gauge.full * 100.0f : 0.0f, found && gauge.pressed});
        } else if (textOf(gauge.piece) == kNearbyPiece && gauge.seen) {
            // The model's counts (the app puts them in).
            const Variable variable = textOf(gauge.name) == kCharactersGauge ? Variable::Characters
                                      : textOf(gauge.name) == kMonstersGauge ? Variable::Monsters
                                                                             : Variable::None;
            int count = 0;
            const std::string_view text = textOf(gauge.text);
            if (variable != Variable::None &&
                std::from_chars(text.data(), text.data() + text.size(), count).ec == std::errc()) {
                variables.value[static_cast<std::size_t>(variable)] = static_cast<float>(count);
                variables.known[static_cast<std::size_t>(variable)] = true;
            }
        }
    }
    return variables;
}

std::optional<float> valueOf(const Operand& operand, const Variables& variables) {
    bool known = true;
    const float value = valueOf(operand, variables, &known);
    return known ? std::optional<float>(value) : std::nullopt;
}

const SkillSeen* Variables::skill(std::string_view name) const {
    const auto it = std::ranges::find_if(skills, [&](const SkillSeen& seen) { return sameText(seen.name, name); });
    return it != skills.end() ? &*it : nullptr;
}

bool holds(const Condition& condition, const Variables& variables) {
    switch (condition.kind) {
    case Kind::Number: break;
    case Kind::HasTarget: return variables.hasTarget == (condition.compare != Compare::NotEqual);
    case Kind::TargetName: {
        if (!variables.targetKnown) {
            return false;
        }
        const bool has = hasText(variables.target, condition.text);
        return condition.compare == Compare::NotEqual ? !has : has;
    }
    case Kind::Skill: {
        if (condition.is == SkillIs::Cast) {
            return variables.castingKnown && !variables.casting.empty() && sameText(variables.casting, condition.text);
        }
        const SkillSeen* skill = variables.skill(condition.text);
        if (skill == nullptr || !skill->found) {
            return false;
        }
        // Pressed (the flash before the cast begins) is neither: it cannot be used again yet, nor is it cooling.
        return condition.is == SkillIs::Ready ? skill->cooldown <= 0.0f && !skill->pressed : skill->cooldown > 0.0f;
    }
    }
    bool known = true;
    const float left = valueOf(condition.left, variables, &known);
    const float right = valueOf(condition.right, variables, &known);
    if (!known) {
        return false;
    }
    switch (condition.compare) {
    case Compare::Less: return left < right;
    case Compare::LessOrEqual: return left <= right;
    case Compare::Equal: return left == right;
    case Compare::NotEqual: return left != right;
    case Compare::GreaterOrEqual: return left >= right;
    case Compare::Greater: return left > right;
    }
    return false;
}

bool pass(const std::vector<Condition>& conditions, const Variables& variables) {
    if (conditions.empty()) {
        return true;
    }
    bool result = holds(conditions.front(), variables);
    for (std::size_t i = 1; i < conditions.size(); ++i) {
        const bool next = holds(conditions[i], variables);
        result = conditions[i].join == Join::And ? result && next : result || next;
    }
    return result;
}

bool Runner::tick(const Tree& tree, const Variables& variables) {
    for (const Transition& transition : tree.find(current(tree))->transitions) {
        if (tree.find(transition.target) != nullptr && pass(transition.conditions, variables)) {
            current_ = transition.target;
            return true;
        }
    }
    return false;
}

std::string writeTree(const Tree& tree) {
    std::string text = std::string(kHeader) + "\n";
    for (const Skill& skill : tree.skills()) {
        text += std::string("skill ") + (skill.cooldown ? "cooldown " : "no-cooldown ") + std::to_string(skill.side) +
                " ";
        if (skill.bgr.empty()) {
            text += "-";
        }
        for (const std::uint8_t byte : skill.bgr) {
            text += kHex[byte >> 4];
            text += kHex[byte & 15];
        }
        text += " " + skill.name + "\n";
    }
    const std::vector<Row> rows = tree.rows();
    for (const Row& row : rows) {
        const State& state = *tree.find(row.state);
        text += "state " + std::to_string(state.id) + " " + std::to_string(state.parent) + " " + state.name + "\n";
    }
    for (const Row& row : rows) {
        const State& state = *tree.find(row.state);
        for (const Task& task : state.tasks) {
            text += "task " + std::to_string(state.id) + " " + kTaskWords[static_cast<int>(task.kind)];
            text += task.skill.empty() ? "\n" : " " + task.skill + "\n";
        }
    }
    for (const Row& row : rows) {
        const State& state = *tree.find(row.state);
        for (const Transition& transition : state.transitions) {
            text += "go " + std::to_string(state.id) + " " + std::to_string(transition.target) + "\n";
            for (const Condition& c : transition.conditions) {
                text += std::string("if ") + (c.join == Join::And ? "and " : "or ");
                const bool notEqual = c.compare == Compare::NotEqual;
                switch (c.kind) {
                case Kind::Number:
                    text += operandText(c.left) + " " + kCompareWords[static_cast<int>(c.compare)] + " " +
                            operandText(c.right);
                    break;
                case Kind::TargetName: text += std::string("target-name ") + (notEqual ? "!= " : "== ") + c.text; break;
                case Kind::Skill: text += std::string("skill ") + kSkillIsWords[static_cast<int>(c.is)] + " " + c.text; break;
                case Kind::HasTarget: text += notEqual ? "has-target no" : "has-target yes"; break;
                }
                text += "\n";
            }
        }
    }
    return text;
}

std::optional<Tree> readTree(std::string_view text, std::string* error) {
    std::size_t number = 0;
    const auto fail = [&](const char* why) -> std::optional<Tree> {
        if (error != nullptr) {
            *error = number > 0 ? "Line " + std::to_string(number) + ": " + why : why;
        }
        return std::nullopt;
    };
    Tree tree;
    std::vector<std::pair<int, int>> ids; // The file's id, the tree's.
    const auto idOf = [&](int fileId) {
        const auto it = std::ranges::find(ids, fileId, &std::pair<int, int>::first);
        return it != ids.end() ? it->second : 0;
    };
    bool header = false;
    int lastFrom = 0; // The state of the last go line.
    while (!text.empty()) {
        const std::size_t end = text.find('\n');
        std::string_view line = text.substr(0, end);
        text = end == std::string_view::npos ? std::string_view() : text.substr(end + 1);
        ++number;
        line = trimmed(line.substr(0, line.find('\r')));
        if (line.empty() || line.front() == '#') {
            continue;
        }
        if (!header) {
            if (line != kHeader) {
                return fail("not an observex brain");
            }
            header = true;
            continue;
        }
        std::size_t at = 0;
        const std::string_view kind = nextWord(line, &at);
        if (kind == "state") {
            int id = 0;
            int parent = 0;
            if (!toInt(nextWord(line, &at), &id) || !toInt(nextWord(line, &at), &parent) || id == 0) {
                return fail("a state needs its id and its parent's");
            }
            const std::string_view name = trimmed(line.substr(at));
            if (name.empty()) {
                return fail("a state needs a name");
            }
            if (idOf(id) != 0) {
                return fail("two states with one id");
            }
            if (parent == 0) {
                if (!ids.empty()) {
                    return fail("only Root, the first state, has no parent");
                }
                tree.find(tree.root())->name = std::string(name);
                ids.emplace_back(id, tree.root());
            } else {
                if (idOf(parent) == 0) {
                    return fail("a state before its parent");
                }
                ids.emplace_back(id, tree.add(idOf(parent), std::string(name)));
            }
        } else if (kind == "skill") {
            const std::string_view has = nextWord(line, &at);
            int side = 0;
            const bool sized = toInt(nextWord(line, &at), &side);
            const std::string_view pixels = nextWord(line, &at);
            const std::string_view name = trimmed(line.substr(at));
            const std::size_t bytes = static_cast<std::size_t>(std::max(side, 0)) * static_cast<std::size_t>(side) * 3;
            if ((has != "cooldown" && has != "no-cooldown") || !sized || side < 0 || side > 64 || name.empty() ||
                (pixels != "-" && pixels.size() != bytes * 2) || (pixels == "-" && side != 0)) {
                return fail("a skill is: skill cooldown|no-cooldown SIDE PIXELS NAME");
            }
            Skill skill{std::string(name), has == "cooldown", side, {}};
            skill.bgr.reserve(bytes);
            for (std::size_t i = 0; i < bytes; ++i) {
                const auto digit = [](char c) {
                    return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
                };
                const int high = digit(pixels[i * 2]);
                const int low = digit(pixels[i * 2 + 1]);
                if (high < 0 || low < 0) {
                    return fail("a skill's pixels are hex digits");
                }
                skill.bgr.push_back(static_cast<std::uint8_t>(high * 16 + low));
            }
            if (tree.skill(skill.name) == nullptr) { // Two of one name: the first.
                tree.skills().push_back(std::move(skill));
            }
        } else if (kind == "task") {
            int of = 0;
            if (!toInt(nextWord(line, &at), &of) || idOf(of) == 0) {
                return fail("a task of a state not there");
            }
            const std::string_view word = nextWord(line, &at);
            const auto* known = std::find(std::begin(kTaskWords), std::end(kTaskWords), word);
            if (known != std::end(kTaskWords)) { // Else a task typed in before there were kinds: left out.
                tree.find(idOf(of))->tasks.push_back(
                    {static_cast<TaskKind>(known - std::begin(kTaskWords)), std::string(trimmed(line.substr(at)))});
            }
        } else if (kind == "go") {
            int from = 0;
            int to = 0;
            if (!toInt(nextWord(line, &at), &from) || !toInt(nextWord(line, &at), &to)) {
                return fail("a transition needs its state and its target");
            }
            if (idOf(from) == 0 || (to != 0 && idOf(to) == 0)) {
                return fail("a transition of a state not there, or to one");
            }
            lastFrom = idOf(from);
            tree.find(lastFrom)->transitions.push_back({to != 0 ? idOf(to) : 0, {}});
        } else if (kind == "if") {
            if (lastFrom == 0) {
                return fail("a condition before any transition");
            }
            Condition condition;
            const std::string_view join = nextWord(line, &at);
            if (join != "and" && join != "or") {
                return fail("a condition starts: if and|or");
            }
            condition.join = join == "and" ? Join::And : Join::Or;
            const std::string_view left = nextWord(line, &at);
            if (left == "target-name") {
                const std::string_view compare = nextWord(line, &at);
                if (compare != "==" && compare != "!=") {
                    return fail("a target name condition is: if and|or target-name ==|!= TEXT");
                }
                condition.kind = Kind::TargetName;
                condition.compare = compare == "==" ? Compare::Equal : Compare::NotEqual;
                condition.text = trimmed(line.substr(at));
            } else if (left == "skill") {
                const std::string_view is = nextWord(line, &at);
                const auto* word = std::find(std::begin(kSkillIsWords), std::end(kSkillIsWords), is);
                const std::string_view skill = trimmed(line.substr(at));
                if (word == std::end(kSkillIsWords) || skill.empty()) {
                    return fail("a skill condition is: if and|or skill ready|cooling|cast SKILL");
                }
                condition.kind = Kind::Skill;
                condition.is = static_cast<SkillIs>(word - std::begin(kSkillIsWords));
                condition.text = skill;
            } else if (left == "has-target") {
                const std::string_view answer = nextWord(line, &at);
                if (answer != "yes" && answer != "no") {
                    return fail("a target condition is: if and|or has-target yes|no");
                }
                condition.kind = Kind::HasTarget;
                condition.compare = answer == "yes" ? Compare::Equal : Compare::NotEqual;
            } else {
                const std::string_view compare = nextWord(line, &at);
                const std::string_view right = nextWord(line, &at);
                const auto* word = std::find(std::begin(kCompareWords), std::end(kCompareWords), compare);
                if (!toOperand(left, &condition.left) || word == std::end(kCompareWords) ||
                    !toOperand(right, &condition.right)) {
                    return fail("a condition is: if and|or LEFT < RIGHT (hp, mp, xp, target-hp, cast, characters, "
                                "monsters, cooldown:SKILL or a number)");
                }
                condition.compare = static_cast<Compare>(word - std::begin(kCompareWords));
            }
            tree.find(lastFrom)->transitions.back().conditions.push_back(std::move(condition));
        } else {
            return fail("not a skill, state, task, go or if line");
        }
    }
    if (ids.empty()) {
        number = 0;
        return fail(header ? "no states in it" : "not an observex brain");
    }
    return tree;
}

void History::reset(const Tree& tree) {
    kept_ = tree;
    text_ = writeTree(tree);
    undo_.clear();
    redo_.clear();
}

bool History::keep(const Tree& tree) {
    std::string text = writeTree(tree); // Every part of a tree is in its text: the same text, the same tree.
    if (text == text_) {
        return false;
    }
    undo_.push_back(std::move(kept_));
    if (undo_.size() > kUndoSteps) {
        undo_.erase(undo_.begin());
    }
    redo_.clear();
    kept_ = tree;
    text_ = std::move(text);
    return true;
}

bool History::undo(Tree* tree) {
    if (undo_.empty()) {
        return false;
    }
    redo_.push_back(std::move(kept_));
    kept_ = std::move(undo_.back());
    undo_.pop_back();
    text_ = writeTree(kept_);
    *tree = kept_;
    return true;
}

bool History::redo(Tree* tree) {
    if (redo_.empty()) {
        return false;
    }
    undo_.push_back(std::move(kept_));
    kept_ = std::move(redo_.back());
    redo_.pop_back();
    text_ = writeTree(kept_);
    *tree = kept_;
    return true;
}

} // namespace observex::statetree
