#include "core/StateTree.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstring>
#include <utility>

namespace observex::statetree {

namespace {

constexpr const char* kVariableNames[kVariables] = {"Number",   "HP",         "MP",       "XP",
                                                    "Target HP", "Cast",      "Cooldown", "Characters",
                                                    "Monsters", "Monsters Around", "Monster Distance",
                                                    "Drops",    "Drop Distance", "Since Kill",
                                                    "Target Distance"};
constexpr const char* kVariableWords[kVariables] = {"",         "hp",         "mp",       "xp",
                                                    "target-hp", "cast",      "cooldown", "characters",
                                                    "monsters", "monsters-around", "nearest-monster",
                                                    "drops",    "drop-distance", "since-kill",
                                                    "target-distance"}; // In the file
                                                                                      // (Monster Distance's is its
                                                                                      // old name's).
constexpr const char* kTaskNames[kTaskKinds] = {"Use Skill",   "Select Target", "Click Mouse",
                                                "Debug Print", "Focus Camera",  "Look Around",
                                                "Move To Drop", "Flee"};
constexpr const char* kTaskWords[kTaskKinds] = {"use-skill",   "select-target", "click-mouse",
                                                "debug-print", "focus-camera",  "look-around",
                                                "move-to-drop", "flee"}; // In the file.
constexpr const char* kFocusModeNames[kFocusModes] = {"Character + Target"};
constexpr const char* kFocusModeWords[kFocusModes] = {"character-target"}; // In the file.
constexpr const char* kFleeFromNames[kFleeFroms] = {"Target", "Attacker"};
constexpr const char* kFleeFromWords[kFleeFroms] = {"target", "attacker"}; // In the file.
constexpr std::string_view kParallelWord = "parallel"; // Before a Parallel task's kind in the file.
// Select Target's word in files written before it was renamed (it was Lock Mouse Onto until 2026-11-15).
constexpr std::string_view kOldSelectTargetWord = "lock-mouse";
constexpr const char* kTargetNames[kTargets] = {"Nearest Monster", "Attacker"};
constexpr const char* kTargetWords[kTargets] = {"nearest-monster", "attacker"}; // In the file.
constexpr const char* kClickButtonNames[kClickButtons] = {"Left", "Right"};
constexpr const char* kClickButtonWords[kClickButtons] = {"left", "right"}; // In the file.
constexpr char kHex[] = "0123456789abcdef";
constexpr const char* kSkillIsNames[kSkillIs] = {"Ready", "Cooling down", "Being cast"};
constexpr const char* kSkillIsWords[kSkillIs] = {"ready", "cooling", "cast"}; // In the file.
constexpr const char* kNeedNames[kNeeds] = {"Any Skill", "All Skills"};
constexpr const char* kNeedWords[kNeeds] = {"any", "all"}; // In the file.
constexpr const char* kEventNames[kEvents] = {"HP Lost", "Target Hit By Others"};
constexpr const char* kEventWords[kEvents] = {"hp-lost", "target-hit-by-others"}; // In the file.
constexpr const char* kEventSpoken[kEvents] = {"HP lost", "target hit by others"}; // In describe().
constexpr const char* kWhenNames[kWhens] = {"Any time", "Tasks done"}; // After the details' "When".
constexpr char kTasksDoneWord[] = "done"; // After a go line's target: When Tasks Done.
constexpr char kListSeparator = '|'; // Between the skills of a list in the file (a skill's name has none).
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

// A list of skills as the file has it: the names between "|".
std::string listText(const std::vector<std::string>& names) {
    std::string text;
    for (const std::string& name : names) {
        text += (text.empty() ? "" : std::string(1, kListSeparator)) + name;
    }
    return text;
}

// The names of a list written by listText (empty ones left out).
std::vector<std::string> listOf(std::string_view text) {
    std::vector<std::string> names;
    while (!text.empty()) {
        const std::size_t end = text.find(kListSeparator);
        const std::string_view name = trimmed(text.substr(0, end));
        if (!name.empty()) {
            names.emplace_back(name);
        }
        text = end == std::string_view::npos ? std::string_view() : text.substr(end + 1);
    }
    return names;
}

// "Wind Strike", "Wind Strike or Ice Bolt", "Wind Strike, Ice Bolt or Heal" (`last` "and" for all of them).
std::string spoken(const std::vector<std::string>& names, const char* last) {
    std::string text;
    for (std::size_t i = 0; i < names.size(); ++i) {
        text += (i == 0 ? "" : i + 1 == names.size() ? std::string(" ") + last + " " : std::string(", ")) + names[i];
    }
    return text;
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

const char* nameOf(Need need) {
    return kNeedNames[static_cast<int>(need)];
}

const char* nameOf(Event event) {
    return kEventNames[static_cast<int>(event)];
}

const char* nameOf(TaskKind kind) {
    return kTaskNames[static_cast<int>(kind)];
}

const char* nameOf(Target target) {
    return kTargetNames[static_cast<int>(target)];
}

const char* nameOf(ClickButton button) {
    return kClickButtonNames[static_cast<int>(button)];
}

const char* nameOf(FocusMode mode) {
    return kFocusModeNames[static_cast<int>(mode)];
}

const char* nameOf(FleeFrom from) {
    return kFleeFromNames[static_cast<int>(from)];
}

const char* nameOf(When when) {
    return kWhenNames[static_cast<int>(when)];
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

bool Tree::move(int id, int parent, int before) {
    State* state = find(id);
    State* to = find(parent);
    if (state == nullptr || to == nullptr || id == root_ || within(parent, id)) {
        return false; // Root stays first; a state never goes under itself.
    }
    if (before != 0 && std::ranges::find(to->children, before) == to->children.end()) {
        return false;
    }
    if (before == id) {
        return true; // Just before itself: where it is.
    }
    std::erase(find(state->parent)->children, id);
    state->parent = parent;
    std::vector<int>& children = to->children;
    children.insert(before == 0 ? children.end() : std::ranges::find(children, before), id);
    return true;
}

bool Tree::moveTransition(int from, std::size_t index, int to, std::size_t before) {
    State* source = find(from);
    State* target = find(to);
    if (source == nullptr || target == nullptr || index >= source->transitions.size() ||
        before > target->transitions.size()) {
        return false;
    }
    if (source == target) {
        return moveWithin(source->transitions, index, before); // A new order: they are tried in order.
    }
    Transition moved = std::move(source->transitions[index]);
    source->transitions.erase(source->transitions.begin() + static_cast<std::ptrdiff_t>(index));
    target->transitions.insert(target->transitions.begin() + static_cast<std::ptrdiff_t>(before), std::move(moved));
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
    // MP in points, from its number ("90/108"; the user asked, 2026-11-15: Use Skill sees a skill's MP cost taken).
    if (const GameGauge* mp = stats.find("Status", "MP"); mp != nullptr && mp->seen) {
        const std::string_view text = textOf(mp->text);
        const std::size_t slash = text.find('/');
        int points = 0;
        int most = 0;
        if (slash != std::string_view::npos && toInt(text.substr(0, slash), &points) &&
            toInt(text.substr(slash + 1), &most) && most > 0 && points >= 0 && points <= most) {
            variables.mpPointsKnown = true;
            variables.mpPoints = static_cast<float>(points);
            variables.mpMax = static_cast<float>(most);
        }
    }
    read(Variable::TargetHp, kTargetPiece, "HP");
    // The cast bar: how far, and the skill cast; no bar on screen is no cast (when stats.txt has one to read).
    if (const GameGauge* cast = stats.find(kCastingPiece, kCastingPiece); cast != nullptr) {
        const auto i = static_cast<std::size_t>(Variable::Cast);
        variables.value[i] = cast->seen ? std::max(cast->full, 0.0f) * 100.0f : 0.0f;
        variables.known[i] = !cast->seen || cast->full >= 0.0f;
        variables.castShown = cast->seen;
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
            const std::string_view name = textOf(gauge.name);
            const Variable variable = name == kCharactersGauge       ? Variable::Characters
                                      : name == kMonstersGauge       ? Variable::Monsters
                                      : name == kMonstersAroundGauge ? Variable::MonstersAround
                                      : name == kMonsterDistanceGauge ? Variable::MonsterDistance
                                      : name == kDropsGauge          ? Variable::Drops
                                      : name == kDropDistanceGauge   ? Variable::DropDistance
                                      : name == kSinceKillGauge      ? Variable::SinceKill
                                      : name == kTargetDistanceGauge ? Variable::TargetDistance
                                                                     : Variable::None;
            float number = 0.0f; // A count, or (Since Kill) seconds.
            const std::string_view text = textOf(gauge.text);
            if (variable != Variable::None &&
                std::from_chars(text.data(), text.data() + text.size(), number).ec == std::errc()) {
                variables.value[static_cast<std::size_t>(variable)] = number;
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

bool isReady(const Variables& variables, std::string_view name) {
    const SkillSeen* skill = variables.skill(name);
    return skill != nullptr && skill->found && skill->cooldown <= 0.0f && !skill->pressed;
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
        if (condition.is == SkillIs::Ready) {
            return isReady(variables, condition.text);
        }
        // Pressed (the flash before the cast begins) is neither: it cannot be used again yet, nor is it cooling.
        const SkillSeen* skill = variables.skill(condition.text);
        return skill != nullptr && skill->found && skill->cooldown > 0.0f;
    }
    case Kind::SkillsReady: {
        if (condition.skills.empty()) {
            return false;
        }
        const auto ready = [&](const std::string& name) { return isReady(variables, name); };
        return condition.need == Need::AllSkills ? std::ranges::all_of(condition.skills, ready)
                                                 : std::ranges::any_of(condition.skills, ready);
    }
    case Kind::Event: return variables.events[static_cast<std::size_t>(condition.event)];
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

std::string describe(const std::vector<Condition>& conditions) {
    std::string text;
    for (std::size_t i = 0; i < conditions.size(); ++i) {
        const Condition& c = conditions[i];
        if (i > 0) {
            text += c.join == Join::And ? " and " : " or ";
        }
        const bool notEqual = c.compare == Compare::NotEqual;
        switch (c.kind) {
        case Kind::Number: {
            const auto side = [](const Operand& operand) {
                return operand.variable == Variable::Cooldown ? operand.skill + " cooldown"
                       : operand.variable == Variable::None   ? operandText(operand)
                                                              : std::string(nameOf(operand.variable));
            };
            text += side(c.left) + " " + symbolOf(c.compare) + " " + side(c.right);
            break;
        }
        case Kind::HasTarget: text += notEqual ? "no target" : "a target"; break;
        case Kind::TargetName: text += std::string("target ") + (notEqual ? "not " : "") + c.text; break;
        case Kind::Skill:
            text += c.text + (c.is == SkillIs::Ready         ? " ready"
                              : c.is == SkillIs::CoolingDown ? " cooling down"
                                                             : " being cast");
            break;
        case Kind::SkillsReady:
            text += c.skills.empty()          ? std::string("no skills ready")
                    : c.skills.size() == 1     ? c.skills.front() + " ready"
                    : c.need == Need::AllSkills ? spoken(c.skills, "and") + " ready"
                                                : spoken(c.skills, "or") + " ready";
            break;
        case Kind::Event: text += kEventSpoken[static_cast<int>(c.event)]; break;
        }
    }
    return text;
}

bool Runner::activeFrom(const Tree& tree, int current, int id) {
    return id == current || (id != tree.root() && tree.within(current, id));
}

std::string statePath(const Tree& tree, int id) {
    std::vector<const std::string*> names; // Leaf first.
    for (const State* state = tree.find(id); state != nullptr && state->id != tree.root() && names.size() < 64;
         state = tree.find(state->parent)) {
        names.push_back(&state->name);
    }
    std::string path;
    for (auto name = names.rbegin(); name != names.rend(); ++name) {
        if (!path.empty()) {
            path += kPathJoin;
        }
        path += **name;
    }
    return path;
}

int stateAtPath(const Tree& tree, std::string_view path) {
    if (path.empty()) {
        return tree.root();
    }
    for (const Row& row : tree.rows()) {
        if (row.state != tree.root() && statePath(tree, row.state) == path) {
            return row.state;
        }
    }
    return 0;
}

bool Runner::canEnter(const Tree& tree, int id, const Variables& variables) {
    const State* state = tree.find(id);
    return state != nullptr && pass(state->enter, variables);
}

int Runner::landing(const Tree& tree, int id, const Variables& variables) {
    // Down through the first child whose enter conditions pass, while there is one (Root picks none: its transitions
    // choose, as before 2026-11-15).
    for (int depth = 0; id != tree.root() && depth < 64; ++depth) {
        const State* state = tree.find(id);
        const auto child = std::ranges::find_if(state->children, [&](int c) { return canEnter(tree, c, variables); });
        if (child == state->children.end()) {
            break;
        }
        id = *child;
    }
    return id;
}

bool Runner::tick(const Tree& tree, const Variables& variables) {
    const int now = current(tree);
    const bool done = task(tree) < 0; // Its tasks all done, or it has none.
    // Transitions: the Any Time ones, then (its tasks done) the When Tasks Done ones; each time the current state's,
    // then those of each state above it (Root's only while it is current itself).
    for (const When when : {When::AnyTime, When::TasksDone}) {
        if (when == When::TasksDone && !done) {
            break;
        }
        for (const State* state = tree.find(now); state != nullptr; state = tree.find(state->parent)) {
            if (state->id == tree.root() && now != tree.root()) {
                break;
            }
            for (const Transition& transition : state->transitions) {
                if (transition.when == when && tree.find(transition.target) != nullptr &&
                    pass(transition.conditions, variables) && canEnter(tree, transition.target, variables)) {
                    enter(landing(tree, transition.target, variables));
                    return true;
                }
            }
        }
    }
    // Picking again, once its tasks are done: from its parent, or from itself while it has children (none could be
    // picked when it was entered). Only when that lands elsewhere.
    const State* state = tree.find(now);
    if (now != tree.root() && done) {
        const int from = state->children.empty() ? state->parent : now;
        if (from != tree.root()) {
            const int land = landing(tree, from, variables);
            if (land != now) {
                enter(land);
                return true;
            }
        }
    }
    return false;
}

int Runner::task(const Tree& tree) const {
    // task_ counts the tasks one after another done since the state was entered; the Parallel ones are skipped.
    const State* state = tree.find(current(tree));
    if (state == nullptr) {
        return -1;
    }
    int done = 0;
    for (std::size_t i = 0; i < state->tasks.size(); ++i) {
        if (state->tasks[i].parallel) {
            continue;
        }
        if (done == task_) {
            return static_cast<int>(i);
        }
        ++done;
    }
    return -1;
}

std::vector<TaskAt> Runner::parallelTasks(const Tree& tree, int current) {
    std::vector<int> active; // The active states, the current one first.
    for (const State* state = tree.find(current); state != nullptr && active.size() < 64;
         state = tree.find(state->parent)) {
        if (activeFrom(tree, current, state->id)) {
            active.push_back(state->id);
        }
    }
    std::vector<TaskAt> tasks;
    for (auto id = active.rbegin(); id != active.rend(); ++id) { // From the top one down.
        const State& state = *tree.find(*id);
        for (std::size_t i = 0; i < state.tasks.size(); ++i) {
            if (state.tasks[i].parallel) {
                tasks.push_back({state.id, static_cast<int>(i)});
            }
        }
    }
    return tasks;
}

std::string whereText(const Tree& tree, int current, int task) {
    const State* state = tree.find(current);
    if (state == nullptr) {
        return "nowhere";
    }
    std::string text = current == tree.root() ? state->name : statePath(tree, current);
    if (task >= 0 && task < static_cast<int>(state->tasks.size())) {
        text += std::string(", running ") + nameOf(state->tasks[static_cast<std::size_t>(task)].kind);
    } else if (std::ranges::any_of(state->tasks, [](const Task& t) { return !t.parallel; })) {
        text += ", its tasks done";
    }
    return text;
}

void Runner::finishTask() {
    ++task_;
    ++taskStarts_;
}

namespace {

// One condition as its "if" line.
std::string conditionLine(const Condition& c) {
    std::string text = std::string("if ") + (c.join == Join::And ? "and " : "or ");
    const bool notEqual = c.compare == Compare::NotEqual;
    switch (c.kind) {
    case Kind::Number:
        text += operandText(c.left) + " " + kCompareWords[static_cast<int>(c.compare)] + " " + operandText(c.right);
        break;
    case Kind::TargetName: text += std::string("target-name ") + (notEqual ? "!= " : "== ") + c.text; break;
    case Kind::Skill: text += std::string("skill ") + kSkillIsWords[static_cast<int>(c.is)] + " " + c.text; break;
    case Kind::HasTarget: text += notEqual ? "has-target no" : "has-target yes"; break;
    case Kind::SkillsReady:
        text += std::string("skills-ready ") + kNeedWords[static_cast<int>(c.need)] + " " + listText(c.skills);
        break;
    case Kind::Event: text += std::string("event ") + kEventWords[static_cast<int>(c.event)]; break;
    }
    return text + "\n";
}

} // namespace

std::string writeTree(const Tree& tree) {
    std::string text = std::string(kHeader) + "\n";
    for (const Skill& skill : tree.skills()) {
        text += std::string("skill ") + (skill.active ? "active " : "utility ") +
                (skill.key.empty() ? "" : "key=" + skill.key + " ") +
                (skill.mp > 0 ? "mp=" + std::to_string(skill.mp) + " " : "") + std::to_string(skill.side) + " ";
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
            text += "task " + std::to_string(state.id) + " " +
                    (task.parallel ? std::string(kParallelWord) + " " : std::string()) +
                    kTaskWords[static_cast<int>(task.kind)];
            switch (task.kind) {
            case TaskKind::UseSkill:
                text += task.skills.empty() ? "\n" : " " + listText(task.skills) + "\n";
                break;
            case TaskKind::SelectTarget:
                text += std::string(" ") + kTargetWords[static_cast<int>(task.target)] + "\n";
                break;
            case TaskKind::ClickMouse:
                text += std::string(" ") + kClickButtonWords[static_cast<int>(task.button)] + "\n";
                break;
            case TaskKind::DebugPrint:
                text += "\n";
                break;
            case TaskKind::LookAround:
                text += task.degrees != kLookAroundDegrees ? " " + std::to_string(task.degrees) + "\n" : "\n";
                break;
            case TaskKind::FocusCamera:
                text += std::string(" ") + kFocusModeWords[static_cast<int>(task.focus)] + "\n";
                break;
            case TaskKind::MoveToDrop:
                text += task.reach != kDropReach ? " " + std::to_string(task.reach) + "\n" : "\n";
                break;
            case TaskKind::Flee:
                text += std::string(" ") + kFleeFromWords[static_cast<int>(task.from)] + "\n";
                break;
            }
        }
    }
    for (const Row& row : rows) {
        const State& state = *tree.find(row.state);
        if (!state.enter.empty()) {
            text += "enter " + std::to_string(state.id) + "\n";
            for (const Condition& c : state.enter) {
                text += conditionLine(c);
            }
        }
    }
    for (const Row& row : rows) {
        const State& state = *tree.find(row.state);
        for (const Transition& transition : state.transitions) {
            text += "go " + std::to_string(state.id) + " " + std::to_string(transition.target) +
                    (transition.when == When::TasksDone ? std::string(" ") + kTasksDoneWord : std::string()) + "\n";
            for (const Condition& c : transition.conditions) {
                text += conditionLine(c);
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
    int lastFrom = 0;          // The state of the last go or enter line,
    bool lastEnter = false;    // which was an enter line: the if lines after it are its enter conditions.
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
            // Active or utility (before 2026-11-16 the words were cooldown and no-cooldown: the same two).
            const std::string_view group = nextWord(line, &at);
            const bool active = group == "active" || group == "cooldown";
            const bool utility = group == "utility" || group == "no-cooldown";
            std::string_view sideWord = nextWord(line, &at);
            std::string_view key;
            int mp = 0;
            bool mpRead = true;
            for (;;) {
                if (sideWord.starts_with("key=")) { // Its shortcut (since 2026-11-15).
                    key = sideWord.substr(4);
                } else if (sideWord.starts_with("mp=")) { // Its MP cost (since 2026-11-15).
                    mpRead = toInt(sideWord.substr(3), &mp) && mp >= 0;
                } else {
                    break;
                }
                sideWord = nextWord(line, &at);
            }
            int side = 0;
            const bool sized = toInt(sideWord, &side);
            const std::string_view pixels = nextWord(line, &at);
            const std::string_view name = trimmed(line.substr(at));
            const std::size_t bytes = static_cast<std::size_t>(std::max(side, 0)) * static_cast<std::size_t>(side) * 3;
            if ((!active && !utility) || !sized || !mpRead || side < 0 || side > 64 || name.empty() ||
                (pixels != "-" && pixels.size() != bytes * 2) || (pixels == "-" && side != 0)) {
                return fail("a skill is: skill active|utility [key=KEY] [mp=MP] SIDE PIXELS NAME");
            }
            Skill skill{std::string(name), active, side, {}, std::string(key), mp};
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
            std::string_view word = nextWord(line, &at);
            const bool parallel = word == kParallelWord;
            if (parallel) {
                word = nextWord(line, &at);
            }
            const auto* known = word == kOldSelectTargetWord
                                    ? std::begin(kTaskWords) + static_cast<int>(TaskKind::SelectTarget)
                                    : std::find(std::begin(kTaskWords), std::end(kTaskWords), word);
            if (known != std::end(kTaskWords)) { // Else an unknown kind (a name typed in before kinds): left out.
                const auto taskKind = static_cast<TaskKind>(known - std::begin(kTaskWords));
                const std::string_view rest = trimmed(line.substr(at));
                Task task;
                task.kind = taskKind;
                task.parallel = parallel && canRunParallel(taskKind);
                if (taskKind == TaskKind::UseSkill) {
                    task.skills = listOf(rest);
                } else if (taskKind == TaskKind::SelectTarget) {
                    const auto* found = std::find(std::begin(kTargetWords), std::end(kTargetWords), rest);
                    if (found != std::end(kTargetWords)) {
                        task.target = static_cast<Target>(found - std::begin(kTargetWords));
                    }
                } else if (taskKind == TaskKind::ClickMouse) {
                    const auto* found = std::find(std::begin(kClickButtonWords), std::end(kClickButtonWords), rest);
                    if (found != std::end(kClickButtonWords)) {
                        task.button = static_cast<ClickButton>(found - std::begin(kClickButtonWords));
                    }
                } else if (taskKind == TaskKind::FocusCamera) {
                    const auto* found = std::find(std::begin(kFocusModeWords), std::end(kFocusModeWords), rest);
                    if (found != std::end(kFocusModeWords)) {
                        task.focus = static_cast<FocusMode>(found - std::begin(kFocusModeWords));
                    }
                } else if (taskKind == TaskKind::LookAround) {
                    if (int degrees = 0; toInt(rest, &degrees)) { // Nothing (or not a number): all the way round.
                        task.degrees = std::clamp(degrees, kLookMinDegrees, kLookMaxDegrees);
                    }
                } else if (taskKind == TaskKind::MoveToDrop) {
                    if (int reach = 0; toInt(rest, &reach)) { // Nothing (or not a number): kDropReach.
                        task.reach = std::clamp(reach, kDropMinReach, kDropMaxReach);
                    }
                } else if (taskKind == TaskKind::Flee) {
                    const auto* found = std::find(std::begin(kFleeFromWords), std::end(kFleeFromWords), rest);
                    if (found != std::end(kFleeFromWords)) {
                        task.from = static_cast<FleeFrom>(found - std::begin(kFleeFromWords));
                    }
                }
                tree.find(idOf(of))->tasks.push_back(std::move(task));
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
            const std::string_view when = nextWord(line, &at); // Nothing: Any Time (every file before 2026-11-15).
            if (!when.empty() && when != kTasksDoneWord) {
                return fail("a transition is: go FROM TO [done]");
            }
            lastFrom = idOf(from);
            lastEnter = false;
            tree.find(lastFrom)->transitions.push_back(
                {to != 0 ? idOf(to) : 0, {}, when.empty() ? When::AnyTime : When::TasksDone});
        } else if (kind == "enter") {
            int of = 0;
            if (!toInt(nextWord(line, &at), &of) || idOf(of) == 0) {
                return fail("enter conditions of a state not there");
            }
            lastFrom = idOf(of);
            lastEnter = true;
        } else if (kind == "if") {
            if (lastFrom == 0) {
                return fail("a condition before any transition or enter line");
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
            } else if (left == "skills-ready") {
                const std::string_view need = nextWord(line, &at);
                const auto* word = std::find(std::begin(kNeedWords), std::end(kNeedWords), need);
                if (word == std::end(kNeedWords)) {
                    return fail("a skills condition is: if and|or skills-ready any|all SKILL|SKILL...");
                }
                condition.kind = Kind::SkillsReady;
                condition.need = static_cast<Need>(word - std::begin(kNeedWords));
                condition.skills = listOf(line.substr(at));
            } else if (left == "has-target") {
                const std::string_view answer = nextWord(line, &at);
                if (answer != "yes" && answer != "no") {
                    return fail("a target condition is: if and|or has-target yes|no");
                }
                condition.kind = Kind::HasTarget;
                condition.compare = answer == "yes" ? Compare::Equal : Compare::NotEqual;
            } else if (left == "event") {
                const std::string_view event = nextWord(line, &at);
                const auto* word = std::find(std::begin(kEventWords), std::end(kEventWords), event);
                if (word == std::end(kEventWords)) {
                    return fail("an event condition is: if and|or event hp-lost|target-hit-by-others");
                }
                condition.kind = Kind::Event;
                condition.compare = Compare::Equal;
                condition.event = static_cast<Event>(word - std::begin(kEventWords));
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
            State& state = *tree.find(lastFrom);
            (lastEnter ? state.enter : state.transitions.back().conditions).push_back(std::move(condition));
        } else {
            return fail("not a skill, state, task, enter, go or if line");
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
