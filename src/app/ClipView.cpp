#include "app/ClipView.h"

#include "app/Theme.h"

#include <imgui.h>

#include <cmath>
#include <exception>
#include <utility>

namespace observex {

namespace {

void centredText(const char* text, ImU32 color, float y) {
    const float width = ImGui::CalcTextSize(text).x;
    ImGui::SetCursorPos(ImVec2(std::round((ImGui::GetWindowWidth() - width) * 0.5f), std::round(y)));
    ImGui::PushStyleColor(ImGuiCol_Text, color);
    ImGui::TextUnformatted(text);
    ImGui::PopStyleColor();
}

} // namespace

ClipView::ClipView(IDXGIAdapter1* gpu, ID3D11Device* device, std::filesystem::path folder, ClipInfo info)
    : folder_(std::move(folder)), info_(std::move(info)) {
    device->GetImmediateContext(&context_);
    try {
        player_ = std::make_unique<ClipPlayer>(gpu, folder_ / kClipVideo);
        reader_ = std::make_unique<ImageReader>(device, player_->frames());
        image_ = std::make_unique<ScreenImage>(device, player_->width(), player_->height());
    } catch (const std::exception& e) {
        image_.reset();
        reader_.reset(); // Before the player, whose channel it reads.
        player_.reset();
        openError_ = e.what();
    }
}

void ClipView::update() {
    ImageStamp stamp;
    if (reader_ && reader_->copyNewImage(context_.Get(), image_->texture(), &stamp)) {
        image_->imageCopied();
        frame_ = stamp.number;
    }
}

bool ClipView::canPlay() const {
    return player_ && player_->state() != ClipPlayer::State::Failed;
}

bool ClipView::playing() const {
    return player_ && player_->state() == ClipPlayer::State::Playing;
}

bool ClipView::ended() const {
    return player_ && player_->state() == ClipPlayer::State::Ended;
}

void ClipView::togglePlay() {
    if (!canPlay()) {
        return;
    }
    if (playing()) {
        player_->pause();
    } else {
        player_->play();
    }
}

double ClipView::position() const {
    return player_ ? player_->position() : 0.0;
}

double ClipView::seconds() const {
    return player_ ? player_->seconds() : info_.seconds;
}

const std::string& ClipView::error() const {
    if (player_ && player_->state() == ClipPlayer::State::Failed) {
        return player_->error();
    }
    return openError_;
}

void ClipView::draw() {
    if (ImGui::IsKeyPressed(ImGuiKey_Space, false) && !ImGui::GetIO().WantTextInput) {
        togglePlay();
    }

    const float middle = ImGui::GetWindowHeight() * 0.5f;
    if (const std::string& why = error(); !why.empty()) {
        centredText("This clip cannot be played.", theme::kText, middle - ImGui::GetTextLineHeightWithSpacing());
        centredText(why.c_str(), theme::kTextMuted, middle);
        return;
    }
    if (!image_->hasImage()) {
        centredText("Loading the clip\xE2\x80\xA6", theme::kTextMuted, middle);
        return;
    }

    // The frame as large as fits, keeping its shape, framed like the observed screen.
    const ImVec2 area = ImGui::GetContentRegionAvail();
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const float aspect = static_cast<float>(image_->width()) / static_cast<float>(image_->height());
    const ImVec2 size = area.x / area.y > aspect ? ImVec2(area.y * aspect, area.y) : ImVec2(area.x, area.x / aspect);
    const ImVec2 min(std::round(origin.x + (area.x - size.x) * 0.5f), std::round(origin.y + (area.y - size.y) * 0.5f));
    const ImVec2 max(min.x + std::round(size.x), min.y + std::round(size.y));
    const float rounding = 8.0f * ImGui::GetStyle().FontScaleDpi;
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    image_->draw(drawList, min, max, rounding, theme::kBackground);
    drawList->PathRect(ImVec2(min.x - 0.5f, min.y - 0.5f), ImVec2(max.x + 0.5f, max.y + 0.5f), rounding + 0.5f);
    drawList->PathStroke(theme::kBorder, ImDrawFlags_Closed, 1.0f);
}

} // namespace observex
