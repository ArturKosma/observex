#pragma once

#include "app/Theme.h"
#include "core/StateTree.h"

#include <imgui.h>

#include <chrono>
#include <filesystem>
#include <string>
#include <vector>

namespace observex {

// The State tile: the bot's brain (core/StateTree) in observex's look. It always works on a profile, a `.brain` file in
// Settings' brains folder: the last one opened, else the newest there, else a new "Brain"; every change is saved into
// it as it is made, and Ctrl+Z / Ctrl+Y undo and redo any change (the user asked, 2026-11-13). At its top Load (the
// profiles in the folder) and the profile's name in a field (typing another renames it and its file; a name taken gets
// the next number); under them the tree on the left and, while a state is selected, its details on the right (a line
// between that drags; nothing there otherwise):
// - The tree: each state a box like the top bar's buttons (the selected one with the accent border, the current one
//   green), indented under its parent, its transitions after it (a faint arrow and the target's name; a returning
//   arrow when it goes back to itself or a state above it, as UE's). A right click offers Add State (under it; on
//   empty space under Root, named at once) and Delete; a click selects, a double click or F2 renames, Delete removes
//   it with all under it (not Root).
// - The details (a quiet "Details" over them): its name in a field, its transitions, each a state to go to and
//   conditions as UE's (IF / AND / OR; a kind picked when added: Compare Number, a number or one of the game's numbers
//   compared with another; Has Target; Target Name; Skill), with a dot by each that holds while it is current; then its
//   tasks (+ Task: a kind from a list, Use Skill with one of the profile's skills).
// The brain steps on every reading tick() is given (statetree::Runner); stop() when nothing is read.
class StateTreeView {
public:
    // Draws into the current window (the tile); trees are saved into, and loaded from, `folder`. Draw it before what
    // else reads keys: while the tile has the keyboard, it takes the keys pressed this frame, so they do not also act on
    // the review or a clip.
    void draw(const theme::Fonts& fonts, const std::filesystem::path& folder);

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
    void saveSkills();
    // The profile as its file holds it: any change to the tree or its skills (a change made, a profile loaded, undo,
    // redo) gives another text.
    const std::string& savedText() const { return history_.text(); }

private:
    void drawToolbar(const theme::Fonts& fonts, const std::filesystem::path& folder);
    void drawTree(const theme::Fonts& fonts);
    void drawDetails(const theme::Fonts& fonts);
    // One transition of `state`, as a card the panel's width; true when its X was clicked.
    bool drawTransition(const statetree::State& state, statetree::Transition& transition, float width);
    // A number to type, or one of the game's numbers to pick, `width` wide.
    void drawOperand(const char* id, statetree::Operand& operand, float width);
    // The skills to offer: the profile's.
    std::vector<std::string> skillNames() const;
    // A part of the details: its title, how many it holds, and a faint line under.
    void sectionHeader(const theme::Fonts& fonts, const char* title, std::size_t count);
    void drawRename(const theme::Fonts& fonts, ImVec2 min, ImVec2 max);
    // While the tile has the keyboard, the keys held this frame are its own.
    void keepKeys();
    // After a name field: the name it had when clicked comes back if it is left empty; trimmed.
    void keepName(std::string& name);
    bool isCurrent(int state) const { return state == runner_.current(tree_); }
    void drawMenu(bool open);
    // The newest profile in `folder` that reads, or a new one ("Brain"), saved at once.
    void openProfile(const std::filesystem::path& folder);
    // The profile and its file renamed (a name taken: the next number).
    void renameProfile(const std::string& name);
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
    std::string title_;              // Its name field, while typed in.
    int selected_ = 0;
    int renaming_ = 0;         // The state whose name is being edited in the tree,
    bool renameFocus_ = false; // which takes the keyboard on its first frame.
    std::string renameText_;
    std::string nameBefore_; // The name when a details' name field was clicked: an empty one puts it back.
    int menuState_ = 0;      // The state the right-click menu is for (0: none, a click on empty space).
    float treeShare_ = 0.45f; // The tree's share of the tile's width; the details have the rest.
    std::vector<std::string> saved_; // The profiles in the folder, read when Load opens.
    std::string fileError_;          // Why the last load, save or rename failed; empty when it worked.
};

} // namespace observex
