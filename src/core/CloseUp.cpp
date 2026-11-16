#include "core/CloseUp.h"

#include <algorithm>
#include <cmath>

namespace observex {

CloseUpArea closeUpArea(float middleX, float middleY, int screenWidth, int screenHeight, int share) {
    CloseUpArea area;
    area.width = std::max(1, screenWidth / std::max(1, share));
    area.height = std::max(1, screenHeight / std::max(1, share));
    area.x = std::clamp(static_cast<int>(std::lround(middleX - area.width * 0.5f)), 0, screenWidth - area.width);
    area.y = std::clamp(static_cast<int>(std::lround(middleY - area.height * 0.5f)), 0, screenHeight - area.height);
    return area;
}

int joinCloseUp(Detection* whole, int count, std::span<const Detection> close, const CloseUpArea& area, float margin,
                int capacity) {
    const float x0 = static_cast<float>(area.x) + margin * static_cast<float>(area.width);
    const float x1 = static_cast<float>(area.x + area.width) - margin * static_cast<float>(area.width);
    const float y0 = static_cast<float>(area.y) + margin * static_cast<float>(area.height);
    const float y1 = static_cast<float>(area.y + area.height) - margin * static_cast<float>(area.height);
    const auto inside = [&](const Detection& thing) {
        const float mx = thing.x + thing.width * 0.5f;
        const float my = thing.y + thing.height * 0.5f;
        return mx >= x0 && mx < x1 && my >= y0 && my < y1;
    };
    // The whole screen's outside the inner part, kept in place (surest first already).
    int kept = 0;
    for (int i = 0; i < count; ++i) {
        if (!inside(whole[i])) {
            whole[kept++] = whole[i];
        }
    }
    // The close-up's inside it, while there is room.
    for (const Detection& thing : close) {
        if (kept >= capacity) {
            break;
        }
        if (inside(thing)) {
            whole[kept++] = thing;
        }
    }
    std::stable_sort(whole, whole + kept, [](const Detection& a, const Detection& b) { return a.sure > b.sure; });
    return kept;
}

} // namespace observex
