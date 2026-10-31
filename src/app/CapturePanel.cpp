#include "app/CapturePanel.h"

#include "engine/Pipeline.h"
#include "engine/PreviewChannel.h"

#include <imgui.h>

#include <algorithm>
#include <cstdint>

namespace observex {

namespace {

void row(const char* label, const char* tooltip) {
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    ImGui::TextDisabled("%s", label);
    if (tooltip != nullptr) {
        ImGui::SetItemTooltip("%s", tooltip);
    }
    ImGui::TableNextColumn();
}

void drawState(const Pipeline& pipeline) {
    const ImVec4 green{0.45f, 0.85f, 0.45f, 1.0f};
    const ImVec4 amber{1.00f, 0.80f, 0.30f, 1.0f};
    const ImVec4 red{1.00f, 0.42f, 0.42f, 1.0f};
    switch (pipeline.state()) {
    case Pipeline::State::Running:
        ImGui::TextColored(green, "Capturing");
        break;
    case Pipeline::State::Reconnecting:
        ImGui::TextColored(amber, "Reconnecting...");
        break;
    case Pipeline::State::NeedsRestart:
        ImGui::TextColored(amber, "Restarting...");
        break;
    case Pipeline::State::Failed:
        ImGui::TextColored(red, "Failed");
        ImGui::SameLine();
        ImGui::TextWrapped("%s", pipeline.error().c_str());
        break;
    case Pipeline::State::Stopped:
        ImGui::TextDisabled("Stopped");
        break;
    }
}

} // namespace

void CapturePanel::draw(const char* title, bool* open, Pipeline& pipeline, PreviewReader* preview,
                        bool& restartRequested) {
    visible_ = false;
    if (!ImGui::Begin(title, open)) {
        ImGui::End();
        return;
    }
    visible_ = true;

    drawState(pipeline);
    drawStats(pipeline);

    ImGui::Checkbox("Show preview", &showPreview_);
    ImGui::SetItemTooltip("Copying each image for the preview costs a little GPU time. Turn it off for top speed.");
    ImGui::SameLine();
    if (ImGui::Button("Restart capture")) {
        restartRequested = true;
    }
    ImGui::Separator();

    if (showPreview_) {
        drawPreview(preview);
    }
    ImGui::End();
}

void CapturePanel::drawStats(Pipeline& pipeline) {
    const PipelineStats& stats = pipeline.stats();
    const double refresh = pipeline.refreshHz();

    if (!ImGui::BeginTable("stats", 2, ImGuiTableFlags_SizingFixedFit)) {
        return;
    }
    row("Screen", nullptr);
    ImGui::Text("%s, %u x %u at %.0f Hz", pipeline.targetName().c_str(), pipeline.width(), pipeline.height(), refresh);

    row("Capture rate", "New images per second. Windows only makes a new image when something on screen changes, "
                        "so this drops when the screen is still.");
    ImGui::Text("%.1f / s  (%.0f%% of refresh)", static_cast<double>(stats.imagesPerSecond),
                refresh > 0.0 ? 100.0 * stats.imagesPerSecond / refresh : 0.0);

    row("Latency", "Time from the screen update to observex holding the image.");
    ImGui::Text("avg %.2f ms   max %.2f ms", static_cast<double>(stats.latencyAvgMs),
                static_cast<double>(stats.latencyMaxMs));

    row("Work per image", "CPU time the pipeline spends on each image.");
    ImGui::Text("avg %.3f ms   max %.3f ms", static_cast<double>(stats.workAvgMs),
                static_cast<double>(stats.workMaxMs));

    row("Merged updates", "Screen updates that came between two captures and were merged into one image. "
                          "Rises when an app draws faster than the monitor refresh, or if observex falls behind.");
    ImGui::Text("%llu", static_cast<unsigned long long>(stats.mergedUpdates));

    row("Images captured", nullptr);
    ImGui::Text("%llu", static_cast<unsigned long long>(stats.images));

    ImGui::EndTable();
}

void CapturePanel::drawPreview(PreviewReader* preview) {
    ID3D11ShaderResourceView* view = preview != nullptr ? preview->view() : nullptr;
    if (view == nullptr) {
        ImGui::TextDisabled("Waiting for the first image...");
        return;
    }
    // Fit the image into the space left, keeping its aspect ratio.
    const ImVec2 space = ImGui::GetContentRegionAvail();
    const float width = static_cast<float>(preview->width());
    const float height = static_cast<float>(preview->height());
    const float scale = std::min(space.x / width, space.y / height);
    if (scale > 0.0f) {
        ImGui::Image(static_cast<ImTextureID>(reinterpret_cast<std::uintptr_t>(view)),
                     ImVec2(width * scale, height * scale));
    }
}

} // namespace observex
