#include "app/Theme.h"

#include "app/Resources.h"

#include <windows.h>

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <stdexcept>

namespace observex::theme {

namespace {

constexpr float kFontSize = 15.0f;

ImFont* loadFont(int resource) {
    const HRSRC info = FindResourceW(nullptr, MAKEINTRESOURCEW(resource), RT_RCDATA);
    const HGLOBAL handle = info != nullptr ? LoadResource(nullptr, info) : nullptr;
    void* data = handle != nullptr ? LockResource(handle) : nullptr;
    if (data == nullptr) {
        throw std::runtime_error("A font is missing from observex.exe");
    }
    ImFontConfig config;
    config.FontDataOwnedByAtlas = false; // The data stays in the loaded exe.
    return ImGui::GetIO().Fonts->AddFontFromMemoryTTF(data, static_cast<int>(SizeofResource(nullptr, info)), 0.0f,
                                                      &config);
}

ImVec4 color(ImU32 value, float alpha = 1.0f) {
    ImVec4 result = ImGui::ColorConvertU32ToFloat4(value);
    result.w *= alpha;
    return result;
}

} // namespace

Fonts apply(float dpiScale) {
    Fonts fonts;
    fonts.regular = loadFont(IDR_FONT_REGULAR); // The first font loaded is the default.
    fonts.medium = loadFont(IDR_FONT_MEDIUM);

    ImGuiStyle& style = ImGui::GetStyle();
    style = ImGuiStyle();
    style.FontSizeBase = kFontSize;
    style.WindowPadding = ImVec2(16, 16);
    style.FramePadding = ImVec2(12, 7);
    style.ItemSpacing = ImVec2(10, 8);
    style.ItemInnerSpacing = ImVec2(6, 6);
    style.WindowRounding = 0;
    style.ChildRounding = 8;
    style.FrameRounding = 6;
    style.PopupRounding = 10;
    style.ScrollbarRounding = 8;
    style.GrabRounding = 6;
    style.TabRounding = 6;
    style.WindowBorderSize = 0;
    style.ChildBorderSize = 1;
    style.PopupBorderSize = 1;
    style.FrameBorderSize = 1;
    style.ScrollbarSize = 10;
    style.ScaleAllSizes(dpiScale);
    style.FontScaleDpi = dpiScale;

    ImVec4* colors = style.Colors;
    for (int i = 0; i < ImGuiCol_COUNT; ++i) {
        colors[i] = color(kSurfaceHover); // Anything not set below still fits the palette.
    }
    colors[ImGuiCol_Text] = color(kText);
    colors[ImGuiCol_TextDisabled] = color(kTextMuted);
    colors[ImGuiCol_WindowBg] = color(kBackground);
    colors[ImGuiCol_ChildBg] = color(kBackground, 0.0f);
    colors[ImGuiCol_PopupBg] = color(kSurface);
    colors[ImGuiCol_Border] = color(kBorder);
    colors[ImGuiCol_BorderShadow] = color(kBackground, 0.0f);
    colors[ImGuiCol_FrameBg] = color(kSurface);
    colors[ImGuiCol_FrameBgHovered] = color(kSurfaceHover);
    colors[ImGuiCol_FrameBgActive] = color(kSurfaceActive);
    colors[ImGuiCol_TitleBg] = color(kBackground);
    colors[ImGuiCol_TitleBgActive] = color(kBackground);
    colors[ImGuiCol_TitleBgCollapsed] = color(kBackground);
    colors[ImGuiCol_MenuBarBg] = color(kBackground);
    colors[ImGuiCol_ScrollbarBg] = color(kBackground, 0.0f);
    colors[ImGuiCol_ScrollbarGrab] = color(kSurfaceActive);
    colors[ImGuiCol_ScrollbarGrabHovered] = color(kTextMuted, 0.5f);
    colors[ImGuiCol_ScrollbarGrabActive] = color(kTextMuted);
    colors[ImGuiCol_CheckMark] = color(kAccent);
    colors[ImGuiCol_SliderGrab] = color(kAccent);
    colors[ImGuiCol_SliderGrabActive] = color(kAccent);
    colors[ImGuiCol_Button] = color(kSurfaceHover);
    colors[ImGuiCol_ButtonHovered] = color(kSurfaceActive);
    colors[ImGuiCol_ButtonActive] = color(kBorderHover);
    colors[ImGuiCol_Header] = color(kSurfaceActive);
    colors[ImGuiCol_HeaderHovered] = color(kSurfaceHover);
    colors[ImGuiCol_HeaderActive] = color(kSurfaceActive);
    colors[ImGuiCol_Separator] = color(kBorder);
    colors[ImGuiCol_ResizeGrip] = color(kBackground, 0.0f);
    colors[ImGuiCol_InputTextCursor] = color(kText);
    colors[ImGuiCol_TextLink] = color(kAccent);
    colors[ImGuiCol_TextSelectedBg] = color(kAccent, 0.35f);
    colors[ImGuiCol_NavCursor] = color(kAccent);
    colors[ImGuiCol_ModalWindowDimBg] = color(kBackground, 0.6f);
    colors[ImGuiCol_NavWindowingDimBg] = color(kBackground, 0.6f);
    return fonts;
}

void drawLabel(ImDrawList* drawList, ImVec2 anchor, ImVec2 pivot, const char* text, const char* detail) {
    const float dpi = ImGui::GetStyle().FontScaleDpi;
    const ImVec2 padding(8.0f * dpi, 4.0f * dpi);
    const float lineHeight = ImGui::GetTextLineHeight();
    const float lineGap = std::round(2.0f * dpi);
    ImVec2 textSize = ImGui::CalcTextSize(text);
    if (detail != nullptr) {
        textSize.x = std::max(textSize.x, ImGui::CalcTextSize(detail).x);
        textSize.y += lineGap + lineHeight;
    }
    const ImVec2 size(textSize.x + padding.x * 2, textSize.y + padding.y * 2);
    const ImVec2 min(std::round(anchor.x - size.x * pivot.x), std::round(anchor.y - size.y * pivot.y));
    drawList->AddRectFilled(min, ImVec2(min.x + size.x, min.y + size.y), kOverlay, 5.0f * dpi);
    drawList->AddText(ImVec2(min.x + padding.x, min.y + padding.y), kText, text);
    if (detail != nullptr) {
        drawList->AddText(ImVec2(min.x + padding.x, min.y + padding.y + lineHeight + lineGap), kTextMuted, detail);
    }
}

void drawBox(ImDrawList* drawList, ImVec2 min, ImVec2 max, ImU32 color, float thickness) {
    const float dpi = ImGui::GetStyle().FontScaleDpi;
    const float opacity = static_cast<float>(color >> IM_COL32_A_SHIFT) / 255.0f;
    drawList->AddRect(min, max, faded(kHalo, opacity), 0.0f, 0, thickness + 2.0f * dpi);
    drawList->AddRect(min, max, color, 0.0f, 0, thickness);
}

namespace {

// A tag's text size, and its padding around the text.
float tagTextSize() {
    return std::round(ImGui::GetFontSize() * 0.87f);
}

ImVec2 tagPadding() {
    const float dpi = ImGui::GetStyle().FontScaleDpi;
    return ImVec2(std::round(5.0f * dpi), std::round(2.0f * dpi));
}

} // namespace

Area drawTag(ImDrawList* drawList, ImFont* font, ImVec2 boxMin, ImVec2 boxMax, float thickness, ImVec2 areaMin,
             ImVec2 areaMax, const char* text, ImU32 fill, ImU32 textColor) {
    const Area tag = placeTag(font, boxMin, boxMax, thickness, areaMin, areaMax, text);
    drawTagAt(drawList, font, tag, text, fill, textColor);
    return tag;
}

void drawTagAt(ImDrawList* drawList, ImFont* font, Area tag, const char* text, ImU32 fill, ImU32 textColor) {
    const ImVec2 padding = tagPadding();
    drawList->AddRectFilled(tag.min, tag.max, fill, 3.0f * ImGui::GetStyle().FontScaleDpi);
    drawList->AddText(font, tagTextSize(), ImVec2(tag.min.x + padding.x, tag.min.y + padding.y), textColor, text);
}

Area placeTag(ImFont* font, ImVec2 boxMin, ImVec2 boxMax, float thickness, ImVec2 areaMin, ImVec2 areaMax,
              const char* text) {
    const ImVec2 padding = tagPadding();
    const ImVec2 textSize = font->CalcTextSizeA(tagTextSize(), FLT_MAX, 0.0f, text);
    const ImVec2 tagSize(textSize.x + padding.x * 2.0f, textSize.y + padding.y * 2.0f);
    const float half = thickness * 0.5f;
    ImVec2 min(boxMin.x - half, boxMin.y - half - tagSize.y);
    if (min.y < areaMin.y) {
        min.y = boxMax.y + half; // No room above: below the box.
        if (min.y + tagSize.y > areaMax.y) {
            min.y = boxMin.y + half; // Nor below: inside it.
        }
    }
    min.x = std::max(std::min(min.x, areaMax.x - tagSize.x), areaMin.x);
    min = ImVec2(std::round(min.x), std::round(min.y));
    return {min, ImVec2(min.x + tagSize.x, min.y + tagSize.y)};
}

void drawLogo(ImDrawList* drawList, ImVec2 centre, float size) {
    // Proportions match tools/make_logo.py.
    const float ring = 0.25f * size;
    const float pupilOffset = 0.055f * size;
    drawList->AddCircle(centre, ring, kText, 0, 0.09f * size);
    drawList->AddCircleFilled(ImVec2(centre.x + pupilOffset, centre.y - pupilOffset), 0.11f * size, kAccent);
}

} // namespace observex::theme
