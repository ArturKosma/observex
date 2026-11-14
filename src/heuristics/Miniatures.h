#pragma once

#include "core/GameStats.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace observex {

// The game's miniatures: the small square pictures of skills and items (the hotbar's, the menu buttons at the bottom
// right, the inventory's). Lineage 2 frames each with a bevel whose top and left edges are a 1-pixel light line; the
// right and bottom ones darken with the picture while a skill cools down, so only the top and the left are looked for
// (2026-11-12, clip 2026-11-12_23-25-45: all five of the hotbar's on every frame, cooling or not, and on the whole screen
// nothing but the hotbar's, the menu's and the inventory's).
//
// Finds them in `area` of a grey image (`grey`, rows `stride` bytes apart, `width` x `height`): each a box with the
// frame's corner at its top left, `side` pixels square (x1 and y1 just past). Writes up to `max` into `out`, top to
// bottom, then left to right; returns how many.
int findMiniatures(const std::uint8_t* grey, std::ptrdiff_t stride, int width, int height, StatsArea area,
                   StatsArea* out, int max);

// The skills the user added (the Stats tile's Add Skill, 2026-11-12: "this will be useful on different chronicles"):
// each is its miniature as it looked when added, ready, grey, saved as SKILLS/NAME.pgm in the stats folder
// (models/ui/skills/Wind Strike.pgm). Read by heuristics/StatsReader, which finds them on the hotbar.
inline constexpr char kSkillsFolder[] = "skills";

// A name a skill can be saved under: trimmed, at most 31 bytes, none of the characters Windows refuses in a file name
// and no "_" (clip-stats.txt writes spaces as "_"). Empty if `name` cannot be one.
std::string skillFileName(std::string_view name);

// Writes a skill's miniature: `pixels` (grey, `side` x `side`) as `folder`/skills/NAME.pgm, the folder made if needed.
// False with *error if it could not be written.
bool saveSkill(const std::filesystem::path& folder, std::string_view name, const std::vector<std::uint8_t>& pixels,
               int side, std::string* error);

} // namespace observex
