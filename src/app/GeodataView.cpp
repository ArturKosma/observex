#include "app/GeodataView.h"

#include <algorithm>
#include <cfloat>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace observex {

namespace {

// A /loc answer whose height is this far from the geodata's floor there was read wrong (or the geodata is not this
// server's there): not taken.
constexpr int kFloorOff = 100;

ImU32 mix(ImU32 a, ImU32 b, float t) {
    const auto channel = [&](int shift) {
        const float from = static_cast<float>((a >> shift) & 0xFF);
        const float to = static_cast<float>((b >> shift) & 0xFF);
        return static_cast<ImU32>(std::lround(from + (to - from) * t)) << shift;
    };
    return channel(IM_COL32_R_SHIFT) | channel(IM_COL32_G_SHIFT) | channel(IM_COL32_B_SHIFT) |
           channel(IM_COL32_A_SHIFT);
}

// A line's colour by how high it is against the feet: level lime, lower cyan, higher orange (all the way at 120 units,
// about two and a half own heights).
ImU32 heightColour(float rise) {
    const float t = std::clamp(rise / 120.0f, -1.0f, 1.0f);
    return t < 0.0f ? mix(theme::kGround, theme::kGeoLow, -t) : mix(theme::kGround, theme::kGeoHigh, t);
}

} // namespace

GeodataView::~GeodataView() {
    if (building_.valid()) {
        building_.wait();
    }
}

void GeodataView::read(const GameStats& stats, double timeMs, std::vector<std::string>& log) {
    const WorldPosition::Doubt before = position_.spot().doubt;
    position_.minimap(stats.minimap);
    const LocationRead& location = stats.location;
    if (location.count != 0 && location.count != lastCount_) {
        lastCount_ = location.count;
        // A number read wrong puts the character elsewhere: the geodata's floor there must be at its height.
        const std::optional<GeoFloor> floor = geodata_.floorNear(location.x, location.y, location.z);
        char text[256];
        if (floor && std::abs(floor->z - location.z) > kFloorOff) {
            std::snprintf(text, sizeof(text),
                          "Location read as %d, %d, %d, but the geodata's floor there is at %d: not taken (read "
                          "wrong?)",
                          location.x, location.y, location.z, floor->z);
            log.emplace_back(text);
        } else {
            std::string line = position_.located(location);
            if (!floor) {
                line += " (no geodata here: " + Geodata::regionName(location.x, location.y) + " is not in " +
                        geodata_.folder().filename().string() + ")";
            }
            log.push_back(std::move(line));
        }
    }
    // Its height: the floor where it is now, nearest the one it had (a bridge, a cave).
    const WorldPosition::Spot& spot = position_.spot();
    if (spot.known && position_.sure()) {
        if (const std::optional<GeoFloor> floor = geodata_.floorNear(spot.x, spot.y, spot.z)) {
            position_.settle(floor->z);
        }
    }
    if (spot.doubt != shownDoubt_) {
        shownDoubt_ = spot.doubt;
        doubtSinceMs_ = timeMs;
        if (before == WorldPosition::Doubt::None && spot.doubt == WorldPosition::Doubt::Moved) {
            log.emplace_back("Where you are: you walked, and the minimap's scale is not known yet: one more /loc");
        } else if (before == WorldPosition::Doubt::None && spot.doubt == WorldPosition::Doubt::MinimapLost) {
            log.emplace_back("Where you are: the minimap lost track of you: /loc again");
        }
    }
    keepField();
}

void GeodataView::keepField() {
    if (building_.valid() && building_.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        field_ = building_.get();
    }
    const WorldPosition::Spot& spot = position_.spot();
    if (!spot.known || building_.valid() || (field_ && field_->covers(spot.x, spot.y, kFieldMargin))) {
        return;
    }
    building_ = std::async(std::launch::async, [this, x = spot.x, y = spot.y, z = spot.z]() {
        return GeoField::build(geodata_, x, y, z);
    });
}

