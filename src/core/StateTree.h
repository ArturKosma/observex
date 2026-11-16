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
    // The monster memory (core/MonsterMemory; the user asked, 2026-11-16: turn round first, remember every monster and
    // its distance, then go for the nearest): how many monsters it remembers round the own character (a count; those
    // Select Target gave up on left out), and how far the nearest of them is: Monster Distance (game units, as the
    // live tags; not known while it remembers none, so "Monster Distance < 600" says some monster it knows, on the
    // screen or off it, is nearer than 600; the user asked for it by that name, 2026-11-16; "Nearest Monster" until
    // then, its file word still nearest-monster).
    MonstersAround,
    MonsterDistance,
    // The drops (the user asked, 2026-11-16: loot them before seeking the next monster): how many the drop finder
    // sees (core/LootWatch's spot; the count held kLostMs after they vanish, so one missed look or the own character
    // standing on them does not end the loot; drops given up on left out), and how far the nearest of those seen now
    // is (game units, as the live tags; not known while none shows or the camera is not known).
    Drops,
    DropDistance,
    // Seconds since the target last died (core/TargetWatch's death: its HP bar read empty on 2 readings, as the Kills
    // counter), timed by the screen image of each reading; before the first kill since observex started, as long ago
    // as can be (infinity: "Since Kill > 0.75" holds, "< 0.5" does not; until 2026-11-16 not known, so neither held and
    // the user's Root never left for Seek Monster) (the user asked, 2026-11-16: an explicit condition to wait a moment
    // for a kill's drops, as the drop finder sees them 0.05-0.25 s after the death, after the brain had already gone to
    // Rest).
    SinceKill,
    // How far the target is from the own character (game units, as the live tags): its box on the screen now as
    // core/TargetWatch knows it, its feet on the ground; not known with no target, its box not known or off the
    // screen, or the camera not known (the user's clip 2026-11-16_14-04-25: Wind Strike kept being cast at an Imp 640
    // away, "The distance is too far", while another hit the own character; with it, "HP Lost and Target Distance >
    // 300" can go to Select Target's Attacker).
    TargetDistance,
};
inline constexpr int kVariables = 15;
const char* nameOf(Variable variable); // "HP", ..., "Cast", "Cooldown"; "Number" for None.
// Whether `variable` is shown as a plain number (a count, or a distance in game units), not a percent.
inline bool isCount(Variable variable) {
    return variable == Variable::Characters || variable == Variable::Monsters || variable == Variable::MonstersAround ||
           variable == Variable::MonsterDistance || variable == Variable::Drops || variable == Variable::DropDistance ||
           variable == Variable::TargetDistance;
}
// Whether `variable` is in seconds (shown "0.4 s"), neither a count nor a percent.
inline bool isSeconds(Variable variable) { return variable == Variable::SinceKill; }

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
    SkillsReady, // Is Skill Ready: of a list of skills, any one is ready, or all are (the user asked, 2026-11-15).
    Event,       // An event came (see Event below; the user asked, 2026-11-16).
};
enum class SkillIs : std::uint8_t { Ready, CoolingDown, Cast };
inline constexpr int kSkillIs = 3;
const char* nameOf(SkillIs is); // "Ready", "Cooling down", "Being cast".
// Is Skill Ready's need: at least one of its skills ready, or every one.
enum class Need : std::uint8_t { AnySkill, AllSkills };
inline constexpr int kNeeds = 2;
const char* nameOf(Need need); // "Any Skill", "All Skills".

// What happened in the game, as UE's StateTree events (the user asked, 2026-11-16: "event-based conditions", to leave
// Rest once HP was lost): the app notes each on the reading it was seen on, and the brain's next step has it, for that
// one step only. Taken by a transition or not, it is gone then, so one that came while in another state never waits
// to fire later. An Event condition holds on that step; on an Any time transition it is UE's On Event trigger.
enum class Event : std::uint8_t {
    HpLost, // The own HP fell (core/HpWatch: its number read lower twice in a row; a lower most left out).
    // The target's HP bar fell while the own character did not hit it: no MP taken and no cast ended near it
    // (core/TargetHitWatch; the user asked, 2026-11-16: someone nearer started hitting the monster the own character
    // runs to, so leave it to them, no kill steal).
    TargetHitByOthers,
};
inline constexpr int kEvents = 2;
const char* nameOf(Event event); // "HP Lost", "Target Hit By Others".

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
    std::vector<std::string> skills; // SkillsReady: the skills' names (none: never holds),
    Need need = Need::AnySkill;      // and whether any or all must be ready.
    Event event = Event::HpLost;     // Event: the one it waits for.
};

