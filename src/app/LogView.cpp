#include "app/LogView.h"

#include "app/Theme.h"

#include <imgui.h>

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <string>
#include <string_view>

namespace observex {

namespace {

// "Friday, 13 November 2026" from a log line's time ("2026-11-13 00:06:01.234"); its first 10 letters as they are if
// they are not a date.
std::string dateOf(std::string_view when) {
    static constexpr const char* kMonths[] = {"January", "February", "March",     "April",   "May",      "June",
                                              "July",    "August",   "September", "October", "November", "December"};
    static constexpr const char* kDays[] = {"Sunday",   "Monday", "Tuesday", "Wednesday",
                                            "Thursday", "Friday", "Saturday"};
    const std::string day(when.substr(0, 10));
    const auto number = [&day](std::size_t at, std::size_t length) {
        int value = 0;
        const char* begin = day.data() + at;
        const auto [end, error] = std::from_chars(begin, begin + length, value);
        return error == std::errc() && end == begin + length ? value : 0;
    };
    if (day.size() < 10) {
        return day;
    }
    const int year = number(0, 4);
    const int month = number(5, 2);
    const int date = number(8, 2);
    if (month < 1 || month > 12 || date < 1) {
        return day;
    }
    namespace chrono = std::chrono;
    const chrono::year_month_day ymd{chrono::year(year), chrono::month(static_cast<unsigned>(month)),
                                     chrono::day(static_cast<unsigned>(date))};
    if (!ymd.ok()) {
        return day;
    }
    const chrono::weekday weekday{chrono::sys_days(ymd)};
    char text[48];
    std::snprintf(text, sizeof(text), "%s, %d %s %d", kDays[weekday.c_encoding()], date, kMonths[month - 1], year);
    return text;
}

} // namespace

void LogView::draw(std::span<const LogLine> lines, std::size_t happened, std::uint64_t version, const char* empty,
                   float dpiScale) {
    if (lines.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, theme::kTextMuted);
        ImGui::TextUnformatted(empty);
        ImGui::PopStyleColor();
        rows_.clear();
        widths_.clear();
        tops_.clear();
        version_ = version;
        count_ = 0;
        lastWhen_.clear();
        return;
    }
    // At the bottom as the last frame left it: then it stays with the newest line.
    const bool atBottom = ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 1.0f;

    // The rows: only lines added after the last measured ones are added to them (the book grows a line at a time);
    // anything else (the book cut at its start, another clip) builds them again.
    const bool same = version == version_ && lines.size() == count_;
    const bool added = !same && count_ > 0 && lines.size() > count_ && version - version_ == lines.size() - count_ &&
                       lines[count_ - 1].when == lastWhen_;
    if (!same && !added) {
        rows_.clear();
        widths_.clear();
        tops_.clear();
        count_ = 0;
    }
    const std::size_t firstAdded = rows_.size();
    for (std::size_t i = count_; i < lines.size(); ++i) {
        if (i == 0 || lines[i].when.compare(0, 10, lines[i - 1].when, 0, 10) != 0) {
            rows_.push_back(static_cast<std::uint32_t>(i) | kDateRow);
        }
        rows_.push_back(static_cast<std::uint32_t>(i));
        widths_.push_back(ImGui::CalcTextSize(lines[i].text.c_str()).x);
    }
    version_ = version;
    count_ = lines.size();
    lastWhen_ = lines.back().when;

    // Their heights: a row a line high, more where the text wraps. All measured again when the tile's width changes.
    const float lineHeight = ImGui::GetTextLineHeight();
    const float pad = std::round(6.0f * dpiScale);
    const float rowHeight = std::round(lineHeight + pad);
    const float textTop = std::round(pad * 0.5f);
    const float timeWidth = ImGui::CalcTextSize("00:00:00").x + std::round(14.0f * dpiScale);
    const float wrap = std::max(ImGui::GetContentRegionAvail().x - timeWidth, ImGui::CalcTextSize("00000").x);
    std::size_t measureFrom = firstAdded;
    if (wrap != wrap_ || rowHeight != rowHeight_ || tops_.empty()) {
        measureFrom = 0;
        wrap_ = wrap;
        rowHeight_ = rowHeight;
    }
    tops_.resize(measureFrom + 1);
    if (measureFrom == 0) {
        tops_[0] = 0.0f;
    }
    for (std::size_t r = measureFrom; r < rows_.size(); ++r) {
        float height = rowHeight;
        const std::size_t index = rows_[r] & ~kDateRow;
        if ((rows_[r] & kDateRow) == 0 && widths_[index] > wrap) {
            height = std::round(ImGui::CalcTextSize(lines[index].text.c_str(), nullptr, false, wrap).y + pad);
        }
        tops_.push_back(tops_.back() + height);
    }

    // Only the rows in sight are drawn; one item as high as all of them scrolls the tile.
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const float seenTop = drawList->GetClipRectMin().y - origin.y;
    const float seenBottom = drawList->GetClipRectMax().y - origin.y;
    ImFont* font = ImGui::GetFont();
    const float fontSize = ImGui::GetFontSize();
    for (std::size_t r = static_cast<std::size_t>(std::upper_bound(tops_.begin() + 1, tops_.end(), seenTop) -
                                                  (tops_.begin() + 1));
         r < rows_.size() && tops_[r] < seenBottom; ++r) {
        const std::uint32_t row = rows_[r];
        const std::size_t index = row & ~kDateRow;
        const LogLine& line = lines[index];
        const ImVec2 textAt(origin.x, origin.y + tops_[r] + textTop);
        if ((row & kDateRow) != 0) {
            drawList->AddText(textAt, theme::kTextMuted, dateOf(line.when).c_str());
        } else {
            const bool later = index >= happened;
            const std::string_view time =
                line.when.size() >= 19 ? std::string_view(line.when).substr(11, 8) : std::string_view(line.when);
            drawList->AddText(textAt, later ? theme::faded(theme::kTextMuted, 0.5f) : theme::kTextMuted, time.data(),
                              time.data() + time.size());
            drawList->AddText(font, fontSize, ImVec2(origin.x + timeWidth, textAt.y),
                              later ? theme::faded(theme::kText, 0.4f) : theme::kText, line.text.c_str(), nullptr,
                              wrap);
        }
    }
    ImGui::Dummy(ImVec2(1.0f, tops_.back()));
    if ((!same || measureFrom == 0) && atBottom) {
        ImGui::SetScrollHereY(1.0f);
    }
}

} // namespace observex
