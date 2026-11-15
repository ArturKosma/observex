#include "app/DebugView.h"

#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace observex {

namespace {

// Milliseconds with as many decimals as matter: "0.04", "3.41", "13.6", "120".
void formatMs(char (&text)[16], float ms) {
    std::snprintf(text, sizeof(text), ms < 10.0f ? "%.2f" : ms < 100.0f ? "%.1f" : "%.0f", static_cast<double>(ms));
}

void formatRate(char (&text)[16], float perSecond) {
    std::snprintf(text, sizeof(text), perSecond < 10.0f ? "%.1f" : "%.0f", static_cast<double>(perSecond));
}

void formatBusy(char (&text)[16], float busy) {
    const float percent = busy * 100.0f;
    if (percent > 0.0f && percent < 0.5f) {
        std::snprintf(text, sizeof(text), "<1%%");
    } else {
        std::snprintf(text, sizeof(text), "%.0f%%", static_cast<double>(percent));
    }
}

// `text` at the right of the current table cell.
void rightAligned(const char* text, ImU32 colour) {
    const float width = ImGui::CalcTextSize(text).x;
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.0f, ImGui::GetContentRegionAvail().x - width));
    ImGui::PushStyleColor(ImGuiCol_Text, colour);
    ImGui::TextUnformatted(text);
    ImGui::PopStyleColor();
}

// One row: the name (indented for a step), then its numbers.
void costRow(const Cost& cost, bool step, const theme::Fonts& fonts, float indent) {
    const ImU32 colour = step ? theme::kTextMuted : theme::kText;
    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);
    if (step) {
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + indent);
    } else {
        ImGui::PushFont(fonts.medium, 0.0f);
    }
    ImGui::PushStyleColor(ImGuiCol_Text, colour);
    ImGui::TextUnformatted(cost.name.data());
    ImGui::PopStyleColor();
    if (!step) {
        ImGui::PopFont();
    }
    char text[16];
    ImGui::TableSetColumnIndex(1);
    formatMs(text, cost.eachMs);
    rightAligned(text, colour);
    ImGui::TableSetColumnIndex(2);
    formatMs(text, cost.slowestMs);
    rightAligned(text, colour);
    ImGui::TableSetColumnIndex(3);
    formatRate(text, cost.perSecond);
    rightAligned(text, colour);
    ImGui::TableSetColumnIndex(4);
    formatBusy(text, cost.busy());
    rightAligned(text, colour);
}

} // namespace

void drawCosts(std::span<const CostPart> parts, const char* when, const char* empty, const theme::Fonts& fonts,
               float dpiScale) {
    if (parts.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, theme::kTextMuted);
        ImGui::TextUnformatted(empty);
        ImGui::PopStyleColor();
        return;
    }
    // The total, as the top bar has it right of Show.
    char total[32];
    std::snprintf(total, sizeof(total), "%.1f ms", static_cast<double>(costTotalMs(parts)));
    ImGui::PushFont(fonts.medium, std::round(ImGui::GetFontSize() * 1.3f));
    ImGui::TextUnformatted(total);
    ImGui::PopFont();
    ImGui::PushStyleColor(ImGuiCol_Text, theme::kTextMuted);
    ImGui::TextWrapped("on each screen image, %s", when);
    ImGui::PopStyleColor();
    ImGui::Dummy(ImVec2(0.0f, std::round(8.0f * dpiScale)));

    // The names keep their whole width; the numbers share what is left (a narrow tile cuts numbers, not names).
    const float indent = std::round(12.0f * dpiScale);
    float names = 0.0f;
    for (const CostPart& part : parts) {
        for (int i = 0; i < part.stepCount; ++i) {
            names = std::max(names, indent + ImGui::CalcTextSize(part.steps[static_cast<std::size_t>(i)].name.data()).x);
        }
    }
    ImGui::PushFont(fonts.medium, 0.0f);
    for (const CostPart& part : parts) {
        names = std::max(names, ImGui::CalcTextSize(part.run.name.data()).x);
    }
    ImGui::PopFont();
    ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(std::round(6.0f * dpiScale), std::round(2.0f * dpiScale)));
    if (ImGui::BeginTable("##costs", 5, ImGuiTableFlags_NoPadOuterX)) {
        ImGui::TableSetupColumn("##name", ImGuiTableColumnFlags_WidthFixed, names);
        const char* headers[] = {"Each", "Slowest", "Per s", "Busy"};
        const float number = ImGui::CalcTextSize("000.00").x;
        for (const char* header : headers) { // Each as wide as its header or a number needs.
            ImGui::TableSetupColumn("##number", ImGuiTableColumnFlags_WidthStretch,
                                    std::max(number, ImGui::CalcTextSize(header).x));
        }
        ImGui::TableNextRow();
        for (int column = 0; column < 4; ++column) {
            ImGui::TableSetColumnIndex(column + 1);
            rightAligned(headers[column], theme::kTextMuted);
        }
        for (const CostPart& part : parts) {
            ImGui::TableNextRow(ImGuiTableRowFlags_None, std::round(6.0f * dpiScale)); // A little apart.
            costRow(part.run, false, fonts, indent);
            for (int i = 0; i < part.stepCount; ++i) {
                if (const Cost& step = part.steps[static_cast<std::size_t>(i)]; step.perSecond > 0.0f) {
                    costRow(step, true, fonts, indent);
                }
            }
        }
        ImGui::EndTable();
    }
    ImGui::PopStyleVar();
}

} // namespace observex
