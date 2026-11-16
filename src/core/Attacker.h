#pragma once

#include "core/Distances.h"
#include "core/GameStats.h"

#include <cstdint>
#include <span>

namespace observex {

// The monster hitting the own character, the brain's Attacker (Select Target's and Flee's; the user's clip
// 2026-11-16_14-04-25: Select Target went for an Imp 640 away while another hit the own character from right beside
// it, and Flee ran from the far one). Lineage 2 does not say who hits: it is taken to be the monster nearest the own
// character, within kAttackerReach. Platform-free.

// A monster on the screen now: its tracker id, its box on the screen image, and how far it is from the own character
// along the ground (own heights; below 0: not known, the ground camera not known yet).
struct AttackerCandidate {
    std::uint32_t id = 0;
    StatsArea box{};
    float distance = -1.0f;
};

// How far from the own character a monster can be to be taken for the attacker: 300 game units (Imps hit from beside
// it, 50-100 away; the distances are about 19% off, 2026-11-16). A guess to tune live; archers and mages hit from
// farther and are not taken.
inline constexpr float kAttackerReach = 300.0f / kGameUnits;
// How sure of the monster kind one must be (the tracker's) to be taken: lower than Select Target's 60% (Recognizer's
// kTellKind), as the own character is being hit, so one is there (that clip's hitting Imp was 45-65% a monster when
// found at all).
inline constexpr float kAttackerKindSure = 0.4f;

// How far a monster's feet are from the own character's on the screen, in own heights (the own box's height), for when
// the ground camera is not known: across the screen and, as the camera looks down, about twice as far up or down it.
float screenDistance(const StatsArea& box, const StatsArea& own);

// The nearest of `candidates` within kAttackerReach (by their distance); its index, -1 for none.
int nearestAttacker(std::span<const AttackerCandidate> candidates);

// Where to point at `box` to select it: the middle of the widest part of it beside `own` (the own character's box; a
// click on the own character targets it) if that is at least kAimWidth wide, else its middle; screen image pixels.
inline constexpr int kAimWidth = 16;
void aimBeside(const StatsArea& box, const StatsArea* own, float& x, float& y);

} // namespace observex
