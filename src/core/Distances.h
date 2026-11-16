#pragma once

#include "core/Detection.h"
#include "core/Ground.h"

#include <optional>

namespace observex {

// How far things are from the user's own character (the user asked, 2026-11-16: a Distance per character and per
// drop, written while the flags Characters or Drops are on and shown in the live view; later a condition of the
// brain). A thing stands on the ground where the ray from the camera's eye through its foot point meets it: the
// terrain the depth network shaped (core/Ground's GroundHeights, heuristics/DepthTerrain) where it is known, else the
// flat ground through the own character's feet. Its distance is along the ground from the own character's feet (as
// Lineage 2 measures a skill's range: across, not up), in own heights; kGameUnits makes them the game's units.
//
// The camera is everything: the pitch says where the ray goes, so a degree off moves a thing at 600 game units about
// 70 away, at 900 about 140 and beyond 2000 400 or more (the geodata check of 2026-11-15, clip 2026-11-15_14-24-16).

// One own character's height in the game's units (the geodata of 2026-11-15: a tree trunk next to the character).
inline constexpr float kGameUnits = 47.0f;

// The farthest a ray looks for the terrain, along the ground from the feet (own heights): farther, the flat ground.
inline constexpr float kFarthestTerrain = 64.0f;

// The farthest distance said (own heights, 4000 game units): farther, a thing's feet are a few pixels under the
// horizon, where a pixel or a tenth of a degree of pitch is hundreds of units (on the user's clips, 2026-11-16, its
// distance moved 3-30% from frame to frame there, against under 1% nearer, and went up to millions of units).
inline constexpr float kFarthestDistance = 4000.0f / kGameUnits;

// Where on the ground a ray from the camera's eye lands: right, away and up of the own character's feet (own heights,
// as worldToScreen's).
struct GroundSpot {
    float right = 0.0f;
    float away = 0.0f;
    float up = 0.0f;
};

// Where the ray through screen pixel (x, y) of `camera` first meets the ground: `heights`' terrain if it is known
// (null or not known: the flat ground through the feet). None while the camera is unknown, at or above the horizon,
// or where the ray meets no ground within kFarthestTerrain and goes above the flat ground's horizon.
std::optional<GroundSpot> groundUnder(const GroundCamera& camera, const GroundHeights* heights, float x, float y);

// How far `spot` is from the own character's feet along the ground (own heights).
float alongGround(const GroundSpot& spot);

// Where a thing found stands, as screen pixels: a character on its box's bottom middle (its feet), a drop on its
// middle (it lies on the ground, a few pixels tall).
inline void characterFoot(const Detection& box, float& x, float& y) {
    x = box.x + box.width * 0.5f;
    y = box.y + box.height;
}
inline void dropFoot(const Detection& box, float& x, float& y) {
    x = box.x + box.width * 0.5f;
    y = box.y + box.height * 0.5f;
}

// The distance of a character or a drop from the own character (own heights); none where groundUnder has none, or
// farther than kFarthestDistance.
std::optional<float> characterDistance(const GroundCamera& camera, const GroundHeights* heights, const Detection& box);
std::optional<float> dropDistance(const GroundCamera& camera, const GroundHeights* heights, const Detection& box);

} // namespace observex