// When a transition is tried, as UE's transition trigger (the user asked, 2026-11-15: "transition at any time OR after
// all tasks are done"): Any Time on every reading, also while a task runs (UE's On Tick: for what must break in, no
// target, low HP); When Tasks Done only once the current state's last task is done, or at once if it has none (UE's
// On State Completed; a state's tasks run one after another here, so UE's per-state All / Any is not needed). A
// transition into its own state When Tasks Done repeats it: its tasks again from the first.
enum class When : std::uint8_t { AnyTime, TasksDone };
inline constexpr int kWhens = 2;
const char* nameOf(When when); // "Any time", "Tasks done".

// Leaving a state for `target` (0: none chosen yet, never goes), when its conditions pass (none: always).
struct Transition {
    int target = 0;
    std::vector<Condition> conditions;
    When when = When::AnyTime;
};

// What a state does while it is current, as UE's tasks (the user asked, 2026-11-13, for Add Task; 2026-11-14: kinds
// picked from a list, not typed; 2026-11-15: generic tasks that drive the controller, then "tasks go one after another"
// and "each task has a definition of done before next task is activated"). They run one at a time, in order, while the
// app acts (app/App): the first when the state is entered (again too, by a transition to itself), each next
// once the one before is done; after the last nothing runs until the state is entered again. Done:
// - Select Target (Lock Mouse Onto until 2026-11-15): the target is chosen, the cursor moved onto it quickly and
//   smoothly, and once the game shows it hovered (its name plate over it, grey orbs) clicked; done once the game shows
//   it is the new target (its plate's orbs coloured, and the target window up with its name); also done, without a
//   target, once there is nothing to select a moment (core/TargetSelect's kNoneWait).
// - Use Skill: of its skills, top to bottom, the first ready one is pressed (the user asked, 2026-11-15); done once the
//   game shows it was used: its cast ended, or its MP cost (the Skills tab) was taken (core/SkillUse says how, with
//   its fallbacks).
// - Click Mouse: done once clicked.
// - Debug Print (the user asked, 2026-11-15, to see a parallel task work): writes where the brain is into the Log,
//   "Debug Print: Combat > Attack Spell, running Use Skill"; done once written. Parallel, it writes that every 0.3 s.
// - Focus Camera (the user asked, 2026-11-16: "rotate with rmb + move gently to make the target box stay within a
//   treshold in 1/4 or 3/4 of the screen", meant to be Parallel in Combat, "whenever lmb mouse is free"): in its mode
//   (FocusMode) Character + Target, short right-button turns of the camera keep the target a quarter of the width
//   from the own character, the pitch within 20-30 degrees (core/CameraFocus); done once the target is in place (or
//   there is none). Parallel, it goes on the whole time, waiting while Select Target or Click Mouse runs.
// - Look Around (the user asked, 2026-11-16: "before we decide to select a monster, we first rotate around, remember
//   all monsters and their distances"): the camera turned once all the way round by the right button (core/LookAround;
//   Task::degrees, 360 unless set otherwise), the monster memory (core/MonsterMemory) forgotten first and filled by
//   every look on the way; done once turned (or, so it never stays stuck, given up on with a note). Left mid-turn (an
//   Any time transition), it lets go of the right button at once.
// - Move To Drop (the user asked, 2026-11-16: "order our character to move on top of it, and then spam the Pick Up
//   skill"): the nearest drop the drop finder sees (core/DropPick); done at once when it lies within Task::reach game
//   units of the own character, or none shows; else the cursor glides onto it and left-clicks (the game walks there;
//   a click on an item picks it up too), the ground just short of it when a character's box covers it; done once it is
//   within reach or gone. Fallbacks: no nearer kRetry after a click, clicked again; kClicks clicks or kGiveUp, given up
//   on (left out of Drops for a while) and done.
// - Flee (the user asked, 2026-11-16: "a task Flee, with parameter, for now just Target ... using depth decides where
//   to click, and keeps moving away"): from what Task::from says (FleeFrom), the ground clicked away from it, again and
//   again (core/Flee: its feet on the ground, the ways over the depth's terrain, off the interface and every box);
//   done once there is nothing to flee from (Target: no target, or it died; Attacker: none seen and no HP lost for
//   kAttackerHoldSeconds). It holds the mouse, so Focus Camera waits while it runs.
// A Parallel task (Task::parallel; the user asked, 2026-11-15: "a parallel task", as UE's tasks of an active state)
// is not one of them: it runs the whole time its state is active (current, or above the current one: Combat's while
// its child Attack Spell runs; Root's only while Root is current), alongside them, starting when its state becomes
// active and stopping when it is left; it is never done. Only kinds that do not clash with the tasks one after
// another can be (canRunParallel): Debug Print, and Focus Camera, which lets go of the mouse while one of them wants
// it; Use Skill, Select Target and Click Mouse press the keyboard and mouse the one-after-another tasks use.
enum class TaskKind : std::uint8_t {
    UseSkill,
    SelectTarget,
    ClickMouse,
    DebugPrint,
    FocusCamera,
    LookAround,
    MoveToDrop,
    Flee,
};
inline constexpr int kTaskKinds = 8;
// "Use Skill", "Select Target", "Click Mouse", "Debug Print", "Focus Camera", "Look Around", "Move To Drop", "Flee".
const char* nameOf(TaskKind kind);
// Whether a task of `kind` can be Parallel.
inline bool canRunParallel(TaskKind kind) { return kind == TaskKind::DebugPrint || kind == TaskKind::FocusCamera; }
// How often a Parallel Debug Print writes.
inline constexpr double kDebugPrintSeconds = 0.3;

