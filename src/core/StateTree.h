#pragma once

#include "core/GameStats.h"

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace observex::statetree {

// The bot's brain as a tree of states, in the spirit of Unreal Engine 5's StateTree (the user asked, 2026-11-12), kept
// small, to be added to a piece at a time (the user: "i will keep adding them"): Root, the states under it, each with
// a name and transitions (conditions on the game's numbers, and the state to go to), run by Runner on every reading.

// What a number condition reads: the game's numbers (core/GameStats), each in percent (0 to 100), but the counts.
enum class Variable : std::uint8_t {
    None, // Not a variable: the operand's own number.
    Hp,
    Mp,
    Xp,
    TargetHp,
    Cast,     // How far the cast bar is; 0 while nothing is cast.
    Cooldown, // The share of the operand's skill's cooldown still to go; 0: ready (the user asked, 2026-11-12: the
              // skills reachable from conditions).
    Characters, // How many characters the model sees, you left out (a count; the user asked, 2026-11-14).
    Monsters,   // How many of them are monsters (a count).
};
inline constexpr int kVariables = 9;
const char* nameOf(Variable variable); // "HP", ..., "Cast", "Cooldown"; "Number" for None.
// Whether `variable` is a count (shown as a number), not a percent.
inline bool isCount(Variable variable) { return variable == Variable::Characters || variable == Variable::Monsters; }

struct Operand {
    Variable variable = Variable::None;
    float number = 0.0f; // When variable is None.
    std::string skill;   // When variable is Cooldown: the skill's name, as added in the Stats tile.
};

enum class Compare : std::uint8_t { Less, LessOrEqual, Equal, NotEqual, GreaterOrEqual, Greater };
inline constexpr int kCompares = 6;
const char* symbolOf(Compare compare); // "<", "<=", "=", "!=", ">=", ">".

// How a condition joins the result of those before it, as UE's AND / OR (left to right; the first one's is unused).
enum class Join : std::uint8_t { And, Or };

// The kinds of condition, as UE's condition types, picked when one is added:
enum class Kind : std::uint8_t {
    Number,     // UE's Compare Float: left compare right.
    TargetName, // The target's name compare (= or !=) text: = holds when the name has the text in it, ignoring case
                // (the user asked, 2026-11-12: "if target is Ashen Wolf, simply typing Ashen should already say True").
    Skill,      // A skill is ready, cooling down or being cast.
    HasTarget,  // Something is targeted (its window shows), or (compare !=) nothing is (the user asked, 2026-11-12).
};
enum class SkillIs : std::uint8_t { Ready, CoolingDown, Cast };
inline constexpr int kSkillIs = 3;
const char* nameOf(SkillIs is); // "Ready", "Cooling down", "Being cast".

// One condition of a transition. Each fails while what it reads is not known (a bar unseen, no target, a skill not
// found on screen).
struct Condition {
    Join join = Join::And;
    Operand left = {Variable::Hp, 0.0f, {}};
    Compare compare = Compare::Less;
    Operand right = {Variable::None, 50.0f, {}};
    Kind kind = Kind::Number;
    std::string text;            // TargetName: what to find in the name; Skill: the skill's name.
    SkillIs is = SkillIs::Ready; // Skill.
};

// Leaving a state for `target` (0: none chosen yet, never goes), when its conditions pass (none: always).
struct Transition {
    int target = 0;
    std::vector<Condition> conditions;
};

// What a state does while it is current, as UE's tasks (the user asked, 2026-11-13, for Add Task; 2026-11-14: kinds
// picked from a list, not typed): for now Use Skill, one of the profile's skills (what it does comes with the input).
enum class TaskKind : std::uint8_t { UseSkill };
inline constexpr int kTaskKinds = 1;
const char* nameOf(TaskKind kind); // "Use Skill".
struct Task {
    TaskKind kind = TaskKind::UseSkill;
    std::string skill; // UseSkill: the skill's name (empty: none chosen yet).
};

// A skill the user added (the Skills tab; the user asked, 2026-11-14: "a pocket with all skills", kept in the profile
// like the tree): its name, whether it has a cooldown to read, and its miniature when added (ready), side x side,
// 3 bytes a pixel (blue, green, red), shown as its icon and turned grey for the stats reader.
struct Skill {
    std::string name;
    bool cooldown = true;
    int side = 0;
    std::vector<std::uint8_t> bgr;

    bool operator==(const Skill&) const = default;
};

struct State {
    int id = 0; // Never reused; 0 is no state.
    std::string name;
    int parent = 0; // 0: Root's own.
    std::vector<int> children; // In order.
    std::vector<Transition> transitions; // Tried in order.
    std::vector<Task> tasks;             // In order.
};

// A state as listed: depth-first, children in order, Root first at depth 0.
struct Row {
    int state = 0;
    int depth = 0;
};

class Tree {
public:
    Tree(); // Just Root.

    int root() const { return root_; }
    State* find(int id);
    const State* find(int id) const;

    // `name` as `parent`'s last child; its id, or 0 if there is no such parent.
    int add(int parent, std::string name);
    // `id` and every state under it, and the transitions that went to them. Root stays: false.
    bool remove(int id);
    std::vector<Row> rows() const;
    // `base`, or "base 2", "base 3", ... if a state already has it.
    std::string unusedName(std::string_view base) const;
    // Whether `id` is `ancestor` or a state under it.
    bool within(int id, int ancestor) const;

    // The profile's skills, in the order added.
    std::vector<Skill>& skills() { return skills_; }
    const std::vector<Skill>& skills() const { return skills_; }
    // The skill named `name` (ignoring case), or null.
    const Skill* skill(std::string_view name) const;

private:
    void addRows(int id, int depth, std::vector<Row>& rows) const;

    std::vector<State> states_;
    std::vector<Skill> skills_;
    int root_ = 0;
    int nextId_ = 1;
};

// An added skill as read: found on screen or not, and how much of its cooldown is left.
struct SkillSeen {
    std::string name;
    bool found = false;     // Its miniature was found on screen.
    float cooldown = 0.0f;  // Percent of its cooldown still to go; 0: ready.
    bool pressed = false;   // Its miniature shows the flash after its key was pressed: not ready, not cooling yet.
};

// What one reading says; what is not known (a bar not seen, under a window) fails every condition that reads it.
struct Variables {
    std::array<float, kVariables> value{}; // By Variable (Cooldown's are in skills).
    std::array<bool, kVariables> known{};
    bool hasTarget = false;   // The target's window shows (its bar or its name read).
    bool targetKnown = false; // The target's name was read,
    std::string target;       // this one.
    bool castingKnown = false; // Which skill is cast is known (none while no cast bar shows),
    std::string casting;       // this one; empty: none.
    std::vector<SkillSeen> skills; // The profile's skills (the Skills tab), in its order.

    const SkillSeen* skill(std::string_view name) const; // Null if no such skill was read.
};
Variables variablesOf(const GameStats& stats);

// An operand's value on a reading; nothing if it reads what is not known.
std::optional<float> valueOf(const Operand& operand, const Variables& variables);
// Whether `condition` holds with `variables`; false if it reads a variable not known.
bool holds(const Condition& condition, const Variables& variables);
// Whether `conditions` pass, joined left to right; true when there are none.
bool pass(const std::vector<Condition>& conditions, const Variables& variables);

// Runs a tree, kept simple for now (the user asked, 2026-11-13: "highlight the current state, initially Root, and write
// the simple state machine that already checks conditions"): one current state, Root at first; on each reading only
// its transitions are tried, in order, and the first whose conditions pass makes its target the current state. (UE's
// StateTree also enters first children down to a leaf and tries the transitions of every active state from the leaf
// up; CLAUDE.md's State tree paragraph keeps how, for when the tree needs it.)
class Runner {
public:
    // The current state: Root until a transition goes, and again after reset() or once its state is removed.
    int current(const Tree& tree) const { return tree.find(current_) != nullptr ? current_ : tree.root(); }
    void reset() { current_ = 0; }
    // A tick on a new reading. Returns whether a transition went (to itself too).
    bool tick(const Tree& tree, const Variables& variables);

private:
    int current_ = 0; // 0: Root.
};

// A tree saved as text (the files in Settings' brains folder; the user asked, 2026-11-12, for observex's own
// extension), one line each:
//   observex-brain 1
//   skill cooldown|no-cooldown SIDE PIXELS NAME   a skill, in order: PIXELS its miniature in hex, 6 digits a pixel
//                                 (blue, green, red), rows top to bottom; NAME the rest of the line
//   state ID PARENT NAME          Root first (PARENT 0), every parent before its children, children in order
//   task ID use-skill SKILL       a task of state ID, in order; SKILL the rest of the line (may be empty). A task line
//                                 of another kind (before 2026-11-14 tasks were names typed in) is left out.
//   go FROM TO                    a transition of FROM, in order
//   if JOIN LEFT COMPARE RIGHT    a condition of the go line before it: JOIN and / or; LEFT and RIGHT hp, mp, xp,
//                                 target-hp, cast, characters, monsters, cooldown:SKILL (a space in SKILL written "_")
//                                 or a number; COMPARE < <= == != >= >
//   if JOIN target-name ==|!= TEXT                  TEXT the rest of the line (may be empty)
//   if JOIN skill ready|cooling|cast SKILL          SKILL the rest of the line
//   if JOIN has-target yes|no
inline constexpr char kTreeExtension[] = ".brain";
std::string writeTree(const Tree& tree);
// A tree written by writeTree (ids may change); nothing, and why in *error, if `text` is not one.
std::optional<Tree> readTree(std::string_view text, std::string* error);

// Undo and redo for a tree (the user asked, 2026-11-13: Ctrl+Z for all operations "and all future operations"): it
// keeps whole trees, so whatever changes a tree, now or added later, is undone alike, ids and all.
class History {
public:
    // Starts again from `tree` (loaded, or new): nothing to undo or redo.
    void reset(const Tree& tree);
    // `tree` as it is now: if it is not the one kept last, it is kept, the one before becomes a step to undo (the
    // oldest dropped past kUndoSteps) and what was undone can no longer be redone. Whether it was.
    bool keep(const Tree& tree);
    // The tree before the last step, or (redo) after the last one undone, into *tree; false if there is none.
    bool undo(Tree* tree);
    bool redo(Tree* tree);
    // The tree kept last, as writeTree writes it (what its file holds).
    const std::string& text() const { return text_; }

    static constexpr std::size_t kUndoSteps = 200;

private:
    Tree kept_;
    std::string text_ = writeTree(Tree());
    std::vector<Tree> undo_;
    std::vector<Tree> redo_;
};

} // namespace observex::statetree
