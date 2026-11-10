#include "core/ImageView.h"

#include <algorithm>
#include <iterator>

namespace observex {

float steppedZoom(float zoom, int steps) {
    // The level at or just under `zoom`, then `steps` from it.
    int level = 0;
    while (level + 1 < static_cast<int>(std::size(kZoomSteps)) && kZoomSteps[level + 1] <= zoom + 0.001f) {
        ++level;
    }
    return kZoomSteps[std::clamp(level + steps, 0, static_cast<int>(std::size(kZoomSteps)) - 1)];
}

ImageView zoomedAt(const ImageView& view, float zoom, float x, float y, float width, float height) {
    // Where the pixel is across the view (0 at its left edge, 1 at its right), before and after.
    const float acrossX = (x - view.left) * view.zoom / width;
    const float acrossY = (y - view.top) * view.zoom / height;
    const ImageView zoomed{zoom, x - acrossX * width / zoom, y - acrossY * height / zoom};
    return keptInside(zoomed, width, height);
}

ImageView keptInside(const ImageView& view, float width, float height) {
    const float zoom = std::clamp(view.zoom, kZoomSteps[0], kZoomSteps[std::size(kZoomSteps) - 1]);
    return ImageView{zoom, std::clamp(view.left, 0.0f, width - width / zoom),
                     std::clamp(view.top, 0.0f, height - height / zoom)};
}

ImageView showing(const ImageView& view, const Box& box, float width, float height) {
    const float shownWidth = width / view.zoom;
    const float shownHeight = height / view.zoom;
    const bool inView = static_cast<float>(box.x) >= view.left && static_cast<float>(box.y) >= view.top &&
                        static_cast<float>(box.x + box.width) <= view.left + shownWidth &&
                        static_cast<float>(box.y + box.height) <= view.top + shownHeight;
    if (inView) {
        return view;
    }
    const float centreX = static_cast<float>(box.x) + static_cast<float>(box.width) * 0.5f;
    const float centreY = static_cast<float>(box.y) + static_cast<float>(box.height) * 0.5f;
    return keptInside(ImageView{view.zoom, centreX - shownWidth * 0.5f, centreY - shownHeight * 0.5f}, width, height);
}

} // namespace observex
