#pragma once

#include "app/Theme.h"
#include "core/StateClipboard.h"
#include "core/StateTree.h"

#include <imgui.h>

#include <array>
#include <chrono>
#include <filesystem>
#include <string>
#include <vector>

namespace observex {

class SkillIcons;

// The State tile: the bot's brain (core/StateTree) in observex's look. It always works on a profile, a `.brain` file in
// Settings' brains folder: the last one opened, else the newest there, else a new "Brain"; every change is saved into
// it as it is made, and Ctrl+Z / Ctrl+Y undo and redo any change (the user asked, 2026-11-13). At its top Save (asks
// for a name and saves a copy under it, then works on the copy), Load (the profiles in the folder) and the profile's
// name in a field (typing another renames it and its file; a name taken gets the next number), then the Brain button;
// under them the tree on the left and, while a state is selected, its details
// on the right (a line
// between that drags; nothing there otherwise):
// - The tree: each state a box like the top bar's buttons (the selected one with the accent border, the current one
//   green while the brain acts), its tasks inside it under its name, smaller (the one that runs green; a Parallel
//   one marked by two upright bars and green the whole time its state is active), indented
//   under its parent, its transitions after it (a faint arrow and the target's name; a returning
//   arrow only when it goes back to itself). A right click offers Add State (under it; on
//   empty space under Root, named at once) and Delete; a click selects, a double click or F2 renames, Delete removes
//   it with all under it (not Root). A state dragged (not Root) moves, with all under it, just above or below the state
//   under the mouse, as its sibling; an accent line shows where while dragging (Escape cancels). The transitions and
//   tasks in the tree are only drawn: they are dragged in the details.
// - The details (a quiet "Details" over them): its name in a field, its transitions, each a card with a state to go to
//   and conditions as UE's (IF / AND / OR; a kind picked when added: Compare Number, a number or one of the game's
//   numbers compared with another; Has Target; Target Name; Skill; Is Skill Ready; Event, e.g. HP Lost), with a dot by
//   each that holds while it is current (an Event's green for a second after it came); a card dragged by its background (the user asked, 2026-11-16) goes above or below another, their
//   order being the order they are tried in, an accent line showing where; then its tasks (+ Task: a kind from a list, Use Skill with the profile's skills; a Parallel checkbox on
//   the kinds that can run alongside, statetree::canRunParallel). Lists of skills (Is Skill
//   Ready's, Use Skill's) are arrays as Unreal's: a row each, + to add one (drawSkillArray).
// - Copy and Paste (the user asked, 2026-11-16): a right click on a state's box, a transition's card, a condition's or a
//   task's row offers Copy for it and Paste for what was copied last where it goes (statetree::Clipboard); on a part's
//   title or + button, Paste last in that part. Ctrl+C / Ctrl+V on the tree's selected state. A task's row and an enter
//   condition's row are dragged by their background as a transition's card is (the user asked, 2026-11-16), to
//   another place in their list (drawDetailsDrag).
// The brain steps on every reading tick() is given (statetree::Runner); stop() when nothing is read.
class StateTreeView {
public:
    // Draws into the current window (the tile); trees are saved into, and loaded from, `folder`; `icons` draws the
    // Use Skill tasks' miniatures (the user asked, 2026-11-15). Draw it before what else reads keys: while the tile has
    // the keyboard, it takes the keys pressed this frame, so they do not also act on the review or a clip.
    // Right of the profile's name, the Brain button: Active (green) while `acting`, else Inactive; no tooltip (the user
    // asked, 2026-11-15). True when it was clicked (the app starts or stops acting). Only while acting is the current
    // state, and its task that runs, shown green (the user asked, 2026-11-15: nothing green while paused).
    // With a clip open, `replay`: the brain as recorded at the frame shown is green instead (the user asked,
    // 2026-11-15: a clip's review showed no active state or task), the condition dots on the frame's reading; a muted
    // line under the bar says when it cannot be shown (not recorded, or its state not in this profile).
    struct Replay {
        bool recorded = false;                           // The clip has brain lines (none before 2026-11-15).
        const statetree::BrainShown* brain = nullptr;    // At the frame shown; null before the first.
        const statetree::Variables* variables = nullptr; // The frame's reading; null: none.
    };
    bool draw(const theme::Fonts& fonts, const std::filesystem::path& folder, const SkillIcons& icons, bool acting,
              const Replay* replay = nullptr);
    // The brain at Root again, its first task next: the app starts the whole tree afresh when it starts acting.
    void restart() { runner_.reset(); }
    // The current state the runner is in (for the app to actuate its tasks while acting); null if there is none.
    const statetree::State* currentState() const { return tree_.find(runner_.current(tree_)); }
    // Its task that runs (index in its tasks; -1: all done, or none), the app's word that it is done (the next one
    // runs), and how many tasks have started so far (a change: one starts, afresh).
    int currentTask() const { return runner_.task(tree_); }
    void finishTask() { runner_.finishTask(); }
    std::uint64_t taskStarts() const { return runner_.taskStarts(); }
    // The Parallel tasks that run now: those of the active states (statetree::Runner::parallelTasks).
    std::vector<statetree::TaskAt> parallelTasks() const {
        return statetree::Runner::parallelTasks(tree_, runner_.current(tree_));
    }
    // Save, Load and the profile's name in a field, as at the tile's top: the Skills tab has them too (the user asked,
    // 2026-11-15: the profile chooseable there as in State). Drawn at the cursor, BarButton::kHeight high; profiles
    // are in `folder`. fileError() says why the last load or save failed (empty when it worked).
    void drawProfileBar(const theme::Fonts& fonts, const std::filesystem::path& folder);
    const std::string& fileError() const { return fileError_; }

