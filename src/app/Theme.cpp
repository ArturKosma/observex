#include "app/Theme.h"

#include "app/Resources.h"

#include <windows.h>

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
    style.FrameBorderSize = 0;
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
    colors[ImGuiCol_Button] = color(kSurface, 0.0f);
    colors[ImGuiCol_ButtonHovered] = color(kSurfaceHover);
    colors[ImGuiCol_ButtonActive] = color(kSurfaceActive);
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

void drawLabel(ImDrawList* drawList, ImVec2 anchor, ImVec2 pivot, const char* text) {
    const float dpi = ImGui::GetStyle().FontScaleDpi;
    const ImVec2 padding(8.0f * dpi, 4.0f * dpi);
    const ImVec2 textSize = ImGui::CalcTextSize(text);
    const ImVec2 size(textSize.x + padding.x * 2, textSize.y + padding.y * 2);
    const ImVec2 min(std::round(anchor.x - size.x * pivot.x), std::round(anchor.y - size.y * pivot.y));
    drawList->AddRectFilled(min, ImVec2(min.x + size.x, min.y + size.y), kOverlay, 5.0f * dpi);
    drawList->AddText(ImVec2(min.x + padding.x, min.y + padding.y), kText, text);
}

void drawLogo(ImDrawList* drawList, ImVec2 centre, float size) {
    // Proportions match tools/make_logo.py.
    const float ring = 0.25f * size;
    const float pupilOffset = 0.055f * size;
    drawList->AddCircle(centre, ring, kText, 0, 0.09f * size);
    drawList->AddCircleFilled(ImVec2(centre.x + pupilOffset, centre.y - pupilOffset), 0.11f * size, kAccent);
}

} // namespace observex::theme
