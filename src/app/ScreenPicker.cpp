#include "app/ScreenPicker.h"

#include "app/Theme.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <exception>
#include <utility>

namespace observex {

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

ImVec2 ScreenPicker::fit(ImVec2 maxSize) {
    if (!open_) {
        open();
    }
    const RECT area = desktopArea();
    if (area.right <= area.left || area.bottom <= area.top) {
        return ImVec2(maxSize.x, ImGui::GetFrameHeight()); // Room for the "no screens" text.
    }
    const float width = static_cast<float>(area.right - area.left);
    const float height = static_cast<float>(area.bottom - area.top);
    const float scale = std::min(maxSize.x / width, maxSize.y / height);
    return ImVec2(width * scale, height * scale);
}

const Screen* ScreenPicker::draw(ImVec2 maxSize, ImU32 background) {
    const ImVec2 size = fit(maxSize);
    drawn_ = true;
    if (miniatures_.empty()) {
        ImGui::TextDisabled("No screens found.");
        return nullptr;
    }

    const RECT area = desktopArea();
    const float scale = size.x / static_cast<float>(area.right - area.left);
    const ImVec2 cursor = ImGui::GetCursorScreenPos();
    const ImVec2 origin(std::floor(cursor.x + (maxSize.x - size.x) * 0.5f), std::floor(cursor.y));

    const float dpi = ImGui::GetStyle().FontScaleDpi;
    const float halfGap = 3.0f * dpi;
    const float rounding = 5.0f * dpi;
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    const Screen* clicked = nullptr;

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
            clicked = &miniature.screen;
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
    return clicked;
}

} // namespace observex