// What Select Target picks: Nearest Monster is the nearest by its distance (the user asked, 2026-11-16), of those on
// the screen and those the monster memory keeps off it (the camera turned to it first); one with no distance (far
// off, or the camera not known yet) after all that have one, the biggest first. Attacker (the user's clip
// 2026-11-16_14-04-25: an Imp hit the own character from beside it while the brain went for one 640 away) is the
// monster hitting the own character, as core/Attacker takes it: the nearest on the screen now within kAttackerReach
// (300 game units), at least kAttackerKindSure a monster, clicked beside the own character's box; none: done without
// a target, as Nearest Monster with nothing to select. Meant for "HP Lost" transitions.
enum class Target : std::uint8_t { NearestMonster, Attacker };
inline constexpr int kTargets = 2;
const char* nameOf(Target target); // "Nearest Monster", "Attacker".

enum class ClickButton : std::uint8_t { Left, Right };
inline constexpr int kClickButtons = 2;
const char* nameOf(ClickButton button); // "Left", "Right".

// What Focus Camera keeps on the screen (the user asked, 2026-11-16, for a chooseable mode): Character + Target, the own
// character in the middle (the camera circles it) and the target at 1/4 or 3/4 of the width.
enum class FocusMode : std::uint8_t { CharacterAndTarget };
inline constexpr int kFocusModes = 1;
const char* nameOf(FocusMode mode); // "Character + Target".

// What Flee runs from (the user asked, 2026-11-16: "with parameter, for now just Target"): the target, its box on the
// screen (the target watch's, or the one Select Target clicked) or, off it, where it was last seen; or the Attacker
// (as Select Target's: the monster nearest the own character within kAttackerReach), while one was seen or HP was lost
// within kAttackerHoldSeconds.
enum class FleeFrom : std::uint8_t { Target, Attacker };
inline constexpr int kFleeFroms = 2;
const char* nameOf(FleeFrom from); // "Target", "Attacker".
inline constexpr double kAttackerHoldSeconds = 1.5;

// How far Look Around turns the camera: all the way round unless set otherwise (a part turn, e.g. to look back).
inline constexpr int kLookAroundDegrees = 360;
inline constexpr int kLookMinDegrees = 10;
inline constexpr int kLookMaxDegrees = 720;

// How near Move To Drop brings the own character to a drop (game units; the own character is 47 tall, a kill's drops
// lie within 70 of the corpse): a guess to tune live, as the game's Pick Up reach is not known.
inline constexpr int kDropReach = 80;
inline constexpr int kDropMinReach = 20;
inline constexpr int kDropMaxReach = 400;