    // A new reading (live, or a clip's): the brain takes a step, and shows its current state.
    void tick(const statetree::Variables& variables);
    // Nothing is read: the brain is back at Root, for the next reading.
    void stop();
    // The profile `name` in `folder` in place of this one; false (and why, shown in the tile) if it cannot be read.
    bool load(const std::filesystem::path& folder, const std::string& name);
    // The profile's name; empty until one is loaded or opened.
    const std::string& name() const { return name_; }
    // A change not saved yet (a field still being typed in) is saved: before observex closes.
    void flush();
    // The profile in `folder` open (the newest there, as draw() opens it), also while the tile is not shown: the
    // Skills tab and the stats reader need its skills.
    void ensureProfile(const std::filesystem::path& folder);
    // The profile's skills (the Skills tab: "a pocket with all skills", kept in the profile like the tree; the user
    // asked, 2026-11-14), to change; saveSkills() after a change keeps it (Ctrl+Z in the tile undoes it) and writes it.
    std::vector<statetree::Skill>& skills() { return tree_.skills(); }
    const std::vector<statetree::Skill>& skills() const { return tree_.skills(); }
    const statetree::Tree& tree() const { return tree_; }
    void saveSkills();
    // The profile as its file holds it: any change to the tree or its skills (a change made, a profile loaded, undo,
    // redo) gives another text.
    const std::string& savedText() const { return history_.text(); }

private:
    // Save, Load and the name field (`title` its text while typed in: each place it is drawn has its own).
    // `reserve`: room kept right of the name's field (it narrows for it).
    void drawToolbar(const theme::Fonts& fonts, const std::filesystem::path& folder, std::string& title,
                     float reserve = 0.0f);
    void drawTree(const theme::Fonts& fonts);
    // While a state is dragged: where it lands shown (its rows' boxes `mins`, `maxes`), and moved there once let go.
    void drawDrag(const theme::Fonts& fonts, const std::vector<statetree::Row>& rows, const std::vector<ImVec2>& mins,
                  const std::vector<ImVec2>& maxes);
    void drawDetails(const theme::Fonts& fonts);
    // The lists in the details whose items are dragged to another place in them (the user asked, 2026-11-16: a
    // state's transitions, then its tasks and enter conditions, as states are dragged in the tree).
    enum class DetailsList : std::uint8_t { Transitions, Tasks, Enter };
    // Each item's box in one of them, as drawn this frame (where a dragged one lands), and the gap between two.
    struct ListBoxes {
        std::vector<ImVec2> mins;
        std::vector<ImVec2> maxes;
        float gap = 0.0f;
        void add(ImVec2 min, ImVec2 max) {
            mins.push_back(min);
            maxes.push_back(max);
        }
    };
    // Right after the invisible button `state`'s item at `index` in `list` is grabbed by: held there and moved past
    // ImGui's drag threshold, it is taken (while nothing else is dragged). True while it is the one dragged.
    bool grab(int state, DetailsList list, std::size_t index);
    // While one of `state`'s items is dragged (`boxes` its list's): what it is in a small box by the mouse, an accent
    // line where it lands (before the first item whose middle is below the mouse, else after the last), and moved
    // there once let go (one change: Ctrl+Z undoes it); Escape, off the details, or just before or after itself: not.
    void drawDetailsDrag(const theme::Fonts& fonts, statetree::State& state, const ListBoxes& boxes);
    // One transition of `state` (`index` in its transitions), as a card the panel's width, dragged by its background;
    // true when its X was clicked. Its card's box goes into `boxes`.
    bool drawTransition(const statetree::State& state, statetree::Transition& transition, std::size_t index,
                        float width, ListBoxes& boxes);
    // `state`'s enter conditions (not Root's): a card with them, each row dragged by its background (their boxes into
    // `boxes`), or only + Condition while it has none.
    void drawEnter(statetree::State& state, float width, ListBoxes& boxes);
    // A card's rows of `conditions` (IF / AND / OR, by kind, X), `inner` wide, then + Condition; `checking`: a dot at
    // `dotX` by each, green while it holds on the last reading. With `drag` (`state`'s enter conditions, `left` to
    // `right` the card's sides), each row is dragged by its background, the grip in place of its dot under the mouse,
    // its box into `drag`.
    struct ConditionsDrag {
        int state = 0;
        float left = 0.0f;
        float right = 0.0f;
        ListBoxes* boxes = nullptr;
    };
    // Where the details' right-click menu (Copy and Paste; the user asked, 2026-11-16) was opened in `state`: on a
    // transition's card (`transition` its index), a condition's row (`index`; in transition `transition`'s card, or
    // with -1 the enter conditions'), a task's row (`index`), or a part's title or + button (Paste only, last there).
    struct Spot {
        enum class On : std::uint8_t { None, Transition, Condition, Task, Transitions, Tasks, Enter };
        On on = On::None;
        int state = 0;
        int transition = -1;
        std::size_t index = 0;
        bool operator==(const Spot&) const = default;
    };
    // `spot`, from `min` to `max`: a right click there opens the details' menu for it (only the first spot that takes
    // it this frame, so a row inside a card wins over the card: rows come first); while that menu is open for a card or
    // a row, a thin accent outline round it.
    void rightClick(const Spot& spot, ImVec2 min, ImVec2 max);
    // The details' menu for spot_ in `state`, opened when `open` (a right click this frame): Copy for the card or row,
    // Paste for what is copied, enabled where it goes. Done once the menu is closed.
    void drawDetailsMenu(statetree::State& state, bool open);
    // What is copied, pasted at spot_ in `state`; `apply` false: only whether it goes there.
    bool pasteAtSpot(statetree::State& state, bool apply);
    // What is copied, pasted onto the state `id` in the tree (its box's menu, or Ctrl+V on it): a state as its last
    // child (then selected), a transition or task after its last, a condition after its last enter condition (not
    // Root's). `apply` false: only whether it goes there.
    bool pasteOnto(int id, bool apply);
    // With `spot` (on Condition, its state and transition), each row is a spot (`index` its row) and, for the enter
    // conditions while there are none, + Condition one (on Enter).
    void drawConditions(std::vector<statetree::Condition>& conditions, bool checking, float inner, float dotX,
                        const ConditionsDrag* drag = nullptr, const Spot* spot = nullptr);
    // A list of the profile's skills as Unreal's arrays (Is Skill Ready's, Use Skill's), at the cursor, `width` wide, on
    // a well filled `fill`: "Skills", how many and + at its top (a list of the skills to add one from), then a row
    // each: its place (a handle: dragged, it moves up or down), miniature, name (a click picks another) and an X.
    void drawSkillArray(std::vector<std::string>& skills, float width, ImU32 fill);
    // One of the profile's skills in a list to pick from: its miniature, name and how the last reading saw it; dimmed
    // and not pickable while `taken` (in the array already). True when picked.
    bool skillOption(const std::string& name, bool selected, bool taken);
    // How wide such a list must be for `names` whole.
    float skillOptionsWidth(const std::vector<std::string>& names) const;
    // How the last reading saw the skill `name`: "ready", "not seen", how much of its cooldown is left ("45%"), or
    // nothing when it was not read.
    std::string skillStatus(const std::string& name) const;
    // A number to type, or one of the game's numbers to pick, `width` wide.
    void drawOperand(const char* id, statetree::Operand& operand, float width);
    // The skills to offer: the profile's.
    std::vector<std::string> skillNames() const;
    // A part of the details: its title, how many it holds, and a faint line under; a right click on it opens the menu
    // for `spot` (to paste last in that part).
    void sectionHeader(const theme::Fonts& fonts, const char* title, std::size_t count, const Spot& spot);
    void drawRename(const theme::Fonts& fonts, ImVec2 min, ImVec2 max);
    // While the tile has the keyboard, the keys held this frame are its own.
    void keepKeys();
    // After a name field: the name it had when clicked comes back if it is left empty; trimmed.
    void keepName(std::string& name);
    // The current state, as shown: none while the brain does not act (and a clip's recorded one while a clip is open).
    bool isCurrent(int state) const { return shownState_ != 0 && state == shownState_; }
    // Active, as shown: the current state or one above it whose transitions count; none while the brain does not act.
    bool isActive(int state) const {
        return shownState_ != 0 && statetree::Runner::activeFrom(tree_, shownState_, state);
    }
    void drawMenu(bool open);
    // The newest profile in `folder` that reads, or a new one ("Brain"), saved at once.
    void openProfile(const std::filesystem::path& folder);
    // The profile and its file renamed (a name taken: the next number).
    void renameProfile(const std::string& name);
    // Save: the profile, skills and tree, written again under `name` as a new profile (a name taken: the next number),
    // the one before kept as it is; the tile then works on the new one (the user asked, 2026-11-16: to copy a profile).
    // False (and why, in fileError()) if the name cannot be a file's or the copy cannot be written.
    bool saveProfileAs(const std::string& name);
    // Save's popup, under the button at `corner`: the name to save as in a field, and Save.
    void drawSavePopup(const std::filesystem::path& folder, ImVec2 corner);
    // A change made (while no field is being typed in): kept for Ctrl+Z, and written.
    void keepChanges();
    // After an undo or redo (`changed`: there was one): written.
    void stepped(bool changed);
    // The tree as the history keeps it into the profile's file (a .tmp first, renamed over).
    void write();

