#pragma once

#include "app/SkillsView.h"
#include "app/Theme.h"
#include "core/ClipLines.h"
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
// The card Camera: the camera by the mouse's counts (core/MouseCamera's cameraOf) on the frame shown (the user asked,
// 2026-11-15 night: "add the Yaw/Pitch into stats, and make sure every single frame its updated"; on a clip since the
// same night: its camera lines, or Today's from its mouse lines): its yaw (degrees clockwise from north, the
// minimap's, or only turned since the counts began), pitch (down) and what said it, and how far the minimap's cone and
// the camera's turns were off the counts lately.
// The card Since the Brain Started (the user asked, 2026-11-16: "a mob kill counter, which counts since i start the
// brain", then "also adena picker which counts how much adena was picked"): the kills and the adena picked up while the
// brain acted, from its last start, each also per hour, and for how long it has acted; kept once it stops, until it
// starts again.

// What the card Since the Brain Started shows (App counts it).
struct BrainTally {
    bool shown = false;   // The card is drawn (live only, not on a clip).
    bool started = false; // The brain started since observex did; else the card says what it will count.
    bool acting = false;  // It acts now (its time still runs).
    double seconds = 0.0; // From its start to now, or to when it stopped.
    int kills = 0;        // Targets that died while it acted (core/TargetWatch's death).
    long long adena = 0;  // Adena picked up while it acted (the game's "You have obtained N Adena." messages).
};

class StatsTile {
public:
    // Draws the tile at the cursor in the current window, taking the room it drew in. `stats` null: `empty` is said
    // instead. `problem` (may be null or empty): why nothing is read, said under it. `skills`: the profile's, whose
    // icons `icons` draws. `camera`: the card Camera, first (not known: none, or one saying `cameraProblem` if that is
    // not empty). `tally`: the card Since the Brain Started, next (shown false: none).
    void draw(const theme::Fonts& fonts, const GameStats* stats, const char* empty, const char* problem,
              const std::vector<statetree::Skill>& skills, const SkillIcons& icons, float dpiScale,
              const CameraShown& camera = {}, const char* cameraProblem = nullptr, const BrainTally& tally = {});
};

} // namespace observex
