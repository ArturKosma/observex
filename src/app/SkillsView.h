#pragma once

#include "app/ScreenImage.h"
#include "app/Theme.h"
#include "core/StateTree.h"
#include "engine/UiFinder.h"

#include <imgui.h>

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace observex {

class StateTreeView;

// Where the Add Skill popup's miniatures come from: the live UI finder, or an open clip's (app/ClipView).
struct MiniatureSource {
    std::function<std::uint64_t()> ask;          // Asks for them on the next image; the ask's number, 0 if none can be.
    std::function<const UiMiniatures*()> newest; // The newest found, or null.
};

// The Skills tab's Refresh button (the app's Refresh: the mouse over each skill on the hotbar, its MP cost read off its
// tooltip): whether it runs and how far it is, and what a click does (starts or stops it).
struct RefreshButton {
    bool running = false;
    float progress = 0.0f; // 0 to 1, while it runs.
    std::function<void()> click;
};

// The profile's skills' miniatures on the GPU, to draw as their icons (the Skills tab, and the Stats tile next to each
// cooldown and the cast; the user asked, 2026-11-14).
class SkillIcons {
public:
    // Makes the icons of `skills` afresh (call when they changed). Through `device`, the UI's.
    void make(ID3D11Device* device, const std::vector<statetree::Skill>& skills);
    // Draws the icon of the skill named `name` from `min` to `max` over `background` (its rounded corners are painted
    // in it); false, drawing nothing, if there is none.
    bool draw(ImDrawList* drawList, std::string_view name, ImVec2 min, ImVec2 max, float rounding,
              ImU32 background) const;

private:
    struct Entry {
        std::string name;
        std::unique_ptr<ScreenImage> image;
    };
    std::vector<Entry> entries_;
};

// The Skills tab (the user asked, 2026-11-14: "a pocket with all skills", kept in the profile like the state tree):
// at the top the profile, chosen as in the State tile (Load, and its name in a field that renames it; the user asked,
// 2026-11-15) and Refresh (RefreshButton); then two columns (the user asked, 2026-11-16, in place of Has Cooldown),
// side by side in a wide tile, else one under the other: **Active skills** (their cooldowns read, in the Stats tile's
// Cooldowns; Use Skill is done once the skill's MP was taken) and **Utility skills** (no cooldown read; Use Skill is
// done once the key was pressed, held a moment and let go). Each column's + adds a skill to it (picks a miniature
// among those on the screen, the hotbar's first, while the skill is ready, then its name). Each skill a card: its
// icon, name, MP Cost (active skills only), Shortcut (the key on its hotbar slot, as read: for the Pico to press;
// typed in, it is learned from the slot) and an X that removes it; dragged by its background (six grip dots at its
// left under the mouse), it moves to the other column or to another place in its own. Changes go into the profile at
// once (StateTreeView::saveSkills: Ctrl+Z in the State tile undoes them).
class SkillsView {
public:
    // A shortcut typed for the skill `name` (already saved into the profile): the slot's letters are to be learned.
    using KeyTyped = std::function<void(const std::string& name, const std::string& key)>;

    // Draws into the current window (the tile); profiles are in `folder` (Settings' brains folder).
    void draw(const theme::Fonts& fonts, ID3D11Device* device, StateTreeView& profile,
              const std::filesystem::path& folder, const SkillIcons& icons, const MiniatureSource& source,
              const KeyTyped& keyTyped, const RefreshButton& refresh, float dpiScale);

private:
    // Where one column was drawn this frame: for a dragged card to land in.
    struct Card {
        int skill = 0; // Its index in the profile's skills.
        float top = 0.0f;
        float bottom = 0.0f;
    };
    struct Column {
        float left = 0.0f;
        float right = 0.0f;
        float top = 0.0f;
        float cardsTop = 0.0f; // Under its header.
        float bottom = 0.0f;
        std::vector<Card> cards;
    };
    // What one frame's cards did, applied once both columns are drawn.
    struct Pass {
        Column columns[2]; // Active, utility.
        int removed = -1;
        bool changed = false;
        std::vector<std::pair<std::string, std::string>> typedKeys; // Skill, key: learned once saved.
        const MiniatureSource* source = nullptr;
    };

    // Draws the column of active or utility skills from `at`, `width` wide, into `pass`; returns its bottom.
    float drawColumn(const theme::Fonts& fonts, std::vector<statetree::Skill>& skills, const SkillIcons& icons,
                     bool active, ImVec2 at, float width, float dpiScale, Pass& pass);
    // A dragged card (dragging_) over the columns: where it would land (-1: nowhere), its place among that column's
    // other cards in `slot`; drawn as a line there and the card by the mouse, or, let go, moved.
    int dropColumn(const Pass& pass, bool sideBySide, int* slot) const;
    void drawDrag(const theme::Fonts& fonts, const std::vector<statetree::Skill>& skills, const SkillIcons& icons,
                  const Pass& pass, bool sideBySide, float dpiScale) const;
    void drop(std::vector<statetree::Skill>& skills, const Pass& pass, bool sideBySide, bool* changed);

    void drawAddSkill(const theme::Fonts& fonts, ID3D11Device* device, StateTreeView& profile,
                      const MiniatureSource& source, float dpiScale);

    // The Add Skill popup: the ask it waits for, the miniatures it shows (a copy of the answer, and its atlas on the
    // GPU), which one is picked, the name typed, and the column it adds to.
    std::uint64_t asked_ = 0;
    std::unique_ptr<UiMiniatures> shown_;
    std::unique_ptr<ScreenImage> atlas_;
    int picked_ = -1;
    std::string name_;
    bool addActive_ = true;
    std::string error_;
    bool focusName_ = false;
    // The card being dragged (its skill's index; -1: none), and whether the mouse has moved it yet.
    int dragging_ = -1;
    bool dragMoved_ = false;
    // The Shortcut field being typed in: its skill (-1: none) and its text.
    int keyEditing_ = -1;
    std::string keyEdit_;
    // The MP Cost field being typed in, likewise.
    int mpEditing_ = -1;
    std::string mpEdit_;
};

} // namespace observex
