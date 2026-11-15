#include "app/ScreenPicker.h"

#include "app/Theme.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <exception>
#include <utility>

namespace observex {

namespace {
constexpr float kNoneTile = 116.0f; // The None tile's width with its margins, at 100%.
constexpr float kNoneGap = 10.0f;   // Added to the usual gap between it and the screens: it is not a screen.
} // namespace

ScreenPicker::ScreenPicker(ID3D11Device* device, IDXGIAdapter1* gpu) : device_(device), gpu_(gpu) {}

void ScreenPicker::setObserved(const std::string& name, const ScreenImage* image) {
    if (name != observed_) {
        close(); // Reopen without capturing the newly observed screen.
        observed_ = name;
    }
    observedImage_ = image;
}

void ScreenPicker::open() {
    miniatures_.clear();
    for (Screen& screen : listScreens(gpu_.Get())) {
        Miniature miniature;
        miniature.screen = std::move(screen);
        if (miniature.screen.name != observed_) {
            try {
                miniature.capture = std::make_unique<ScreenCapture>(device_.Get(), miniature.screen.output.Get());
                miniature.image = std::make_unique<ScreenImage>(device_.Get(), miniature.capture->width(),
                                                                miniature.capture->height());
            } catch (const std::exception&) {
                miniature.capture.reset(); // Drawn as an empty tile, e.g. while a UAC prompt is showing.
            }
        }
        miniatures_.push_back(std::move(miniature));
    }
    open_ = true;
}

void ScreenPicker::close() {
    miniatures_.clear();
    open_ = false;
}

void ScreenPicker::endFrame() {
    if (open_ && !drawn_) {
        close();
    }
    drawn_ = false;
}

void ScreenPicker::refresh(Miniature& miniature) {
    if (!miniature.capture) {
        return;
    }
    ScreenCapture& capture = *miniature.capture;
    ScreenCapture::Frame frame;
    switch (capture.acquire(0, frame)) {
    case ScreenCapture::Status::NewImage:
        if (capture.width() == miniature.image->width() && capture.height() == miniature.image->height()) {
            miniature.image->update(frame.texture);
        }
        break;
    case ScreenCapture::Status::Lost:
        if (capture.reopen() &&
            (capture.width() != miniature.image->width() || capture.height() != miniature.image->height())) {
            try {
                miniature.image = std::make_unique<ScreenImage>(device_.Get(), capture.width(), capture.height());
            } catch (const std::exception&) {
                miniature.capture.reset();
            }
        }
        break;
    case ScreenCapture::Status::Failed:
        miniature.capture.reset();
        break;
    default:
        break;
    }
}

RECT ScreenPicker::desktopArea() const {
    RECT area{};
    for (std::size_t i = 0; i < miniatures_.size(); ++i) {
        const RECT& bounds = miniatures_[i].screen.bounds;
        if (i == 0) {
            area = bounds;
        }
        area.left = std::min(area.left, bounds.left);
        area.top = std::min(area.top, bounds.top);
        area.right = std::max(area.right, bounds.right);
        area.bottom = std::max(area.bottom, bounds.bottom);
    }
    return area;
}

float ScreenPicker::noneWidth() const {
    const float dpi = ImGui::GetStyle().FontScaleDpi;
    return observed_.empty() ? 0.0f : std::round(kNoneTile * dpi) + std::round(kNoneGap * dpi);
}

ImVec2 ScreenPicker::fit(ImVec2 maxSize) {
    if (!open_) {
        open();
    }
    const RECT area = desktopArea();
    if (area.right <= area.left || area.bottom <= area.top) {
        return ImVec2(maxSize.x, ImGui::GetFrameHeight()); // Room for the "no screens" text.
    }
    const float none = noneWidth();
    const float width = static_cast<float>(area.right - area.left);
    const float height = static_cast<float>(area.bottom - area.top);
    const float scale = std::min(std::max(maxSize.x - none, 1.0f) / width, maxSize.y / height);
    return ImVec2(none + width * scale, height * scale);
}

bool ScreenPicker::drawNone(ImVec2 min, ImVec2 max, float rounding) const {
    // An empty, dark screen with the "none" sign (a circle struck through), named like the screens.
    const float dpi = ImGui::GetStyle().FontScaleDpi;
    ImGui::SetCursorScreenPos(min);
    const bool clicked = ImGui::InvisibleButton("none", ImVec2(max.x - min.x, max.y - min.y));
    const bool hovered = ImGui::IsItemHovered();
    if (hovered) {
        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    }

    ImDrawList* drawList = ImGui::GetWindowDrawList();
    drawList->AddRectFilled(min, max, theme::kBackground, rounding);
    const float outline = (hovered ? 2.0f : 1.0f) * dpi;
    const float half = outline * 0.5f;
    drawList->PathRect(ImVec2(min.x - half, min.y - half), ImVec2(max.x + half, max.y + half), rounding + half);
    drawList->PathStroke(hovered ? theme::kAccent : theme::kBorder, ImDrawFlags_Closed, outline);

    const ImVec2 centre(std::round((min.x + max.x) * 0.5f), std::round((min.y + max.y) * 0.5f));
    const float radius = std::round(13.0f * dpi);
    const float stroke = 2.0f * dpi;
    const float reach = radius * 0.7071f;
    const ImU32 color = hovered ? theme::kAccent : theme::kTextMuted;
    drawList->AddCircle(centre, radius, color, 0, stroke);
    drawList->AddLine(ImVec2(centre.x - reach, centre.y + reach), ImVec2(centre.x + reach, centre.y - reach), color,
                      stroke);

    drawList->PushClipRect(min, max, true);
    theme::drawLabel(drawList, ImVec2(min.x + 6.0f * dpi, max.y - 6.0f * dpi), ImVec2(0.0f, 1.0f), "None");
    drawList->PopClipRect();
    return clicked;
}

ScreenPicker::Choice ScreenPicker::draw(ImVec2 maxSize, ImU32 background) {
    const ImVec2 size = fit(maxSize);
    drawn_ = true;
    if (miniatures_.empty()) {
        ImGui::TextDisabled("No screens found.");
        return {};
    }

    const RECT area = desktopArea();
    const float none = noneWidth();
    const float scale = (size.x - none) / static_cast<float>(area.right - area.left);
    const ImVec2 cursor = ImGui::GetCursorScreenPos();
    const float left = std::floor(cursor.x + (maxSize.x - size.x) * 0.5f);
    const ImVec2 origin(left + none, std::floor(cursor.y)); // The screens' top left.

    const float dpi = ImGui::GetStyle().FontScaleDpi;
    const float halfGap = 3.0f * dpi;
    const float rounding = 5.0f * dpi;
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    Choice choice;

    // None first, left of the screens and as tall as all of them, while a screen is observed.
    if (none > 0.0f) {
        const ImVec2 min(left + halfGap, origin.y + halfGap);
        const ImVec2 max(left + std::round(kNoneTile * dpi) - halfGap, std::round(origin.y + size.y - halfGap));
        choice.none = drawNone(min, max, rounding);
    }

    for (std::size_t i = 0; i < miniatures_.size(); ++i) {
        Miniature& miniature = miniatures_[i];
        refresh(miniature);

        const RECT& bounds = miniature.screen.bounds;
        const ImVec2 min(std::round(origin.x + static_cast<float>(bounds.left - area.left) * scale + halfGap),
                         std::round(origin.y + static_cast<float>(bounds.top - area.top) * scale + halfGap));
        const ImVec2 max(std::round(origin.x + static_cast<float>(bounds.right - area.left) * scale - halfGap),
                         std::round(origin.y + static_cast<float>(bounds.bottom - area.top) * scale - halfGap));

        ImGui::SetCursorScreenPos(min);
        ImGui::PushID(static_cast<int>(i));
        if (ImGui::InvisibleButton("screen", ImVec2(max.x - min.x, max.y - min.y))) {
            choice.screen = &miniature.screen;
        }
        const bool hovered = ImGui::IsItemHovered();
        ImGui::PopID();

        const bool isObserved = miniature.screen.name == observed_;
        const ScreenImage* image = isObserved ? observedImage_ : miniature.image.get();
        if (image != nullptr && image->hasImage()) {
            image->draw(drawList, min, max, rounding, background);
        } else {
            drawList->AddRectFilled(min, max, theme::kSurfaceHover, rounding);
        }

        // Accent outline on the screen under the mouse and on the one being observed.
        const float outline = (hovered || isObserved ? 2.0f : 1.0f) * dpi;
        const float half = outline * 0.5f;
        drawList->PathRect(ImVec2(min.x - half, min.y - half), ImVec2(max.x + half, max.y + half), rounding + half);
        drawList->PathStroke(hovered || isObserved ? theme::kAccent : theme::kBorder, ImDrawFlags_Closed, outline);

        // The monitor's name, and under it its size and refresh rate while the mouse is over it.
        char detail[64];
        if (hovered) {
            ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
            std::snprintf(detail, sizeof(detail), "%ld \xC3\x97 %ld  \xC2\xB7  %u Hz", bounds.right - bounds.left,
                          bounds.bottom - bounds.top, miniature.screen.refreshHz);
        }
        drawList->PushClipRect(min, max, true);
        theme::drawLabel(drawList, ImVec2(min.x + 6.0f * dpi, max.y - 6.0f * dpi), ImVec2(0.0f, 1.0f),
                         miniature.screen.monitor.c_str(), hovered ? detail : nullptr);
        drawList->PopClipRect();
    }

    // Leave the cursor below the miniatures, as if they were one item.
    ImGui::SetCursorScreenPos(cursor);
    ImGui::Dummy(ImVec2(maxSize.x, size.y));
    return choice;
}

} // namespace observex
