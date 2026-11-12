#include "app/Overlay.h"

#include "app/Review.h"
#include "core/Marks.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace observex {

void drawThings(ImDrawList* drawList, ImFont* font, ImVec2 min, ImVec2 max, float imageWidth,
                std::span<const Tracked> things, const std::vector<std::string>& kinds, bool showKinds,
                float dpiScale) {
    const Category& category = kCategories[0];
    showKinds = showKinds && !kinds.empty();
    // The "can't tell" kind, for a model that learnt it from the review's answers.
    const auto unknown = std::find(kinds.begin(), kinds.end(), kUnknownKind);
    const int unknownKind = unknown != kinds.end() ? static_cast<int>(unknown - kinds.begin()) : -1;
    const float scale = (max.x - min.x) / imageWidth;
    const float thickness = 1.5f * dpiScale;
    drawList->PushClipRect(min, max, true);
    for (auto i = things.size(); i-- > 0;) { // Surest last, so it is on top.
        const Detection& thing = things[i];
        const ImVec2 boxMin(std::round(min.x + thing.x * scale), std::round(min.y + thing.y * scale));
        const ImVec2 boxMax(std::round(min.x + (thing.x + thing.width) * scale),
                            std::round(min.y + (thing.y + thing.height) * scale));
        // One the review does not know by its word in model.json.
        const char* name = category.yes;
        const char* unsure = "";
        ImU32 color = theme::kAccent;
        if (showKinds && cantTellKind(thing, unknownKind)) {
            unsure = "?";
            color = theme::kUnknown;
        } else if (showKinds && thing.kind < kinds.size()) {
            name = kinds[thing.kind].c_str();
            for (const Kind& kind : category.kinds) {
                if (kind.id == kinds[thing.kind]) {
                    name = kind.name;
                    color = kind.color;
                }
            }
        }
        theme::drawBox(drawList, boxMin, boxMax, color, thickness);
        char tag[48];
        std::snprintf(tag, sizeof(tag), "%s%s %.0f%%", name, unsure, static_cast<double>(thing.sure * 100.0f));
        theme::drawTag(drawList, font, boxMin, boxMax, thickness, min, max, tag, color, theme::kBackground);
    }
    drawList->PopClipRect();
}

void drawUiPieces(ImDrawList* drawList, ImFont* font, ImVec2 min, ImVec2 max, float imageWidth,
                  std::span<const UiPiece> pieces, float dpiScale) {
    const float scale = (max.x - min.x) / imageWidth;
    const float thickness = 1.5f * dpiScale;
    const auto box = [&](const UiPiece& piece) {
        return theme::Area{ImVec2(std::round(min.x + static_cast<float>(piece.x0) * scale),
                                  std::round(min.y + static_cast<float>(piece.y0) * scale)),
                           ImVec2(std::round(min.x + static_cast<float>(piece.x1) * scale),
                                  std::round(min.y + static_cast<float>(piece.y1) * scale))};
    };
    drawList->PushClipRect(min, max, true);
    for (const UiPiece& piece : pieces) {
        const theme::Area area = box(piece);
        drawList->AddRectFilled(area.min, area.max, theme::faded(theme::kUi, 0.22f));
        theme::drawBox(drawList, area.min, area.max, theme::kUi, thickness);
    }
    // The tags after all the boxes, so none hides under a neighbour's fill.
    for (const UiPiece& piece : pieces) {
        const theme::Area area = box(piece);
        theme::drawTag(drawList, font, area.min, area.max, thickness, min, max, piece.name.data(), theme::kUi,
                       theme::kBackground);
    }
    drawList->PopClipRect();
}

