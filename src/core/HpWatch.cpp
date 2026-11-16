#include "core/HpWatch.h"

#include <charconv>
#include <cstdio>
#include <string_view>

namespace observex {

namespace {

// "199/256" as 199 and 256; false if it is not such a number.
bool pointsOf(const GameGauge& gauge, int* points, int* most) {
    const std::string_view text(gauge.text.data());
    const std::size_t slash = text.find('/');
    if (slash == std::string_view::npos || slash == 0 || slash + 1 >= text.size()) {
        return false;
    }
    const char* end = text.data() + text.size();
    const auto first = std::from_chars(text.data(), text.data() + slash, *points);
    const auto second = std::from_chars(text.data() + slash + 1, end, *most);
    return first.ec == std::errc() && first.ptr == text.data() + slash && second.ec == std::errc() &&
           second.ptr == end && *most > 0 && *points >= 0 && *points <= *most;
}

} // namespace

std::optional<HpWatch::Lost> HpWatch::read(const GameGauge* hp, double seconds) {
    int points = 0;
    int most = 0;
    if (hp == nullptr || !hp->seen || !pointsOf(*hp, &points, &most)) {
        return std::nullopt;
    }
    if (held_ >= 0 && seconds - heldSeconds_ > kForgetSeconds) {
        held_ = -1; // Long unseen: maybe another character.
    }
    if (points == held_ && most == heldMost_) {
        heldSeconds_ = seconds;
        seen_ = -1;
        return std::nullopt;
    }
    if (points != seen_ || most != seenMost_) {
        seen_ = points;
        seenMost_ = most;
        seenSeconds_ = seconds;
        return std::nullopt;
    }
    // Read twice in a row: taken.
    const int before = held_;
    const int beforeMost = heldMost_;
    held_ = points;
    heldMost_ = most;
    heldSeconds_ = seconds;
    seen_ = -1;
    if (before < 0 || points >= before || most < beforeMost) {
        return std::nullopt; // The first, HP that rose, or a lower most (a buff gone): no damage.
    }
    char text[96];
    std::snprintf(text, sizeof(text), "HP lost %d: %d/%d to %d/%d", before - points, before, beforeMost, points, most);
    return Lost{before - points, before, points, most, text, seenSeconds_};
}

} // namespace observex
