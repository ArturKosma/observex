#include "app/StatsPanel.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>

namespace observex {

namespace {

constexpr char kYou[] = "You";
constexpr char kCooldowns[] = "Cooldowns";
constexpr char kNearby[] = "Nearby";
constexpr char kTarget[] = "Target";
// Before DPI scaling.
constexpr float kTwoColumns = 560.0f; // From this wide the target is a column of its own.
constexpr float kPad = 12.0f;         // Inside a card.
constexpr float kCardGap = 10.0f;     // Between cards, and columns.
constexpr float kRounding = 8.0f;

// The colour of a bar's fill by its piece and name.
ImU32 barColour(std::string_view piece, std::string_view name) {
    if (piece == kCastingPiece) {
        return IM_COL32(110, 196, 255, 255); // The game's cast bar: light blue.
    }
    if (name == "HP") {
        return IM_COL32(232, 72, 64, 255);
    }
    if (name == "MP") {
        return IM_COL32(56, 128, 250, 255);
    }
    if (name == "XP") {
        return IM_COL32(206, 200, 255, 255);
    }
    return theme::kTextMuted;
}

// What a row shows for one gauge.
struct Value {
    float full = -1.0f; // Below 0: the bar drawn empty.
    char text[40] = {};
    char percent[16] = {};
    bool doubted = false;
    bool none = false;  // Nothing known: muted.
    bool noBar = false; // Words or a count only (the target's name, Nearby).
    ImU32 fill = 0;
    const char* icon = nullptr; // A skill's, before the text (the cast).
};

Value valueOf(const GameGauge* gauge) {
    Value value;
    if (gauge == nullptr) {
        value.none = true;
        std::snprintf(value.text, sizeof(value.text), "Not read yet");
        return value;
    }
    const std::string_view piece = gauge->piece.data();
    value.fill = barColour(piece, gauge->name.data());
    const bool skill = piece == kSkillsPiece;
    const bool casting = piece == kCastingPiece;
    value.noBar = piece == kNearbyPiece ||
                  (piece == kTargetPiece && std::strcmp(gauge->name.data(), kTargetNameGauge) == 0);
    if (!gauge->seen) {
        value.none = true;
        std::snprintf(value.text, sizeof(value.text), skill ? "Not on the screen" : "\xE2\x80\x94"); // An em dash.
        return value;
    }
    if (skill) {
        // How far it has recharged (the game's wipe grows back the same way), the seconds left, Ready or Pressed.
        if (gauge->pressed) {
            value.full = 0.0f;
            std::snprintf(value.text, sizeof(value.text), "Pressed");
            return value;
        }
        const bool ready = gauge->full <= 0.0f;
        value.full = ready ? 1.0f : 1.0f - gauge->full;
        value.fill = ready ? theme::kYes : theme::kAccent;
        if (ready) {
            std::snprintf(value.text, sizeof(value.text), "Ready");
        } else if (gauge->text[0] != '\0') {
            std::snprintf(value.text, sizeof(value.text), "%s s", gauge->text.data());
        } else {
            std::snprintf(value.percent, sizeof(value.percent), "%.0f%% left",
                          static_cast<double>(gauge->full * 100.0f));
        }
        return value;
    }
    if (casting) {
        // What is cast, by its icon and name, while the bar fills.
        value.full = gauge->full;
        if (gauge->full <= 0.0f) {
            value.none = true;
            std::snprintf(value.text, sizeof(value.text), "\xE2\x80\x94");
            return value;
        }
        if (gauge->text[0] != '\0') {
            std::snprintf(value.text, sizeof(value.text), "%s", gauge->text.data());
            value.icon = gauge->text.data();
        }
        std::snprintf(value.percent, sizeof(value.percent), "%.0f%%", static_cast<double>(gauge->full * 100.0f));
        return value;
    }
    value.full = gauge->full;
    if (gauge->text[0] != '\0') {
        std::snprintf(value.text, sizeof(value.text), "%s", gauge->text.data());
    } else if (gauge->doubted[0] != '\0') {
        std::snprintf(value.text, sizeof(value.text), "%s?", gauge->doubted.data());
        value.doubted = true;
    }
    // How full in percent, unless the number already is one.
    if (!value.noBar && gauge->full >= 0.0f && std::strchr(value.text, '%') == nullptr) {
        std::snprintf(value.percent, sizeof(value.percent), "%.0f%%", static_cast<double>(gauge->full * 100.0f));
    }
    if (value.text[0] == '\0' && value.percent[0] == '\0') {
        value.none = true;
        std::snprintf(value.text, sizeof(value.text), "\xE2\x80\x94");
    }
    return value;
}

// One row of a card: a gauge (null: a profile's skill not read yet), and its label.
struct Row {
    const GameGauge* gauge = nullptr;
    std::string label;
    bool skill = false; // Its icon before its label.
};
struct Card {
    const char* title = "";
    std::vector<Row> rows;
    const char* empty = nullptr; // Said when it has no rows.
};

class Drawer {
public:
    Drawer(const theme::Fonts& fonts, const SkillIcons& icons, float dpi)
        : fonts_(fonts), icons_(icons), dpi_(dpi), drawList_(ImGui::GetWindowDrawList()),
          lineHeight_(ImGui::GetTextLineHeight()), rowHeight_(std::round(ImGui::GetTextLineHeight() + 10.0f * dpi)),
          pad_(std::round(kPad * dpi)), gap_(std::round(8.0f * dpi)), iconSide_(std::round(lineHeight_ + 4.0f * dpi)) {}

