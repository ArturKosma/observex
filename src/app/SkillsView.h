#pragma once

#include "app/ScreenImage.h"
#include "app/Theme.h"
#include "core/StateTree.h"
#include "engine/UiFinder.h"

#include <imgui.h>

#include <cstdint>
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
// + Add Skill at the top (picks a miniature among those on the screen, the hotbar's first, while the skill is ready,
// then its name and whether it has a cooldown to read: one without is never in the Stats tile's Cooldowns), then the
// profile's skills, each its icon, name, Has Cooldown, and an X that removes it. Changes go into the profile at once
// (StateTreeView::saveSkills: Ctrl+Z in the State tile undoes them).
class SkillsView {
public:
    // Draws into the current window (the tile).
    void draw(const theme::Fonts& fonts, ID3D11Device* device, StateTreeView& profile, const SkillIcons& icons,
              const MiniatureSource& source, float dpiScale);

private:
    void drawAddSkill(const theme::Fonts& fonts, ID3D11Device* device, StateTreeView& profile,
                      const MiniatureSource& source, float dpiScale);

    // The Add Skill popup: the ask it waits for, the miniatures it shows (a copy of the answer, and its atlas on the
    // GPU), which one is picked, the name typed, whether it has a cooldown.
    std::uint64_t asked_ = 0;
    std::unique_ptr<UiMiniatures> shown_;
    std::unique_ptr<ScreenImage> atlas_;
    int picked_ = -1;
    std::string name_;
    bool cooldown_ = true;
    std::string error_;
    bool focusName_ = false;
};

} // namespace observex
