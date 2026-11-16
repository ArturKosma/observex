#include "app/Overlay.h"

#include "app/Review.h"
#include "core/Marks.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

namespace observex {

namespace {

// " · 610" (to the nearest 10 game units: frame to frame a distance moves under 1%, which still flickered the last
// digit), "?" after it while the pitch is only presumed; nothing for none (`distances` empty or the one at `i` under 0).
void addDistance(char* tag, std::size_t size, std::span<const float> distances, std::size_t i, bool sure) {
    if (i >= distances.size() || distances[i] < 0.0f) {
        return;
    }
    const std::size_t used = std::strlen(tag);
    std::snprintf(tag + used, size - used, " \xC2\xB7 %.0f%s", std::round(distances[i] / 10.0f) * 10.0,
                  sure ? "" : "?");
}

} // namespace

void drawThings(ImDrawList* drawList, ImFont* font, ImVec2 min, ImVec2 max, float imageWidth,
                std::span<const Tracked> things, const std::vector<std::string>& kinds, bool showKinds,
                float dpiScale, std::span<const float> distances, bool distancesSure) {
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
        char tag[64];
        std::snprintf(tag, sizeof(tag), "%s%s %.0f%%", name, unsure, static_cast<double>(thing.sure * 100.0f));
        addDistance(tag, sizeof(tag), distances, i, distancesSure);
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

void drawLoot(ImDrawList* drawList, ImFont* font, ImVec2 min, ImVec2 max, float imageWidth,
              std::span<const Detection> drops, float dpiScale, std::span<const float> distances,
              bool distancesSure) {
    const float scale = (max.x - min.x) / imageWidth;
    const float thickness = 1.5f * dpiScale;
    const float gap = std::round(2.0f * dpiScale); // Between the drop and its corners.
    const float shortest = std::round(4.0f * dpiScale); // A corner's arm at least.
    drawList->PushClipRect(min, max, true);
    for (std::size_t i = 0; i < drops.size(); ++i) {
        const Detection& drop = drops[i];
        // Its four corners just outside it: a drop is a few pixels, and a box's lines over its edges hid it.
        const ImVec2 a(std::round(min.x + drop.x * scale) - gap, std::round(min.y + drop.y * scale) - gap);
        const ImVec2 b(std::round(min.x + (drop.x + drop.width) * scale) + gap,
                       std::round(min.y + (drop.y + drop.height) * scale) + gap);
        const float arm = std::max(shortest, std::round(0.3f * std::min(b.x - a.x, b.y - a.y)));
        const float armX = std::min(arm, (b.x - a.x) * 0.5f); // Never past the side's middle.
        const float armY = std::min(arm, (b.y - a.y) * 0.5f);
        for (const ImU32 color : {theme::kHalo, theme::kDrop}) { // A dark halo under the gold, as boxes have.
            const float width = color == theme::kDrop ? thickness : thickness + 2.0f * dpiScale;
            for (const ImVec2& at : {a, ImVec2(b.x, a.y), b, ImVec2(a.x, b.y)}) {
                const float x = at.x == a.x ? armX : -armX;
                const float y = at.y == a.y ? armY : -armY;
                const ImVec2 points[3] = {ImVec2(at.x + x, at.y), at, ImVec2(at.x, at.y + y)};
                drawList->AddPolyline(points, 3, color, ImDrawFlags_None, width);
            }
        }
        char tag[48];
        std::snprintf(tag, sizeof(tag), "Drop %.0f%%", static_cast<double>(drop.sure * 100.0f));
        addDistance(tag, sizeof(tag), distances, i, distancesSure);
        theme::drawTag(drawList, font, a, b, thickness, min, max, tag, theme::kDrop, theme::kBackground);
    }
    drawList->PopClipRect();
}

void drawGround(ImDrawList* drawList, ImVec2 min, ImVec2 max, float imageWidth, const GroundCamera& camera,
                const GroundHeights* heights, float dpiScale) {
    if (!camera.known) {
        return;
    }
    static std::vector<GridPoint> grid(kGridPoints); // The UI thread's alone.
    groundGrid(camera, heights, grid);
    const float strength = camera.pitchMeasured ? 1.0f : 0.5f;
    const float scale = (max.x - min.x) / imageWidth;
    const float thickness = 1.0f * dpiScale;
    const auto at = [&](int i, int j) -> const GridPoint& {
        return grid[static_cast<std::size_t>(j * kGridSide + i)];
    };
    const auto onScreen = [&](float x, float y) { return ImVec2(min.x + x * scale, min.y + y * scale); };

    drawList->PushClipRect(min, max, true);
    // The fill: each piece of a cell as two triangles, each corner as strong as it shows there (not at all where
    // something stands in front of it), not smoothed at the edges (pieces side by side would show their seams).
    const ImVec2 white = ImGui::GetFontTexUvWhitePixel();
    for (int j = 0; j + 1 < kGridSide; ++j) {
        for (int i = 0; i + 1 < kGridSide; ++i) {
            const GridPoint* corners[4] = {&at(i, j), &at(i + 1, j), &at(i + 1, j + 1), &at(i, j + 1)};
            bool ok = true;
            float shown = 0.0f;
            for (const GridPoint* corner : corners) {
                ok = ok && corner->ok;
                shown = std::max(shown, corner->hidden ? 0.0f : corner->shown);
            }
            if (!ok || shown <= 0.01f) {
                continue;
            }
            drawList->PrimReserve(6, 4);
            const auto first = static_cast<ImDrawIdx>(drawList->_VtxCurrentIdx);
            for (const int k : {0, 1, 2, 0, 2, 3}) {
                drawList->PrimWriteIdx(static_cast<ImDrawIdx>(first + k));
            }
            for (const GridPoint* corner : corners) {
                drawList->PrimWriteVtx(onScreen(corner->x, corner->y), white,
                                       theme::faded(theme::kGround, corner->hidden ? 0.0f : 0.22f * corner->shown));
            }
        }
    }
    // The cells' two edges nearer the map's start (the far ones are the next cells'), bent over the ground, cut where
    // something stands in front (each piece tested every few pixels: a trunk can be thinner than a piece).
    const auto edge = [&](int i, int j, int di, int dj) {
        float shown = 0.0f;
        for (int k = 0; k <= kGridSteps; ++k) {
            const GridPoint& point = at(i + k * di, j + k * dj);
            if (!point.ok) {
                return;
            }
            shown += point.shown / static_cast<float>(kGridSteps + 1);
        }
        if (shown <= 0.01f) {
            return;
        }
        const ImU32 color = theme::faded(theme::kGround, 0.75f * shown);
        ImVec2 run[kGridSteps * kGridMostSteps + 1];
        int count = 0;
        const auto flush = [&] {
            if (count >= 2) {
                drawList->AddPolyline(run, count, color, ImDrawFlags_None, thickness);
            }
            count = 0;
        };
        GridStep steps[kGridMostSteps + 1];
        for (int k = 0; k < kGridSteps; ++k) {
            const GridPoint& a = at(i + k * di, j + k * dj);
            const GridPoint& b = at(i + (k + 1) * di, j + (k + 1) * dj);
            int pieces = 1;
            if (heights != nullptr) {
                pieces = gridStepCount(a, b);
                gridSteps(*heights, a, b, pieces, steps);
            } else {
                steps[0] = {a.x, a.y, false};
                steps[1] = {b.x, b.y, false};
            }
            for (int step = k == 0 ? 0 : 1; step <= pieces; ++step) { // The piece before ended on its first.
                if (steps[step].hidden) {
                    flush();
                } else {
                    run[count++] = onScreen(steps[step].x, steps[step].y);
                }
            }
        }
        flush();
    };
    for (int row = 0; row < 2 * kGridCells; ++row) {
        for (int column = 0; column < 2 * kGridCells; ++column) {
            edge(column * kGridSteps, row * kGridSteps, 1, 0);
            edge(column * kGridSteps, row * kGridSteps, 0, 1);
        }
    }
    // A ring round the feet, half an own height out, on the ground there (cut where something stands in front: the own
    // character's legs).
    constexpr int kRingPoints = 40;
    ImVec2 ring[kRingPoints];
    bool ringHidden[kRingPoints] = {};
    int ringCount = 0;
    int ringHiddenCount = 0;
    for (int i = 0; i < kRingPoints; ++i) {
        const float angle = 2.0f * 3.14159265f * static_cast<float>(i) / kRingPoints;
        const float mapX = camera.feetX + 0.5f * std::cos(angle);
        const float mapY = camera.feetY + 0.5f * std::sin(angle);
        float right = 0.0f;
        float away = 0.0f;
        mapToGround(camera, mapX, mapY, right, away);
        const float up = heights != nullptr ? groundHeight(*heights, camera, mapX, mapY) : 0.0f;
        if (aheadOfCamera(camera, right, away, up) < 0.3f) {
            continue;
        }
        if (const GroundPoint point = worldToScreen(camera, right, away, up); point.ahead) {
            const bool hidden =
                heights != nullptr && groundHidden(*heights, mapX, mapY, heightOnMap(*heights, mapX, mapY));
            ringHidden[ringCount] = hidden;
            ringHiddenCount += hidden ? 1 : 0;
            ring[ringCount++] = ImVec2(min.x + point.x * scale, min.y + point.y * scale);
        }
    }
    const ImU32 ringColor = theme::faded(theme::kGround, strength);
    if (ringCount == kRingPoints && ringHiddenCount == 0) {
        drawList->AddPolyline(ring, ringCount, ringColor, ImDrawFlags_Closed, 2.0f * dpiScale);
    } else if (ringCount == kRingPoints && ringHiddenCount < kRingPoints) {
        // From the first point after a hidden one, round once: each stretch shown on its own.
        int first = 0;
        while (!ringHidden[first]) {
            ++first;
        }
        ImVec2 stretch[kRingPoints];
        int count = 0;
        for (int k = 1; k <= kRingPoints; ++k) {
            const int i = (first + k) % kRingPoints;
            if (!ringHidden[i]) {
                stretch[count++] = ring[i];
            }
            if ((ringHidden[i] || k == kRingPoints) && count > 0) {
                if (count >= 2) {
                    drawList->AddPolyline(stretch, count, ringColor, ImDrawFlags_None, 2.0f * dpiScale);
                }
                count = 0;
            }
        }
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
