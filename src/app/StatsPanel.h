#pragma once

#include "app/SkillsView.h"
#include "app/Theme.h"
#include "core/GameStats.h"
#include "core/StateTree.h"

#include <imgui.h>

#include <vector>

namespace observex {

// The Stats tile, live and on a clip (the user asked, 2026-11-12: first "a button to show all the stats on the right
// side of the screen. its toggleable", then a tile under the tab Stats, then Casting and Cooldowns; 2026-11-14: skills
// added in the Skills tab, their icons by their cooldowns and the cast, the target a column of its own when there is
// room, and the model's count of characters): what was read (engine/UiFinder through observex_stats.dll), in cards:
// "You" (the status window's bars, then Casting: how far, and the skill's icon and name), "Cooldowns" (each of the
// profile's skills with a cooldown: its icon, how far it has recharged, the seconds left, Ready or Pressed), "Nearby"
// (how many characters the model shows, and monsters), "Target" (its name and HP), and any other window's. Few colours:
// labels muted, values bright, bars coloured as the game's; what is not seen is muted with a dash. A number that did
// not agree with its bar is muted with a "?".
class StatsTile {
public:
    // Draws the tile at the cursor in the current window, taking the room it drew in. `stats` null: `empty` is said
    // instead. `problem` (may be null or empty): why nothing is read, said under it. `skills`: the profile's, whose
    // icons `icons` draws.
    void draw(const theme::Fonts& fonts, const GameStats* stats, const char* empty, const char* problem,
              const std::vector<statetree::Skill>& skills, const SkillIcons& icons, float dpiScale);
};

} // namespace observex