    // The label column's width for `cards`: as wide as the widest label (and icon), at most a few words.
    float labelWidth(const std::vector<Card>& cards) const {
        float width = std::round(40.0f * dpi_);
        for (const Card& card : cards) {
            for (const Row& row : card.rows) {
                width = std::max(width, ImGui::CalcTextSize(row.label.c_str()).x + (row.skill ? iconSide_ + gap_ : 0.0f) +
                                            gap_ * 1.5f);
            }
        }
        return std::min(width, std::round(170.0f * dpi_));
    }

    // Draws `card` from (x, y), `width` wide; returns the y under it.
    float card(const Card& card, float x, float y, float width, float labelWidth) const {
        const float titleHeight = std::round(lineHeight_ + 8.0f * dpi_);
        const std::size_t rows = card.rows.empty() ? 1 : card.rows.size();
        const float height = pad_ + titleHeight + rowHeight_ * static_cast<float>(rows) + pad_ - 6.0f * dpi_;
        const ImVec2 min(x, y);
        const ImVec2 max(x + width, std::round(y + height));
        drawList_->AddRectFilled(min, max, theme::kSurface, kRounding * dpi_);
        drawList_->AddRect(min, max, theme::kBorder, kRounding * dpi_);
        ImGui::PushFont(fonts_.medium, 0.0f);
        drawList_->AddText(ImVec2(x + pad_, y + pad_), theme::kTextMuted, card.title);
        ImGui::PopFont();
        float rowY = y + pad_ + titleHeight;
        if (card.rows.empty()) {
            drawList_->AddText(ImVec2(x + pad_, rowY), theme::kTextMuted, card.empty != nullptr ? card.empty : "");
            return max.y;
        }
        for (const Row& row : card.rows) {
            this->row(row, x + pad_, rowY, width - pad_ * 2.0f, labelWidth);
            rowY += rowHeight_;
        }
        return max.y;
    }

private:
    void row(const Row& row, float x, float y, float width, float labelWidth) const {
        const Value value = valueOf(row.gauge);
        const ImU32 labelColour = value.none ? theme::faded(theme::kTextMuted, 0.6f) : theme::kTextMuted;
        // The label, after the skill's icon.
        float labelX = x;
        if (row.skill) {
            const ImVec2 iconMin(x, std::round(y + (lineHeight_ - iconSide_) * 0.5f));
            const ImVec2 iconMax(iconMin.x + iconSide_, iconMin.y + iconSide_);
            if (!icons_.draw(drawList_, row.label, iconMin, iconMax, std::round(3.0f * dpi_), theme::kSurface)) {
                drawList_->AddRectFilled(iconMin, iconMax, theme::kSurfaceActive, std::round(3.0f * dpi_));
            }
            labelX = iconMax.x + gap_;
        }
        ImGui::PushClipRect(ImVec2(labelX, y), ImVec2(x + labelWidth - gap_ * 0.5f, y + rowHeight_), true);
        drawList_->AddText(ImVec2(labelX, y), labelColour, row.label.c_str());
        ImGui::PopClipRect();

        // The bar: its empty part, then as full as read; as wide as the card leaves, within reason.
        const float cx = x + labelWidth;
        float textX = cx;
        if (!value.noBar) {
            const float barWidth = std::clamp(width - labelWidth - std::round(130.0f * dpi_), std::round(48.0f * dpi_),
                                              std::round(180.0f * dpi_));
            const float barHeight = std::round(7.0f * dpi_);
            const ImVec2 barMin(cx, std::round(y + (lineHeight_ - barHeight) * 0.5f));
            const ImVec2 barMax(cx + barWidth, barMin.y + barHeight);
            drawList_->AddRectFilled(barMin, barMax, theme::kSurfaceActive, barHeight * 0.5f);
            if (value.full > 0.0f) {
                const float fill = std::round(barWidth * std::min(value.full, 1.0f));
                drawList_->AddRectFilled(barMin, ImVec2(barMin.x + std::max(fill, barHeight), barMax.y),
                                         value.doubted ? theme::faded(value.fill, 0.5f) : value.fill,
                                         barHeight * 0.5f);
            }
            textX = barMax.x + gap_ * 1.5f;
        }
        // The value: the cast skill's icon, then the text bright (muted when not known), then the percent muted.
        if (value.icon != nullptr) {
            const ImVec2 iconMin(textX, std::round(y + (lineHeight_ - iconSide_) * 0.5f));
            const ImVec2 iconMax(iconMin.x + iconSide_, iconMin.y + iconSide_);
            if (icons_.draw(drawList_, value.icon, iconMin, iconMax, std::round(3.0f * dpi_), theme::kSurface)) {
                textX = iconMax.x + gap_;
            }
        }
        float end = textX;
        if (value.text[0] != '\0') {
            const bool muted = value.none || value.doubted;
            ImGui::PushFont(muted ? fonts_.regular : fonts_.medium, 0.0f);
            drawList_->AddText(ImVec2(textX, y), muted ? theme::kTextMuted : theme::kText, value.text);
            end = textX + ImGui::CalcTextSize(value.text).x;
            ImGui::PopFont();
        }
        if (value.percent[0] != '\0') {
            const float at = value.text[0] != '\0' ? end + gap_ : textX;
            drawList_->AddText(ImVec2(at, y), theme::kTextMuted, value.percent);
        }
    }

