#pragma once

#include "app/Theme.h"
#include "core/Costs.h"

#include <span>

namespace observex {

// The Debug tile (the user asked, 2026-11-15: "sum up every ms cost of the observex ... its needed to know what to
// optimize"): at its top the total the top bar shows right of Show (each part's run added up, core/Costs), then a row
// for each part of observex (a thread: capture, the recogniser, ..., the app's own frame) and under it its steps
// (those that ran that second), each with one run's average and slowest time, runs a second, and how busy it kept its
// thread (each x runs). `when` says what the numbers are of, under the total ("over the last second", or a clip's
// moment); `empty` is said with no parts.
void drawCosts(std::span<const CostPart> parts, const char* when, const char* empty, const theme::Fonts& fonts,
               float dpiScale);

} // namespace observex
