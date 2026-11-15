#include "core/WorldPosition.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace observex {

std::string WorldPosition::located(const LocationRead& read) {
    char text[256];
    std::snprintf(text, sizeof(text), "Location %d, %d, %d", read.x, read.y, read.z);
    std::string line = text;
    // From the answer before, followed by the minimap in one run: how far off it said, and the scale.
    if (anchor_.count != 0 && anchor_.map.seen && read.map.seen && anchor_.map.run == read.map.run) {
        const double slideX = read.map.x - anchor_.map.x;
        const double slideY = read.map.y - anchor_.map.y;
        const double slid = std::hypot(slideX, slideY);
        const double walkX = read.x - anchor_.x;
        const double walkY = read.y - anchor_.y;
        const double walked = std::hypot(walkX, walkY);
        if (scale_ > 0.0f && spot_.doubt == Doubt::None) {
            const double off = std::hypot(spot_.x - read.x, spot_.y - read.y);
            std::snprintf(text, sizeof(text), " (the minimap said %.0f units off, over %.0f walked)", off, walked);
            line += text;
        }
        if (slid >= kLearnPixels && walked >= kLearnWalk) {
            // North-up: walking east slides the map west, walking south (y up) slides it north (up the screen).
            const double along = (walkX * -slideX + walkY * -slideY) / (walked * slid);
            if (along >= kLearnAngle) {
                const auto measured = static_cast<float>(walked / slid);
                const auto pixels = static_cast<float>(slid);
                const float before = scale_;
                scale_ = weight_ > 0.0f ? (scale_ * weight_ + measured * pixels) / (weight_ + pixels) : measured;
                weight_ = std::min(weight_ + pixels, 2000.0f);
                std::snprintf(text, sizeof(text), "; minimap scale %.2f units a pixel (this walk %.2f%s)",
                              static_cast<double>(scale_), static_cast<double>(measured),
                              before > 0.0f ? "" : ", learned");
                line += text;
            } else {
                std::snprintf(text, sizeof(text), "; the minimap slid another way than the walk (%.0f degrees): no "
                                                  "scale from it",
                              std::acos(std::clamp(along, -1.0, 1.0)) * 57.29578);
                line += text;
            }
        }
    }
    anchor_ = read;
    spot_.known = true;
    spot_.x = read.x;
    spot_.y = read.y;
    spot_.z = read.z;
    spot_.doubt = read.map.seen ? Doubt::None : Doubt::MinimapHidden;
    return line;
}

void WorldPosition::minimap(const MinimapRead& map) {
    if (anchor_.count == 0) {
        spot_.doubt = Doubt::NoLocation;
        return;
    }
    if (!map.seen) {
        // Not followed on this image: where it was stays, in doubt (a new run, if the minimap is lost for good).
        spot_.doubt = map.run == anchor_.map.run && anchor_.map.seen ? Doubt::MinimapHidden : Doubt::MinimapLost;
        return;
    }
    if (!anchor_.map.seen || map.run != anchor_.map.run) {
        spot_.doubt = Doubt::MinimapLost;
        return;
    }
    const double slideX = map.x - anchor_.map.x;
    const double slideY = map.y - anchor_.map.y;
    if (scale_ <= 0.0f) {
        spot_.doubt = std::hypot(slideX, slideY) <= kStillPixels ? Doubt::None : Doubt::Moved;
        return;
    }
    spot_.x = anchor_.x - scale_ * slideX;
    spot_.y = anchor_.y - scale_ * slideY;
    spot_.doubt = Doubt::None;
}

} // namespace observex