    const theme::Fonts& fonts_;
    const SkillIcons& icons_;
    float dpi_;
    ImDrawList* drawList_;
    float lineHeight_;
    float rowHeight_;
    float pad_;
    float gap_;
    float iconSide_;
};

} // namespace

void StatsTile::draw(const theme::Fonts& fonts, const GameStats* stats, const char* empty, const char* problem,
                     const std::vector<statetree::Skill>& skills, const SkillIcons& icons, float dpi) {
    const ImVec2 start = ImGui::GetCursorScreenPos();
    const float width = std::max(1.0f, ImGui::GetContentRegionAvail().x);
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    float bottom = start.y;
    if (stats == nullptr) {
        drawList->AddText(start, theme::kTextMuted, empty != nullptr ? empty : "Nothing read yet");
        bottom += ImGui::GetTextLineHeight();
    } else {
        // The cards, each its gauges in the order read: You (the status window and the cast bar), Cooldowns (the
        // profile's skills with one, then any other read: a clip's), Nearby, Target (its name first), any other.
        Card you{kYou, {}, nullptr};
        Card cooldowns{kCooldowns, {}, "No skills with a cooldown yet (Skills tab)"};
        Card nearby{kNearby, {}, nullptr};
        Card target{kTarget, {}, "No target"};
        std::vector<Card> others;
        for (const statetree::Skill& skill : skills) {
            if (skill.cooldown) {
                cooldowns.rows.push_back({stats->find(kSkillsPiece, skill.name), skill.name, true});
            }
        }
        for (int i = 0; i < stats->count; ++i) {
            const GameGauge& gauge = stats->gauges[static_cast<std::size_t>(i)];
            const std::string_view piece = gauge.piece.data();
            Row row{&gauge, gauge.name.data(), false};
            if (piece == "Status" || piece == kCastingPiece) {
                you.rows.push_back(row);
            } else if (piece == kSkillsPiece) {
                const bool inProfile = std::any_of(skills.begin(), skills.end(), [&](const statetree::Skill& skill) {
                    return skill.name == gauge.name.data();
                });
                if (!inProfile) {
                    row.skill = true;
                    cooldowns.rows.push_back(row);
                }
            } else if (piece == kNearbyPiece) {
                nearby.rows.push_back(row);
            } else if (piece == kTargetPiece) {
                if (stats->hasTarget) {
                    const bool name = std::strcmp(gauge.name.data(), kTargetNameGauge) == 0;
                    target.rows.insert(name ? target.rows.begin() : target.rows.end(), row);
                }
            } else {
                auto card = std::find_if(others.begin(), others.end(),
                                         [&](const Card& c) { return piece == c.title; });
                if (card == others.end()) {
                    others.push_back({gauge.piece.data(), {}, nullptr});
                    card = others.end() - 1;
                }
                card->rows.push_back(row);
            }
        }

        // One column, or (wide enough) the target a column of its own at the right.
        const float cardGap = std::round(kCardGap * dpi);
        const bool two = width >= kTwoColumns * dpi;
        std::vector<Card> left;
        if (!you.rows.empty()) {
            left.push_back(you);
        }
        left.push_back(cooldowns);
        if (!two) {
            left.push_back(target);
        }
        if (!nearby.rows.empty()) {
            left.push_back(nearby);
        }
        left.insert(left.end(), others.begin(), others.end());
        const Drawer drawer(fonts, icons, dpi);
        std::vector<Card> all = left;
        all.push_back(target);
        const float labelWidth = drawer.labelWidth(all);
        const float leftWidth = two ? std::floor((width - cardGap) * 0.55f) : width;
        float y = start.y;
        for (const Card& card : left) {
            y = drawer.card(card, start.x, y, leftWidth, labelWidth) + cardGap;
        }
        bottom = y - cardGap;
        if (two) {
            const float right = drawer.card(target, start.x + leftWidth + cardGap, start.y,
                                            width - leftWidth - cardGap, labelWidth);
            bottom = std::max(bottom, right);
        }
    }
    if (problem != nullptr && problem[0] != '\0') {
        bottom += std::round(8.0f * dpi);
        drawList->AddText(ImGui::GetFont(), ImGui::GetFontSize(), ImVec2(start.x, bottom), theme::kTextMuted, problem,
                          nullptr, width);
        bottom += ImGui::CalcTextSize(problem, nullptr, false, width).y;
    }
    ImGui::SetCursorScreenPos(start);
    ImGui::Dummy(ImVec2(width, bottom - start.y));
}

} // namespace observex