void GeodataView::draw(ImDrawList* drawList, const theme::Fonts& fonts, ImVec2 min, ImVec2 max, float imageWidth,
                       float imageHeight, const GroundCamera& camera, std::span<const UiPiece> pieces, double timeMs,
                       float dpiScale) {
    keepField();
    const WorldPosition::Spot& spot = position_.spot();
    const float scale = (max.x - min.x) / imageWidth;
    const auto view = [&](float x, float y) { return ImVec2(min.x + x * scale, min.y + y * scale); };
    drawList->PushClipRect(min, max, true);
    if (spot.known && field_ && camera.known) {
        const double feet[3] = {spot.x, spot.y, spot.z};
        // On the camera with its pitch set right on the geodata's ground, the interface left clear.
        geoGrid(*field_, geoCamera(*field_, camera, feet, imageWidth, imageHeight), feet, imageWidth, imageHeight,
                pieces, grid_);
        const float strength = position_.sure() ? 1.0f : 0.4f; // Where it was last known: fainter.
        const float thickness = std::max(1.0f, 1.0f * dpiScale);
        for (const GeoGrid::Cell& cell : grid_.blocked) {
            drawList->AddQuadFilled(view(cell.x[0], cell.y[0]), view(cell.x[1], cell.y[1]),
                                    view(cell.x[2], cell.y[2]), view(cell.x[3], cell.y[3]),
                                    theme::faded(theme::kGeoBlocked, 0.35f * cell.strength * strength));
        }
        for (const GeoGrid::Line& line : grid_.lines) {
            const float alpha = (line.fine ? 0.8f : 0.6f) * line.strength * strength;
            drawList->AddLine(view(line.x0, line.y0), view(line.x1, line.y1),
                              theme::faded(heightColour(line.rise), alpha), thickness);
        }
        if (!grid_.feet.empty()) {
            std::vector<ImVec2> ring;
            for (std::size_t k = 0; k + 1 < grid_.feet.size(); k += 2) {
                ring.push_back(view(grid_.feet[k], grid_.feet[k + 1]));
            }
            drawList->AddPolyline(ring.data(), static_cast<int>(ring.size()), theme::faded(theme::kOwn, strength),
                                  ImDrawFlags_Closed, 2.0f * dpiScale);
        }
    }
    drawList->PopClipRect();

    // In the middle (a third down, over the character's head): what to do while it is not sure where it is, or that
    // there is no geodata there.
    const char* title = nullptr;
    std::string detail;
    if (spot.doubt != WorldPosition::Doubt::None && timeMs - doubtSinceMs_ >= kDoubtShowMs) {
        switch (spot.doubt) {
        case WorldPosition::Doubt::NoLocation:
            title = "Type /loc";
            detail = "observex does not know where you are yet";
            break;
        case WorldPosition::Doubt::Moved:
            title = "Type /loc";
            detail = "once more: the minimap's scale is learned from two";
            break;
        case WorldPosition::Doubt::MinimapLost:
            title = "Type /loc";
            detail = "the minimap lost track of you";
            break;
        case WorldPosition::Doubt::MinimapHidden:
            title = "Open the minimap";
            detail = "it follows you between /loc";
            break;
        case WorldPosition::Doubt::None:
            break;
        }
    } else if (spot.known && field_ && std::isnan(field_->ground(spot.x, spot.y))) {
        title = "No geodata here";
        detail = Geodata::regionName(spot.x, spot.y) + " is not in " + geodata_.folder().string();
    }
    if (title != nullptr) {
        const float titleSize = std::round(30.0f * dpiScale);
        const float detailSize = ImGui::GetFontSize();
        const ImVec2 titleText = fonts.medium->CalcTextSizeA(titleSize, FLT_MAX, 0.0f, title);
        const ImVec2 detailText = fonts.regular->CalcTextSizeA(detailSize, FLT_MAX, 0.0f, detail.c_str());
        const ImVec2 padding(std::round(18.0f * dpiScale), std::round(12.0f * dpiScale));
        const float gap = std::round(4.0f * dpiScale);
        const ImVec2 size(std::max(titleText.x, detailText.x) + 2.0f * padding.x,
                          titleText.y + gap + detailText.y + 2.0f * padding.y);
        const ImVec2 middle((min.x + max.x) * 0.5f, min.y + (max.y - min.y) * 0.3f);
        const ImVec2 corner(std::round(middle.x - size.x * 0.5f), std::round(middle.y - size.y * 0.5f));
        drawList->AddRectFilled(corner, ImVec2(corner.x + size.x, corner.y + size.y), theme::kOverlay,
                                8.0f * dpiScale);
        drawList->AddText(fonts.medium, titleSize,
                          ImVec2(std::round(middle.x - titleText.x * 0.5f), corner.y + padding.y), theme::kText, title);
        drawList->AddText(fonts.regular, detailSize,
                          ImVec2(std::round(middle.x - detailText.x * 0.5f), corner.y + padding.y + titleText.y + gap),
                          theme::kTextMuted, detail.c_str());
    }
}

} // namespace observex
