#pragma once

#include "core/Heuristics.h"
#include "heuristics/Tracker.h"

namespace observex {

// Today's heuristics (core/Heuristics): the things at least kMinSure sure, followed from look to look by the Tracker.
// New rules go here (or in what it uses, all in src/heuristics); building observex_heuristics then makes the running
// app take them up: live, and on an open clip's "Today's heuristics".
class Rules final : public Heuristics {
public:
    int update(double timeMs, std::span<const Detection> found, Tracked* out, int max) override;
    void clear() override { tracker_.clear(); }

private:
    Tracker tracker_;
};

} // namespace observex
