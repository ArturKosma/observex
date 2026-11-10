#pragma once

#include "core/Detection.h"

#include <cstddef>
#include <span>
#include <string_view>
#include <vector>

namespace observex {

// The lines a live log (engine/LiveLog) holds about one look of the recogniser, written by the app and read back by
// observex_bench --track:
//   look IMAGE TIME [SURE X Y WIDTH HEIGHT]...  what it found, surest first, in screen pixels;
//   kinds IMAGE [KIND SURE SECOND SURE]...       right after it, for a model with kinds (since 2026-11-08, so that
//                                                how steady the kinds shown are can be measured): each thing's surest
//                                                kind and how sure of it, then its second-surest, in the same order.
//                                                Kinds are numbered from 0 in the order of the model's model.json.
// Older logs have no kinds lines; readers skip the lines they do not know.

// Room for a look of up to 100 things (engine/Recognizer's Found::kMax) with its kinds.
inline constexpr std::size_t kLookChars = 8192;

// Writes the look line of `found`, and its kinds line for a model with kinds, into `out`, each ending in '\n'.
// Returns how many characters. Leaves out the things that do not fit (both lines hold the same ones).
std::size_t writeLook(std::span<char> out, unsigned long long image, double timeMs, std::span<const Detection> found);

// Reads a look line into `image`, `timeMs` and `found` (replaced; no kinds yet). False if `line` is not one.
bool readLook(std::string_view line, unsigned long long& image, double& timeMs, std::vector<Detection>& found);

// Reads a kinds line of the look of `image` into the kinds of `found`, thing by thing. False if `line` is not one
// for that image.
bool readKinds(std::string_view line, unsigned long long image, std::vector<Detection>& found);

} // namespace observex
