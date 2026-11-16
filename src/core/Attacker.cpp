#include "core/Attacker.h"

#include <algorithm>
#include <cmath>

namespace observex {

float screenDistance(const StatsArea& box, const StatsArea& own) {
    const float height = static_cast<float>(std::max(1, own.y1 - own.y0));
    const float across = 0.5f * static_cast<float>((box.x0 + box.x1) - (own.x0 + own.x1)) / height;
    const float up = 2.0f * static_cast<float>(box.y1 - own.y1) / height;
    return std::sqrt(across * across + up * up);
}

int nearestAttacker(std::span<const AttackerCandidate> candidates) {
    int best = -1;
    for (int i = 0; i < static_cast<int>(candidates.size()); ++i) {
        const float distance = candidates[static_cast<std::size_t>(i)].distance;
        if (distance >= 0.0f && distance <= kAttackerReach &&
            (best < 0 || distance < candidates[static_cast<std::size_t>(best)].distance)) {
            best = i;
        }
    }
    return best;
}

void aimBeside(const StatsArea& box, const StatsArea* own, float& x, float& y) {
    x = 0.5f * static_cast<float>(box.x0 + box.x1);
    y = 0.5f * static_cast<float>(box.y0 + box.y1);
    if (own == nullptr || own->x1 <= box.x0 || own->x0 >= box.x1 || own->y1 <= box.y0 || own->y0 >= box.y1) {
        return; // Not over the own box.
    }
    // The parts of its width left and right of the own box.
    const int left = std::max(0, std::min(box.x1, own->x0) - box.x0);
    const int right = std::max(0, box.x1 - std::max(box.x0, own->x1));
    if (std::max(left, right) < kAimWidth) {
        return;
    }
    x = left >= right ? static_cast<float>(box.x0) + 0.5f * static_cast<float>(left)
                      : static_cast<float>(box.x1) - 0.5f * static_cast<float>(right);
}

} // namespace observex
