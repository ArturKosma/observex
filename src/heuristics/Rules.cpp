#include "heuristics/Rules.h"

namespace observex {

int Rules::update(double timeMs, std::span<const Detection> found, Tracked* out, int max) {
    // Surest first, so what is at least kMinSure sure comes first.
    std::size_t sure = 0;
    while (sure < found.size() && found[sure].sure >= kMinSure) {
        ++sure;
    }
    return tracker_.update(timeMs, found.data(), static_cast<int>(sure), out, max);
}

} // namespace observex
