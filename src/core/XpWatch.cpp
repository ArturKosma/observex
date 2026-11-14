#include "core/XpWatch.h"

#include <cstdio>
#include <cstdlib>

namespace observex {

namespace {

// "26.64%" as 26.64; below 0 if it is not a percent.
double percentOf(const GameGauge& gauge) {
    const char* text = gauge.text.data();
    char* end = nullptr;
    const double value = std::strtod(text, &end);
    if (end == text || *end != '%' || value < 0.0 || value > 100.0) {
        return -1.0;
    }
    return value;
}

} // namespace

std::optional<XpWatch::Change> XpWatch::read(const GameGauge* xp, double seconds) {
    const double value = xp != nullptr && xp->seen ? percentOf(*xp) : -1.0;
    if (value < 0.0) {
        return std::nullopt;
    }
    if (held_ >= 0.0 && seconds - heldSeconds_ > kForgetSeconds) {
        held_ = -1.0; // Long unseen: maybe another character.
    }
    if (value == held_) {
        heldSeconds_ = seconds;
        seen_ = -1.0;
        return std::nullopt;
    }
    if (value != seen_) {
        seen_ = value;
        seenSeconds_ = seconds;
        return std::nullopt;
    }
    // Read twice in a row: taken.
    const double before = held_;
    held_ = value;
    heldSeconds_ = seconds;
    seen_ = -1.0;
    if (before < 0.0) {
        return std::nullopt;
    }
    char text[96];
    if (before - value > 50.0) {
        std::snprintf(text, sizeof(text), "Level up, got %.2f%% XP: %.2f%% to %.2f%%", 100.0 - before + value, before,
                      value);
    } else if (value > before) {
        std::snprintf(text, sizeof(text), "Got %.2f%% XP: %.2f%% to %.2f%%", value - before, before, value);
    } else {
        std::snprintf(text, sizeof(text), "Lost %.2f%% XP: %.2f%% to %.2f%%", before - value, before, value);
    }
    return Change{text, seenSeconds_};
}

} // namespace observex
