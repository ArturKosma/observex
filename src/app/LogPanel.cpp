#include "app/LogPanel.h"

#include "core/LogStore.h"

#include <imgui.h>
#include <misc/cpp/imgui_stdlib.h>

#include <cstdio>
#include <string>
#include <string_view>
#include <utility>

namespace observex {

namespace {

ImVec4 levelColor(LogLevel level) {
    switch (level) {
    case LogLevel::Trace:
        return {0.55f, 0.55f, 0.55f, 1.0f};
    case LogLevel::Debug:
        return {0.60f, 0.75f, 0.90f, 1.0f};
    case LogLevel::Info:
        return {0.88f, 0.88f, 0.88f, 1.0f};
    case LogLevel::Warning:
        return {1.00f, 0.80f, 0.30f, 1.0f};
    case LogLevel::Error:
        return {1.00f, 0.42f, 0.42f, 1.0f};
    case LogLevel::Fatal:
        return {1.00f, 0.30f, 0.70f, 1.0f};
    }
    return {1.0f, 1.0f, 1.0f, 1.0f};
}

void textView(std::string_view text) {
    ImGui::TextUnformatted(text.data(), text.data() + text.size());
}

} // namespace

LogPanel::LogPanel(LogStore& store) : store_(store), maxEntries_(store.capacity()) {}

void LogPanel::draw(const char* title, bool* open) {
    // Pull even when the window is collapsed, so the local copy stays current.
    pullNewEntries();

    if (!ImGui::Begin(title, open)) {
        ImGui::End();
        return;
    }
    drawToolbar();
    if (filter_ != appliedFilter_) {
        refilter();
    }
    ImGui::Separator();
    drawTable();
    ImGui::End();
}

void LogPanel::pullNewEntries() {
    incoming_.clear();
    if (store_.copySince(lastSeq_, incoming_) == 0) {
        return;
    }
    for (LogEntry& entry : incoming_) {
        lastSeq_ = entry.seq;
        ++levelCounts_[static_cast<std::size_t>(entry.level)];
        if (appliedFilter_.matches(entry)) {
            visible_.push_back(firstIndex_ + entries_.size());
        }
        entries_.push_back(std::move(entry));
    }
    dropOldEntries();
}

void LogPanel::dropOldEntries() {
    while (entries_.size() > maxEntries_) {
        --levelCounts_[static_cast<std::size_t>(entries_.front().level)];
        entries_.pop_front();
        ++firstIndex_;
    }
    while (!visible_.empty() && visible_.front() < firstIndex_) {
        visible_.pop_front();
    }
}

void LogPanel::refilter() {
    visible_.clear();
    for (std::size_t i = 0; i < entries_.size(); ++i) {
        if (filter_.matches(entries_[i])) {
            visible_.push_back(firstIndex_ + i);
        }
    }
    appliedFilter_ = filter_;
}

void LogPanel::clear() {
    // There is only one view for now, so clearing it clears the shared store too.
    store_.clear();
    entries_.clear();
    visible_.clear();
    levelCounts_ = {};
    firstIndex_ = 0;
}

void LogPanel::drawToolbar() {
    for (int i = 0; i < kLogLevelCount; ++i) {
        const auto level = static_cast<LogLevel>(i);
        const std::string_view name = levelName(level);

        // The count changes every frame, so the widget ID comes after "###" to keep it stable.
        char label[64];
        std::snprintf(label, sizeof(label), "%.*s %zu###level%d", static_cast<int>(name.size()), name.data(),
                      levelCounts_[static_cast<std::size_t>(i)], i);

        if (i > 0) {
            ImGui::SameLine();
        }
        ImGui::PushStyleColor(ImGuiCol_Text, levelColor(level));
        ImGui::Checkbox(label, &filter_.levels[static_cast<std::size_t>(i)]);
        ImGui::PopStyleColor();
    }

    ImGui::SameLine();
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 18.0f);
    ImGui::InputTextWithHint("##filter", "Filter source or message", &filter_.text);

    ImGui::SameLine();
    if (ImGui::Button("Clear")) {
        clear();
    }
    ImGui::SameLine();
    ImGui::Checkbox("Auto-scroll", &autoScroll_);
    ImGui::SameLine();
    ImGui::TextDisabled("%zu / %zu shown", visible_.size(), entries_.size());
}

void LogPanel::drawTable() {
    if (entries_.empty()) {
        ImGui::TextDisabled("No entries yet.");
        return;
    }

    constexpr ImGuiTableFlags kFlags = ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV |
                                       ImGuiTableFlags_Resizable | ImGuiTableFlags_Reorderable |
                                       ImGuiTableFlags_Hideable | ImGuiTableFlags_SizingFixedFit;
    if (!ImGui::BeginTable("entries", 4, kFlags)) {
        return;
    }
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("Time", ImGuiTableColumnFlags_WidthFixed);
    ImGui::TableSetupColumn("Level", ImGuiTableColumnFlags_WidthFixed);
    ImGui::TableSetupColumn("Source", ImGuiTableColumnFlags_WidthFixed);
    ImGui::TableSetupColumn("Message", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableHeadersRow();

    // Only the rows on screen are drawn, so this stays fast with hundreds of thousands of entries.
    ImGuiListClipper clipper;
    clipper.Begin(static_cast<int>(visible_.size()));
    while (clipper.Step()) {
        for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row) {
            const LogEntry& entry = entries_[visible_[static_cast<std::size_t>(row)] - firstIndex_];
            ImGui::TableNextRow();

            ImGui::TableNextColumn();
            textView(formatTime(entry.time));

            ImGui::TableNextColumn();
            ImGui::PushStyleColor(ImGuiCol_Text, levelColor(entry.level));
            textView(levelName(entry.level));
            ImGui::PopStyleColor();

            ImGui::TableNextColumn();
            textView(entry.source);

            ImGui::TableNextColumn();
            textView(entry.message);
        }
    }

    // Keep following new entries while the view is scrolled to the bottom.
    // Scrolling up stops following; scrolling back down resumes it.
    if (autoScroll_ && ImGui::GetScrollY() >= ImGui::GetScrollMaxY()) {
        ImGui::SetScrollHereY(1.0f);
    }
    ImGui::EndTable();
}

} // namespace observex