    void add(int parent);
    void rename(int state);
    void remove(int state);

    statetree::Tree tree_;
    statetree::History history_; // Ctrl+Z and Ctrl+Y; what the profile's file holds.
    statetree::Runner runner_;
    statetree::Variables variables_; // The last reading's, shown when picking a variable.
    std::filesystem::path folder_;   // The profile's folder,
    std::string name_;               // and name (its file NAME.brain).
    std::string title_;              // Its name field, while typed in (the tile's,
    std::string barTitle_;           // and the Skills tab's).
    int selected_ = 0;
    int dragging_ = 0; // A state dragged to another place in the tree (the user asked, 2026-11-15); 0: none.
    // A transition's card, a task's row or an enter condition's row dragged to another place in its list in the
    // details (the user asked, 2026-11-16): its state (0: none), its list and its index there.
    struct DetailsDrag {
        int state = 0;
        DetailsList list = DetailsList::Transitions;
        std::size_t index = 0;
    };
    DetailsDrag detailsDrag_;
    ImGuiID dragArray_ = 0; // A skill array whose row is being dragged up or down (0: none),
    int dragIndex_ = -1;    // and where the dragged skill is in it now.
    int renaming_ = 0;         // The state whose name is being edited in the tree,
    bool renameFocus_ = false; // which takes the keyboard on its first frame.
    std::string renameText_;
    const SkillIcons* icons_ = nullptr; // The profile's skills' icons, for the Use Skill tasks; set each draw().
    bool acting_ = false;               // The brain acts (set each draw()): the current state and task shown green.
    // What is shown green (set each draw()): the runner's current state and task while acting, a clip's recorded ones
    // while a clip is open; 0 / -1: none.
    int shownState_ = 0;
    int shownTask_ = -1;
    bool replaying_ = false;              // A clip is open: variables_ is its frame's reading,
    statetree::Variables liveVariables_;  // and this the last live one, for after.
    std::string replayNote_;              // Why the clip's brain is not shown; empty: it is (or no clip).
    std::string nameBefore_; // The name when a details' name field was clicked: an empty one puts it back.
    int menuState_ = 0;      // The state the right-click menu is for (0: none, a click on empty space).
    // Copy and Paste: what was copied last, kept while observex runs (into another profile too).
    statetree::Clipboard clipboard_;
    Spot spot_;                  // Where the details' menu was opened,
    bool spotClicked_ = false;   // by a right click this frame;
    bool spotMenuShown_ = false; // it was shown last frame (its card or row outlined).
    float treeShare_ = 0.45f; // The tree's share of the tile's width; the details have the rest.
    std::vector<std::string> saved_; // The profiles in the folder, read when Load opens.
    std::string saveName_;           // Save's name field,
    bool saveFocus_ = false;         // which takes the keyboard on the popup's first frame.
    std::string fileError_;          // Why the last load, save or rename failed; empty when it worked.
    // When each event (by statetree::Event) last came live: its conditions' dots stay green kEventShownSeconds after.
    std::array<std::chrono::steady_clock::time_point, statetree::kEvents> eventSeen_{};
    static constexpr double kEventShownSeconds = 1.0;
    bool eventShown(statetree::Event event) const {
        const auto seen = eventSeen_[static_cast<std::size_t>(event)];
        return seen != std::chrono::steady_clock::time_point{} &&
               std::chrono::duration<double>(std::chrono::steady_clock::now() - seen).count() < kEventShownSeconds;
    }
};

} // namespace observex