struct Task {
    TaskKind kind = TaskKind::UseSkill;
    std::vector<std::string> skills;        // UseSkill: the skills' names, tried top to bottom (none: none chosen yet).
    Target target = Target::NearestMonster; // SelectTarget: what it selects.
    ClickButton button = ClickButton::Left; // ClickMouse: which button is clicked.
    FocusMode focus = FocusMode::CharacterAndTarget; // FocusCamera: what it keeps on the screen.
    int degrees = kLookAroundDegrees; // LookAround: how far it turns (kLookMinDegrees to kLookMaxDegrees).
    int reach = kDropReach;           // MoveToDrop: near enough, game units (kDropMinReach to kDropMaxReach).
    FleeFrom from = FleeFrom::Target; // Flee: what it runs from.
    bool parallel = false; // Runs alongside the others the whole time its state is active (see TaskKind).
};

// A task where it is in the tree: its state's id and its index in that state's tasks.
struct TaskAt {
    int state = 0;
    int task = 0;

    bool operator==(const TaskAt&) const = default;
};

// A skill the user added (the Skills tab; the user asked, 2026-11-14: "a pocket with all skills", kept in the profile
// like the tree): its name, whether it is an active or a utility skill, its miniature (ready) on the hotbar slot it is
// in, side x side, 3 bytes a pixel (blue, green, red), shown as its icon and turned grey for the stats reader, and its
// shortcut: the key written on that slot (the user asked, 2026-11-15: for the Pico to press later). Both follow the
// skill when it is moved to another slot (app/App's followSkills).
struct Skill {
    std::string name;
    // The Skills tab's two columns (the user asked, 2026-11-16, in place of Has Cooldown): an active skill (Wind
    // Strike) has a cooldown to read and Use Skill is done once its MP was taken; a utility skill (Sit, Pick Up) has
    // none, and Use Skill is done once its key was pressed, held a moment and let go.
    bool active = true;
    int side = 0;
    std::vector<std::uint8_t> bgr;
    // As read off the slot (heuristics/KeyLabels), e.g. "F2", "A+K1"; a '?' for each letter not known yet; "-" for a
    // slot with no key written; empty while not read yet.
    std::string key;
    // The MP it costs (typed in the Skills tab; the user asked, 2026-11-15: Use Skill is done once it was taken); 0:
    // not set.
    int mp = 0;

    bool operator==(const Skill&) const = default;
};

struct State {
    int id = 0; // Never reused; 0 is no state.
    std::string name;
    int parent = 0; // 0: Root's own.
    std::vector<int> children; // In order.
    // Its enter conditions (UE's; the user asked, 2026-11-15): it is entered only while they pass (none: always). Of a
    // state's children the first whose pass is the one entered (see Runner).
    std::vector<Condition> enter;
    std::vector<Transition> transitions; // Tried in order.
    std::vector<Task> tasks;             // In order.
};

// `items`' item at `index` moved to just before the one at `before` (counted before the move; their count: as the
// last), as the tile's drag and drop puts a state's transitions, tasks and enter conditions (the user asked,
// 2026-11-16: tasks and enter conditions dragged as transitions are). False, and nothing moved, for an `index` or a
// `before` past the end.
template <class T>
bool moveWithin(std::vector<T>& items, std::size_t index, std::size_t before) {
    if (index >= items.size() || before > items.size()) {
        return false;
    }
    if (before == index || before == index + 1) {
        return true; // Just before or after itself: where it is.
    }
    T moved = std::move(items[index]);
    items.erase(items.begin() + static_cast<std::ptrdiff_t>(index));
    if (before > index) {
        --before; // It was counted before it went.
    }
    items.insert(items.begin() + static_cast<std::ptrdiff_t>(before), std::move(moved));
    return true;
}

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
    // `id`, with every state under it, as `parent`'s child just before `before` (0: as its last child), as the tile's
    // drag and drop puts it (the user asked, 2026-11-15). False, and nothing moved, for Root, a `parent` that is `id` or
    // under it, or a `before` that is not one of `parent`'s children.
    bool move(int id, int parent, int before);
    // `from`'s transition at `index`, with its target, When and conditions, into `to`'s transitions just before the
    // one at `before` (counted before the move; their count: as the last), as the tile's drag and drop puts it (the
    // user asked, 2026-11-15: transitions dragged as states are). `to` may be `from` (a new order: transitions are
    // tried in order) or another state. False, and nothing moved, for no such state or transition, or a `before` past
    // the end.
    bool moveTransition(int from, std::size_t index, int to, std::size_t before);
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
    bool castShown = false;    // The cast bar shows (something is being cast).
    bool castingKnown = false; // Which skill is cast is known (none while no cast bar shows),
    std::string casting;       // this one; empty: none.
    std::vector<SkillSeen> skills; // The profile's skills (the Skills tab), in its order.
    // MP in points, from the number on the Status MP bar ("90/108"): known only while that number was read.
    bool mpPointsKnown = false;
    float mpPoints = 0.0f;
    float mpMax = 0.0f;
    // The events that came since the brain's last step (by Event; the app puts them in, variablesOf() leaves them
    // all false): they hold for this one step.
    std::array<bool, kEvents> events{};

    const SkillSeen* skill(std::string_view name) const; // Null if no such skill was read.
};
Variables variablesOf(const GameStats& stats);
// Whether the skill named `name` can be used now: found on screen, its cooldown 0 and not just pressed.
bool isReady(const Variables& variables, std::string_view name);

