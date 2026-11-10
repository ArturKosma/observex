#include "app/Widgets.h"

#include <algorithm>
#include <cfloat>
#include <cmath>

namespace observex {

namespace {

struct Layout {
    float dpi;
    float height;
    float padding;
    float iconWidth;
    float gap;
    float arrowWidth;
    float stroke;
};

Layout layout() {
    const float dpi = ImGui::GetStyle().FontScaleDpi;
    return {dpi,
            std::round(BarButton::kHeight * dpi),
            std::round(12.0f * dpi),
            std::round(16.0f * dpi),
            std::round(9.0f * dpi),
            std::round(9.0f * dpi),
            std::round(1.5f * dpi * 2.0f) * 0.5f};
}

float textWidth(ImFont* font, const char* text) {
    return text != nullptr ? font->CalcTextSizeA(ImGui::GetFontSize(), FLT_MAX, 0.0f, text).x : 0.0f;
}

void drawIcon(ImDrawList* drawList, Icon icon, ImVec2 centre, ImU32 color, const Layout& l) {
    const float dpi = l.dpi;
    switch (icon) {
    case Icon::Monitor: {
        const float half = l.iconWidth * 0.5f;
        const float top = centre.y - std::round(7.0f * dpi);
        const float bottom = centre.y + std::round(4.0f * dpi);
        const float base = centre.y + std::round(7.0f * dpi);
        drawList->AddRect(ImVec2(centre.x - half, top), ImVec2(centre.x + half, bottom), color, 2.0f * dpi, 0,
                          l.stroke);
        drawList->AddLine(ImVec2(centre.x, bottom), ImVec2(centre.x, base), color, l.stroke);
        drawList->AddLine(ImVec2(centre.x - std::round(4.0f * dpi), base),
                          ImVec2(centre.x + std::round(4.0f * dpi), base), color, l.stroke);
        break;
    }
    case Icon::Record:
        drawList->AddCircleFilled(centre, 5.0f * dpi, color);
        break;
    case Icon::Stop: {
        const float half = std::round(4.5f * dpi);
        drawList->AddRectFilled(ImVec2(centre.x - half, centre.y - half), ImVec2(centre.x + half, centre.y + half),
                                color, 1.5f * dpi);
        break;
    }
    case Icon::Gear: {
        // Eight teeth around a ring, drawn as one outline, with a hole in the middle.
        constexpr int kTeeth = 8;
        constexpr float kPi = 3.14159265f;
        const float outer = 8.0f * dpi;
        const float inner = 5.9f * dpi;
        const float step = 2.0f * kPi / kTeeth;
        for (int i = 0; i < kTeeth; ++i) {
            const float angle = static_cast<float>(i) * step;
            const float corners[4][2] = {{inner, angle - step * 0.30f},
                                         {outer, angle - step * 0.17f},
                                         {outer, angle + step * 0.17f},
                                         {inner, angle + step * 0.30f}};
            for (const auto& [radius, a] : corners) {
                drawList->PathLineTo(ImVec2(centre.x + std::cos(a) * radius, centre.y + std::sin(a) * radius));
            }
        }
        drawList->PathStroke(color, ImDrawFlags_Closed, l.stroke);
        drawList->AddCircle(centre, 2.4f * dpi, color, 0, l.stroke);
        break;
    }
    case Icon::Picture: {
        const float halfWidth = l.iconWidth * 0.5f;
        const float halfHeight = std::round(6.0f * dpi);
        const float left = centre.x - halfWidth;
        const float bottom = centre.y + halfHeight;
        drawList->AddRect(ImVec2(left, centre.y - halfHeight), ImVec2(centre.x + halfWidth, bottom), color, 2.0f * dpi,
                          0, l.stroke);
        // A hill from the bottom left, and a sun at the top right.
        drawList->PathLineTo(ImVec2(left + 1.5f * dpi, bottom - 1.5f * dpi));
        drawList->PathLineTo(ImVec2(centre.x - 2.0f * dpi, centre.y - 1.0f * dpi));
        drawList->PathLineTo(ImVec2(centre.x + 1.0f * dpi, centre.y + 2.0f * dpi));
        drawList->PathLineTo(ImVec2(centre.x + 3.0f * dpi, centre.y + 0.5f * dpi));
        drawList->PathLineTo(ImVec2(centre.x + halfWidth - 1.5f * dpi, bottom - 1.5f * dpi));
        drawList->PathStroke(color, 0, l.stroke);
        drawList->AddCircleFilled(ImVec2(centre.x + 3.5f * dpi, centre.y - 2.5f * dpi), 1.5f * dpi, color);
        break;
    }
    case Icon::Close: {
        const float half = std::round(5.0f * dpi);
        drawList->AddLine(ImVec2(centre.x - half, centre.y - half), ImVec2(centre.x + half, centre.y + half), color,
                          l.stroke);
        drawList->AddLine(ImVec2(centre.x - half, centre.y + half), ImVec2(centre.x + half, centre.y - half), color,
                          l.stroke);
        break;
    }
    case Icon::Back:
    case Icon::Forward: {
        const float half = std::round(5.0f * dpi);
        const float tip = (icon == Icon::Back ? -1.0f : 1.0f) * std::round(2.5f * dpi);
        drawList->PathLineTo(ImVec2(centre.x - tip, centre.y - half));
        drawList->PathLineTo(ImVec2(centre.x + tip, centre.y));
        drawList->PathLineTo(ImVec2(centre.x - tip, centre.y + half));
        drawList->PathStroke(color, 0, l.stroke);
        break;
    }
    case Icon::Eye: {
        // Two lids from corner to corner, drawn as one outline, and a pupil.
        constexpr int kSteps = 12;
        constexpr float kPi = 3.14159265f;
        const float half = l.iconWidth * 0.5f;
        const float open = 5.5f * dpi;
        for (int i = 0; i < kSteps; ++i) { // Upper lid, left to right.
            const float t = static_cast<float>(i) / kSteps;
            drawList->PathLineTo(ImVec2(centre.x - half + 2.0f * half * t, centre.y - open * std::sin(t * kPi)));
        }
        for (int i = 0; i < kSteps; ++i) { // Lower lid, right to left.
            const float t = static_cast<float>(i) / kSteps;
            drawList->PathLineTo(ImVec2(centre.x + half - 2.0f * half * t, centre.y + open * std::sin(t * kPi)));
        }
        drawList->PathStroke(color, ImDrawFlags_Closed, l.stroke);
        drawList->AddCircleFilled(centre, 2.6f * dpi, color);
        break;
    }
    case Icon::None:
        break;
    }
}

} // namespace

float BarButton::width(const theme::Fonts& fonts) const {
    const Layout l = layout();
    if (caption == nullptr && text == nullptr && detail == nullptr && !arrow) {
        return l.height; // Icon only: square.
    }
    float width = l.padding * 2.0f;
    if (icon != Icon::None) {
        width += l.iconWidth + l.gap;
    }
    if (caption != nullptr) {
        width += textWidth(ImGui::GetFont(), caption) + std::round(5.0f * l.dpi);
    }
    width += textWidth(fonts.medium, text);
    if (detail != nullptr) {
        width += std::round(10.0f * l.dpi) + textWidth(ImGui::GetFont(), detail);
    }
    if (arrow) {
        width += l.gap + l.arrowWidth;
    }
    return std::round(width);
}

bool BarButton::draw(const char* id, const theme::Fonts& fonts) const {
    const Layout l = layout();
    const ImVec2 min = ImGui::GetCursorScreenPos();
    const bool clicked = ImGui::InvisibleButton(id, ImVec2(width(fonts), l.height)) && enabled;
    const bool hovered = enabled && ImGui::IsItemHovered();
    const bool held = enabled && ImGui::IsItemActive();
    if (hovered) {
        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    }
    const ImVec2 max = ImGui::GetItemRectMax();
    const bool lit = hovered || open;

    ImDrawList* drawList = ImGui::GetWindowDrawList();
    const float rounding = 8.0f * l.dpi;
    const ImU32 fill = held || open ? theme::kSurfaceActive : hovered ? theme::kSurfaceHover : theme::kSurface;
    const ImU32 border = open ? highlight : hovered ? theme::kBorderHover : theme::kBorder;
    drawList->AddRectFilled(min, max, fill, rounding);
    drawList->AddRect(min, max, border, rounding, 0, 1.0f);
    if (progress >= 0.0f) {
        const float left = min.x + rounding;
        const float bottom = max.y - std::round(4.0f * l.dpi);
        const float thickness = std::round(3.0f * l.dpi);
        drawList->AddRectFilled(ImVec2(left, bottom - thickness), ImVec2(max.x - rounding, bottom), theme::kBorderHover);
        drawList->AddRectFilled(ImVec2(left, bottom - thickness),
                                ImVec2(left + (max.x - rounding - left) * std::min(progress, 1.0f), bottom), highlight);
    }

    const float middle = std::round((min.y + max.y) * 0.5f);
    const ImU32 textColor = enabled ? theme::kText : theme::kTextMuted;
    const bool red = icon == Icon::Record || icon == Icon::Stop;
    const ImU32 iconColor = !enabled ? theme::kBorderHover
                            : red    ? theme::kRecord
                            : lit    ? theme::kText
                                     : theme::kTextMuted;

    if (caption == nullptr && text == nullptr && detail == nullptr && !arrow) {
        drawIcon(drawList, icon, ImVec2(std::round((min.x + max.x) * 0.5f), middle), iconColor, l);
        return clicked;
    }

    float x = min.x + l.padding;
    if (icon != Icon::None) {
        drawIcon(drawList, icon, ImVec2(x + l.iconWidth * 0.5f, middle), iconColor, l);
        x += l.iconWidth + l.gap;
    }
    const float fontSize = ImGui::GetFontSize();
    const float textY = std::round(middle - fontSize * 0.5f);
    if (caption != nullptr) {
        drawList->AddText(ImGui::GetFont(), fontSize, ImVec2(x, textY), theme::kTextMuted, caption);
        x += textWidth(ImGui::GetFont(), caption) + std::round(5.0f * l.dpi);
    }
    if (text != nullptr) {
        drawList->AddText(fonts.medium, fontSize, ImVec2(x, textY), textColor, text);
        x += textWidth(fonts.medium, text);
    }
    if (detail != nullptr) {
        x += std::round(10.0f * l.dpi);
        drawList->AddText(ImGui::GetFont(), fontSize, ImVec2(x, textY), theme::kTextMuted, detail);
        x += textWidth(ImGui::GetFont(), detail);
    }
    if (arrow) {
        x += l.gap;
        const float tip = std::round(l.arrowWidth * 0.5f) * (open ? -1.0f : 1.0f);
        drawList->PathLineTo(ImVec2(x, middle - tip * 0.5f));
        drawList->PathLineTo(ImVec2(x + l.arrowWidth * 0.5f, middle + tip * 0.5f));
        drawList->PathLineTo(ImVec2(x + l.arrowWidth, middle - tip * 0.5f));
        drawList->PathStroke(lit ? theme::kText : theme::kTextMuted, 0, l.stroke);
    }
    return clicked;
}

} // namespace observex