void drawGround(ImDrawList* drawList, ImVec2 min, ImVec2 max, float imageWidth, const GroundCamera& camera,
                float dpiScale) {
    if (!camera.known) {
        return;
    }
    // Cells of kCell own heights (about 2.5 m) on the ground's map, out to kReach of the feet.
    constexpr float kCell = 1.5f;
    constexpr int kCells = 20; // Each way from the cell under the feet.
    constexpr float kReach = kCell * static_cast<float>(kCells);
    constexpr float kNearest = 0.3f; // Own heights ahead of the camera's eye: nearer is not drawn.
    const float strength = camera.pitchMeasured ? 1.0f : 0.5f;
    const float scale = (max.x - min.x) / imageWidth;
    const float thickness = 1.0f * dpiScale;

    // A map point on the screen as drawn, if well ahead of the camera.
    const auto onScreen = [&](float mapX, float mapY, ImVec2& at) {
        float right = 0.0f;
        float away = 0.0f;
        mapToGround(camera, mapX, mapY, right, away);
        if (aheadOfCamera(camera, right, away, 0.0f) < kNearest) {
            return false;
        }
        const GroundPoint point = groundToScreen(camera, right, away);
        at = ImVec2(min.x + point.x * scale, min.y + point.y * scale);
        return point.ahead;
    };
    // How strongly a map point shows: fading out towards kReach of the feet.
    const auto fade = [&](float mapX, float mapY) {
        const float closeness = 1.0f - std::hypot(mapX - camera.feetX, mapY - camera.feetY) / kReach;
        return closeness > 0.0f ? closeness * closeness * strength : 0.0f;
    };

    drawList->PushClipRect(min, max, true);
    const float firstX = (std::floor(camera.feetX / kCell) - static_cast<float>(kCells)) * kCell;
    const float firstY = (std::floor(camera.feetY / kCell) - static_cast<float>(kCells)) * kCell;
    for (int row = 0; row < 2 * kCells; ++row) {
        for (int column = 0; column < 2 * kCells; ++column) {
            const float x0 = firstX + static_cast<float>(column) * kCell;
            const float y0 = firstY + static_cast<float>(row) * kCell;
            const float shown = fade(x0 + kCell * 0.5f, y0 + kCell * 0.5f);
            if (shown <= 0.01f) {
                continue;
            }
            ImVec2 corners[4];
            if (!onScreen(x0, y0, corners[0]) || !onScreen(x0 + kCell, y0, corners[1]) ||
                !onScreen(x0 + kCell, y0 + kCell, corners[2]) || !onScreen(x0, y0 + kCell, corners[3])) {
                continue;
            }
            drawList->AddQuadFilled(corners[0], corners[1], corners[2], corners[3],
                                    theme::faded(theme::kGround, 0.22f * shown));
            // Its two edges nearer the map's start; the far ones are the next cells'.
            const ImU32 line = theme::faded(theme::kGround, 0.75f * shown);
            drawList->AddLine(corners[0], corners[1], line, thickness);
            drawList->AddLine(corners[0], corners[3], line, thickness);
        }
    }
    // A ring round the feet, half an own height out.
    constexpr int kRingPoints = 40;
    ImVec2 ring[kRingPoints];
    int ringCount = 0;
    for (int i = 0; i < kRingPoints; ++i) {
        const float angle = 2.0f * 3.14159265f * static_cast<float>(i) / kRingPoints;
        if (onScreen(camera.feetX + 0.5f * std::cos(angle), camera.feetY + 0.5f * std::sin(angle), ring[ringCount])) {
            ++ringCount;
        }
    }
    if (ringCount == kRingPoints) {
        drawList->AddPolyline(ring, ringCount, theme::faded(theme::kGround, strength), ImDrawFlags_Closed,
                              2.0f * dpiScale);
    }
    // The horizon, where the flat ground ends.
    const float horizon = min.y + horizonRow(camera) * scale;
    if (horizon > min.y && horizon < max.y) {
        drawList->AddLine(ImVec2(min.x, horizon), ImVec2(max.x, horizon), theme::faded(theme::kGround, 0.6f * strength),
                          thickness);
    }
    drawList->PopClipRect();
}

} // namespace observex