// An operand's value on a reading; nothing if it reads what is not known.
std::optional<float> valueOf(const Operand& operand, const Variables& variables);
// Whether `condition` holds with `variables`; false if it reads a variable not known.
bool holds(const Condition& condition, const Variables& variables);
// Whether `conditions` pass, joined left to right; true when there are none.
bool pass(const std::vector<Condition>& conditions, const Variables& variables);
// `conditions` in a few words, e.g. "Wind Strike ready and HP < 50"; empty when there are none.
std::string describe(const std::vector<Condition>& conditions);

// Runs a tree (the user asked, 2026-11-13: "highlight the current state, initially Root, and write the simple state
// machine that already checks conditions"; 2026-11-15: enter conditions, children picked by them, as UE's "Try Select
// Children In Order"): one current state, Root at first. On each reading, one step at most:
// - Any Time transitions: the current state's, in order, then its parent's, and so on up, Root's left out (Root is the
//   hub: its transitions go only while it is current, and it never picks a child itself). The first whose conditions
//   pass and whose target's enter conditions pass goes.
// - When Tasks Done transitions, only if none went and the current state's tasks are all done (or it has none): the
//   same way, the current state's first, then up. As they come after every Any Time one, a parent's Any Time
//   transition (no target, low HP) always wins over a child's repeat.
// - Entering a state (Root aside) goes on down to its first child, in order, whose enter conditions pass, and so on;
//   with none, the state itself is current. So a parent's children are its choices by priority, and its transitions
//   count while any of them runs.
// - Picking again: once the current state's tasks are all done, its parent (it itself while it has children, as none
//   could be picked) picks again, as when entered, on every reading; it moves only when that lands on another state (so
//   a skill is not pressed again while its state stays picked: Sit twice stands up). Not under Root.
// Events (Variables::events) are read by Event conditions like any other condition, on the step they came.
// (UE's StateTree also has failure, priorities and backtracking; CLAUDE.md's State tree paragraph keeps how, for when
// the tree needs it.)
//
// It also keeps which of the current state's tasks runs (they run one after another, see TaskKind): the first when a
// state is entered (by a transition to itself too), the next on finishTask(). Parallel tasks are not among them: they
// are skipped (a state whose tasks are all Parallel is done at once), and parallelTasks() lists them.
class Runner {
public:
    // The current state: Root until a transition goes, and again after reset() or once its state is removed.
    int current(const Tree& tree) const { return tree.find(current_) != nullptr ? current_ : tree.root(); }
    // Whether `id` is active: the current state or a state above it whose transitions count (Root only while current).
    bool active(const Tree& tree, int id) const { return activeFrom(tree, current(tree), id); }
    // The same with `current` the current state (a clip's recorded one).
    static bool activeFrom(const Tree& tree, int current, int id);
    void reset() {
        current_ = 0;
        task_ = 0;
        ++taskStarts_;
    }
    // A step on a new reading. Returns whether it entered a state (itself again too).
    bool tick(const Tree& tree, const Variables& variables);
    // Whether `id`'s enter conditions pass now, and the state entering it lands in (see above).
    static bool canEnter(const Tree& tree, int id, const Variables& variables);
    static int landing(const Tree& tree, int id, const Variables& variables);

    // The current state's task that runs (its index in State::tasks, Parallel ones skipped), or -1 once all are done
    // (or it has none).
    int task(const Tree& tree) const;
    // The Parallel tasks that run while `current` is the current state: those of every active state (activeFrom),
    // from the top one down, each state's in order.
    static std::vector<TaskAt> parallelTasks(const Tree& tree, int current);
    // The task that runs is done: the next one runs.
    void finishTask();
    // One more each time a task starts (the first on entering a state, or the next after finishTask()), so whoever
    // runs it knows to start it afresh, the same task index again included.
    std::uint64_t taskStarts() const { return taskStarts_; }

private:
    void enter(int id) {
        current_ = id;
        task_ = 0; // Entered (again, if it is the same state): its tasks from the first.
        ++taskStarts_;
    }

    int current_ = 0; // 0: Root.
    int task_ = 0;
    std::uint64_t taskStarts_ = 0;
};

// The brain as the State tile shows it, for a clip (the user asked, 2026-11-15: a clip's review showed no active state
// or task): whether it was active (inactive: nothing green), its current state by name, as a tree's ids change when it
// is read again, and which of its tasks ran.
struct BrainShown {
    bool acting = false;
    std::string state; // statePath of the current state ("Combat > Strike"); empty: Root.
    int task = -1;     // Its task that ran (its index in State::tasks); -1: all done, or none.

    bool operator==(const BrainShown&) const = default;
};
inline constexpr char kPathJoin[] = " > ";
// The names of `id` and the states above it, Root left out, from the top, kPathJoin between; empty for Root.
std::string statePath(const Tree& tree, int id);
// The state whose statePath is `path` (Root for an empty one); 0 if none is (renamed, or another profile).
int stateAtPath(const Tree& tree, std::string_view path);
// Where the brain is, as Debug Print writes it: `current`'s statePath ("Root" for Root), then its task that runs
// (`task`, an index in its tasks; -1: none), ", running Use Skill", or ", its tasks done" once a state that has tasks
// one after another ran them all.
std::string whereText(const Tree& tree, int current, int task);

// A tree saved as text (the files in Settings' brains folder; the user asked, 2026-11-12, for observex's own
// extension), one line each:
//   observex-brain 1
//   skill active|utility [key=KEY] [mp=MP] SIDE PIXELS NAME   a skill, in order: KEY its shortcut (left out
//                                 while not read); MP its MP cost (left out while 0); PIXELS its miniature in hex, 6
//                                 digits a pixel (blue, green, red), rows top to bottom; NAME the rest of the line
//                                 (files before 2026-11-16 say cooldown for active, no-cooldown for utility)
//   state ID PARENT NAME          Root first (PARENT 0), every parent before its children, children in order
//   task ID use-skill SKILLS      a task of state ID, in order; SKILLS the rest of the line, names between "|" (may be
//                                 empty)
//   task ID select-target TARGET  TARGET = nearest-monster ("lock-mouse", its word before 2026-11-15, is read too)
//   task ID click-mouse BUTTON    BUTTON = left | right
//   task ID debug-print
//   task ID focus-camera MODE     MODE = character-target
//   task ID look-around [DEGREES] DEGREES how far it turns (left out: 360; written only when not 360)
//   task ID move-to-drop [REACH]  REACH near enough, game units (left out: kDropReach; written only when not that)
//   task ID flee FROM             FROM = target
//   task ID parallel KIND ...     a Parallel task: "parallel" before its kind (only kinds that canRunParallel; on
//                                 another it is read as not Parallel)
//                                 (a task line of an unknown kind, e.g. a name typed in before 2026-11-14, is left out)
//   enter ID                      the if lines after it are state ID's enter conditions
//   go FROM TO [done]             a transition of FROM, in order; the if lines after it are its conditions; "done": When
//                                 Tasks Done (without it Any Time, as every transition before 2026-11-15)
//   if JOIN LEFT COMPARE RIGHT    a condition: JOIN and / or; LEFT and RIGHT hp, mp, xp, target-hp, cast, characters,
//                                 monsters, monsters-around, nearest-monster, drops, drop-distance, since-kill,
//                                 cooldown:SKILL (a space in SKILL written
//                                 "_") or a number; COMPARE < <= == != >= >
//   if JOIN target-name ==|!= TEXT                  TEXT the rest of the line (may be empty)
//   if JOIN skill ready|cooling|cast SKILL          SKILL the rest of the line
//   if JOIN skills-ready any|all SKILLS             SKILLS the rest of the line, names between "|"
//   if JOIN has-target yes|no
//   if JOIN event EVENT                             EVENT = hp-lost, target-hit-by-others
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
