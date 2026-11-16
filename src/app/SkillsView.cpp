#include "app/SkillsView.h"

#include "app/StateTreeView.h"
#include "app/Widgets.h"

#include <misc/cpp/imgui_stdlib.h> // ImGui::InputText on a std::string.

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <exception>

namespace observex {

namespace {

constexpr char kAddPopup[] = "##add-skill";
constexpr std::size_t kMaxName = 31; // SkillPicture's name, with its 0.
constexpr float kMinColumn = 300.0f; // The narrowest a column is beside the other (before DPI); else one under it.

const char* columnTitle(bool active) {
    return active ? "Active skills" : "Utility skills";
}

// Six dots, two columns of three, round `centre`: a card that can be dragged (as the State tile's rows).
void drawGrip(ImDrawList* drawList, ImVec2 centre, ImU32 colour, float dpi) {
    const float step = std::round(4.0f * dpi);
    const float radius = 1.25f * dpi;
    for (int column = 0; column < 2; ++column) {
        for (int line = -1; line <= 1; ++line) {
            drawList->AddCircleFilled(ImVec2(std::round(centre.x + (static_cast<float>(column) - 0.5f) * step) + 0.5f,
                                             std::round(centre.y + static_cast<float>(line) * step) + 0.5f),
                                      radius, colour, 8);
        }
    }
}

// A square + at the cursor, `size` wide; true when clicked.
bool plusButton(const char* id, float size, float dpi) {
    const ImVec2 min = ImGui::GetCursorScreenPos();
    const bool clicked = ImGui::InvisibleButton(id, ImVec2(size, size));
    const bool hovered = ImGui::IsItemHovered();
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    if (hovered) {
        drawList->AddRectFilled(min, ImVec2(min.x + size, min.y + size), theme::kSurfaceHover, 6.0f * dpi);
    }
    const float half = std::round(5.0f * dpi);
    const ImVec2 centre(std::round(min.x + size * 0.5f), std::round(min.y + size * 0.5f));
    const ImU32 colour = hovered ? theme::kText : theme::kTextMuted;
    drawList->AddLine(ImVec2(centre.x - half, centre.y), ImVec2(centre.x + half, centre.y), colour, 1.5f * dpi);
    drawList->AddLine(ImVec2(centre.x, centre.y - half), ImVec2(centre.x, centre.y + half), colour, 1.5f * dpi);
    return clicked;
}

std::string trimmed(std::string_view text) {
    const std::size_t first = text.find_first_not_of(" \t");
    if (first == std::string_view::npos) {
        return {};
    }
    return std::string(text.substr(first, text.find_last_not_of(" \t") - first + 1));
}

bool sameText(std::string_view a, std::string_view b) {
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
               return std::tolower(static_cast<unsigned char>(x)) == std::tolower(static_cast<unsigned char>(y));
           });
}

// Why `name` cannot be a skill's (empty when it can): its stats lines write spaces as "_" (core/GameStats).
std::string nameProblem(const std::string& name) {
    if (name.empty()) {
        return {};
    }
    if (name.find('_') != std::string::npos || name.find('|') != std::string::npos) {
        return "A skill's name has no _ or | in it."; // The profile's lists of skills are joined by |.
    }
    if (name.size() > kMaxName) {
        return "At most 31 letters.";
    }
    if (std::any_of(name.begin(), name.end(), [](char c) { return static_cast<unsigned char>(c) < 32; })) {
        return "Not a name a skill can have.";
    }
    return {};
}

// A square X, the frame's height; true when clicked.
bool crossButton(const char* id, float dpi) {
    const float size = ImGui::GetFrameHeight();
    const ImVec2 min = ImGui::GetCursorScreenPos();
    const bool clicked = ImGui::InvisibleButton(id, ImVec2(size, size));
    const bool hovered = ImGui::IsItemHovered();
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    if (hovered) {
        drawList->AddRectFilled(min, ImVec2(min.x + size, min.y + size), theme::kSurfaceHover, 6.0f * dpi);
    }
    const float half = std::round(4.0f * dpi);
    const ImVec2 centre(std::round(min.x + size * 0.5f), std::round(min.y + size * 0.5f));
    const ImU32 colour = hovered ? theme::kText : theme::kTextMuted;
    drawList->AddLine(ImVec2(centre.x - half, centre.y - half), ImVec2(centre.x + half, centre.y + half), colour,
                      1.5f * dpi);
    drawList->AddLine(ImVec2(centre.x - half, centre.y + half), ImVec2(centre.x + half, centre.y - half), colour,
                      1.5f * dpi);
    return clicked;
}

} // namespace

void SkillIcons::make(ID3D11Device* device, const std::vector<statetree::Skill>& skills) {
    entries_.clear();
    std::vector<std::uint8_t> bgra;
    for (const statetree::Skill& skill : skills) {
        const auto pixels = static_cast<std::size_t>(skill.side) * static_cast<std::size_t>(skill.side);
        if (skill.side <= 0 || skill.bgr.size() != pixels * 3) {
            continue;
        }
        bgra.resize(pixels * 4);
        for (std::size_t i = 0; i < pixels; ++i) {
            bgra[i * 4] = skill.bgr[i * 3];
            bgra[i * 4 + 1] = skill.bgr[i * 3 + 1];
            bgra[i * 4 + 2] = skill.bgr[i * 3 + 2];
            bgra[i * 4 + 3] = 255;
        }
        try {
            auto image = std::make_unique<ScreenImage>(device, static_cast<unsigned>(skill.side),
                                                       static_cast<unsigned>(skill.side));
            image->upload(bgra.data(), static_cast<unsigned>(skill.side) * 4);
            entries_.push_back({skill.name, std::move(image)});
        } catch (const std::exception&) {
            // No icon for it: its name alone is shown.
        }
    }
}

bool SkillIcons::draw(ImDrawList* drawList, std::string_view name, ImVec2 min, ImVec2 max, float rounding,
                      ImU32 background) const {
    for (const Entry& entry : entries_) {
        if (entry.name == name) {
            entry.image->draw(drawList, min, max, rounding, background);
            return true;
        }
    }
    return false;
}

void SkillsView::draw(const theme::Fonts& fonts, ID3D11Device* device, StateTreeView& profile,
                      const std::filesystem::path& folder, const SkillIcons& icons, const MiniatureSource& source,
                      const KeyTyped& keyTyped, const RefreshButton& refresh, float dpi) {
    // The profile at the top left as in the State tile, Refresh after it (under it in a narrow tile).
    const float left = ImGui::GetCursorScreenPos().x;
    const float rowTop = ImGui::GetCursorScreenPos().y;
    profile.drawProfileBar(fonts, folder);
    BarButton again;
    again.icon = Icon::Refresh;
    again.text = "Refresh";
    again.open = refresh.running; // Highlighted while it runs, how far along its bottom; a click stops it.
    again.progress = refresh.running ? refresh.progress : -1.0f;
    const float gap = std::round(12.0f * dpi);
    ImGui::SameLine(0.0f, gap);
    if (again.width(fonts) <= ImGui::GetContentRegionAvail().x) {
        ImGui::SetCursorScreenPos(ImVec2(ImGui::GetCursorScreenPos().x, rowTop));
    } else {
        ImGui::SetCursorScreenPos(ImVec2(left, rowTop + std::round(BarButton::kHeight * dpi) + std::round(8.0f * dpi)));
    }
    if (again.draw("##refresh-skills", fonts) && refresh.click) {
        refresh.click();
    }
    if (ImGui::IsItemHovered() && !refresh.running) {
        ImGui::SetTooltip("Reads each active skill's MP cost off its tooltip in the game (the mouse goes over them).");
    }
    const float barBottom = ImGui::GetItemRectMax().y;
    ImGui::SetCursorScreenPos(ImVec2(left, barBottom + std::round(8.0f * dpi)));
    if (!profile.fileError().empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, theme::kRecord);
        ImGui::TextWrapped("%s", profile.fileError().c_str());
        ImGui::PopStyleColor();
    }
    const float top = ImGui::GetCursorScreenPos().y + std::round(8.0f * dpi);

    // The two columns: side by side when both fit, else Active over Utility.
    std::vector<statetree::Skill>& skills = profile.skills();
    if (dragging_ >= static_cast<int>(skills.size())) {
        dragging_ = -1; // The profile changed under it (another loaded, Ctrl+Z).
    }
    const float width = ImGui::GetContentRegionAvail().x;
    const float columnGap = std::round(20.0f * dpi);
    const bool sideBySide = width >= 2.0f * std::round(kMinColumn * dpi) + columnGap;
    const float columnWidth = sideBySide ? std::floor((width - columnGap) * 0.5f) : width;
    Pass pass;
    pass.source = &source;
    const float activeBottom = drawColumn(fonts, skills, icons, true, ImVec2(left, top), columnWidth, dpi, pass);
    const ImVec2 utilityAt = sideBySide ? ImVec2(left + columnWidth + columnGap, top)
                                        : ImVec2(left, activeBottom + std::round(14.0f * dpi));
    const float utilityBottom = drawColumn(fonts, skills, icons, false, utilityAt, columnWidth, dpi, pass);

    // A card dragged: where it lands, or, let go, moved there.
    bool moved = false;
    if (dragging_ >= 0) {
        if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            if (!dragMoved_ && ImGui::IsMouseDragging(ImGuiMouseButton_Left, std::round(4.0f * dpi))) {
                dragMoved_ = true;
            }
            if (dragMoved_) {
                drawDrag(fonts, skills, icons, pass, sideBySide, dpi);
            }
        } else {
            if (dragMoved_) {
                drop(skills, pass, sideBySide, &moved);
            }
            dragging_ = -1;
            dragMoved_ = false;
        }
    }

    // The Add Skill popup, under the header of the column whose + opened it.
    const Column& adding = pass.columns[addActive_ ? 0 : 1];
    ImGui::SetNextWindowPos(ImVec2(adding.left, adding.cardsTop));
    if (ImGui::BeginPopup(kAddPopup)) {
        drawAddSkill(fonts, device, profile, source, dpi);
        ImGui::EndPopup();
    }

    ImGui::SetCursorScreenPos(ImVec2(left, std::max(activeBottom, utilityBottom)));
    ImGui::Dummy(ImVec2(width, 0.0f));
    if (pass.removed >= 0 && pass.removed < static_cast<int>(skills.size())) {
        skills.erase(skills.begin() + pass.removed);
        pass.changed = true;
    }
    if (pass.removed >= 0 || moved) {
        keyEditing_ = -1; // The indices moved.
        mpEditing_ = -1;
    }
    if (pass.changed || moved) {
        profile.saveSkills();
    }
    for (const auto& [name, key] : pass.typedKeys) {
        if (keyTyped) {
            keyTyped(name, key);
        }
    }
}

float SkillsView::drawColumn(const theme::Fonts& fonts, std::vector<statetree::Skill>& skills,
                             const SkillIcons& icons, bool active, ImVec2 at, float width, float dpi, Pass& pass) {
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    Column& column = pass.columns[active ? 0 : 1];
    column.left = at.x;
    column.right = at.x + width;
    column.top = at.y;
    const float frame = ImGui::GetFrameHeight();
    const float pad = std::round(8.0f * dpi);
    const bool dragging = dragging_ >= 0 && dragMoved_;

    // Its header: the title, how many (muted), and + at the right, which adds a skill to it.
    std::size_t count = 0;
    for (const statetree::Skill& skill : skills) {
        count += skill.active == active ? 1 : 0;
    }
    const char* title = columnTitle(active);
    const float headerMiddle = std::round(at.y + frame * 0.5f);
    ImGui::PushFont(fonts.medium, 0.0f);
    const ImVec2 titleAt(at.x + std::round(2.0f * dpi), std::round(headerMiddle - ImGui::GetFontSize() * 0.5f));
    const ImVec2 titleSize = ImGui::CalcTextSize(title);
    drawList->AddText(titleAt, theme::kText, title);
    ImGui::PopFont();
    if (count > 0) {
        char text[16];
        std::snprintf(text, sizeof(text), "%zu", count);
        drawList->AddText(ImVec2(titleAt.x + titleSize.x + std::round(7.0f * dpi),
                                 std::round(headerMiddle - ImGui::GetFontSize() * 0.5f)),
                          theme::faded(theme::kTextMuted, 0.7f), text);
    }
    if (!dragging && ImGui::IsWindowHovered() &&
        ImGui::IsMouseHoveringRect(titleAt, ImVec2(titleAt.x + titleSize.x, titleAt.y + titleSize.y))) {
        ImGui::SetTooltip("%s", active ? "Their cooldowns are read. Use Skill is done once the skill's MP was taken."
                                       : "No cooldown is read. Use Skill is done once the skill's key was pressed, "
                                         "held a moment and let go.");
    }
    ImGui::SetCursorScreenPos(ImVec2(column.right - frame, at.y));
    if (plusButton(active ? "##add-active" : "##add-utility", frame, dpi)) {
        asked_ = pass.source->ask ? pass.source->ask() : 0;
        shown_.reset();
        picked_ = -1;
        name_.clear();
        addActive_ = active;
        error_.clear();
        focusName_ = false;
        ImGui::OpenPopup(kAddPopup);
    }
    if (ImGui::IsItemHovered() && !dragging) {
        ImGui::SetTooltip("Add Skill: pick its miniature on the screen");
    }
    float y = at.y + frame + std::round(8.0f * dpi);
    column.cardsTop = y;
    if (count == 0) {
        drawList->AddText(ImVec2(titleAt.x, y), theme::faded(theme::kTextMuted, 0.8f),
                          active ? "None yet: + picks one from the screen (Wind Strike)."
                                 : "None yet: + picks one from the screen (Sit, Pick Up).");
        y += ImGui::GetTextLineHeight() + std::round(8.0f * dpi);
        column.bottom = y;
        return y;
    }

    // Each a card the column's width: the grip it is dragged by, its icon, its name, MP Cost (an active skill's),
    // Shortcut, X.
    const float grip = std::round(16.0f * dpi);
    const float iconSide = std::round(36.0f * dpi);
    const float rounding = std::round(8.0f * dpi);
    const float inner = ImGui::GetStyle().ItemInnerSpacing.x;
    const char* caption = "Shortcut";
    const char* mpCaption = "MP Cost";
    const float keyWidth = std::round(64.0f * dpi);
    const float mpWidth = std::round(52.0f * dpi);
    // From the card's right edge to the first field's caption: X, Shortcut and (active) MP Cost.
    const float controlsWidth = pad + frame + pad * 2.0f + keyWidth + inner + ImGui::CalcTextSize(caption).x +
                                (active ? pad * 2.0f + mpWidth + inner + ImGui::CalcTextSize(mpCaption).x : 0.0f);
    ImGui::PushFont(fonts.medium, 0.0f);
    float widestName = 0.0f;
    for (const statetree::Skill& skill : skills) {
        if (skill.active == active) {
            widestName = std::max(widestName, ImGui::CalcTextSize(skill.name.c_str()).x);
        }
    }
    ImGui::PopFont();
    // Every name whole left of the fields, or, in a column too narrow for that, the fields on a line of their own
    // under the names.
    const float lineGap = std::round(6.0f * dpi);
    const bool oneLine = grip + iconSide + pad * 1.5f + widestName + pad * 2.0f + controlsWidth <= width;
    const float cardHeight = (oneLine ? iconSide : std::max(iconSide, frame * 2.0f + lineGap)) + pad * 2.0f;
    for (std::size_t i = 0; i < skills.size(); ++i) {
        statetree::Skill& skill = skills[i];
        if (skill.active != active) {
            continue;
        }
        const int index = static_cast<int>(i);
        ImGui::PushID(index);
        const ImVec2 min(at.x, y);
        const ImVec2 max(at.x + width, y + cardHeight);
        const bool dragged = dragging && dragging_ == index;
        const ImU32 back = dragged ? theme::kSurfaceActive : theme::kSurface;
        drawList->AddRectFilled(min, max, back, rounding);
        drawList->AddRect(min, max, dragged ? theme::kAccent : theme::kBorder, rounding);
        const ImVec2 iconMin(min.x + grip, min.y + pad);
        const ImVec2 iconMax(iconMin.x + iconSide, iconMin.y + iconSide);
        if (!icons.draw(drawList, skill.name, iconMin, iconMax, std::round(4.0f * dpi), back)) {
            drawList->AddRectFilled(iconMin, iconMax, theme::kSurfaceActive, std::round(4.0f * dpi));
        }
        // The name's line and the fields' line (one, in a wide column).
        const float nameMiddle =
            oneLine ? std::round((min.y + max.y) * 0.5f) : std::round(min.y + pad + frame * 0.5f);
        const float middle = oneLine ? nameMiddle : std::round(nameMiddle + frame + lineGap);
        // (MP Cost,) Shortcut and X at the right; the name between the icon and them (cut short if it must be).
        const float right = max.x - pad;
        const float keyLeft = right - frame - pad * 2.0f - keyWidth;
        const float captionLeft = keyLeft - inner - ImGui::CalcTextSize(caption).x;
        const float mpLeft = captionLeft - pad * 2.0f - mpWidth;
        const float mpCaptionLeft = mpLeft - inner - ImGui::CalcTextSize(mpCaption).x;
        const float fieldsLeft = active ? mpCaptionLeft : captionLeft;
        const float textTop = std::round(middle - ImGui::GetFontSize() * 0.5f);
        ImGui::PushFont(fonts.medium, 0.0f);
        const ImVec4 nameClip(iconMax.x, min.y, oneLine ? fieldsLeft - pad : right, max.y);
        drawList->AddText(ImGui::GetFont(), ImGui::GetFontSize(),
                          ImVec2(iconMax.x + pad * 1.5f, std::round(nameMiddle - ImGui::GetFontSize() * 0.5f)),
                          theme::kText, skill.name.c_str(), nullptr, 0.0f, &nameClip);
        ImGui::PopFont();
        if (active) {
            // The MP it costs (the user asked, 2026-11-15; 2026-11-16: an active skill's Use Skill is done once its
            // MP was taken); empty: not set.
            drawList->AddText(ImVec2(mpCaptionLeft, textTop), theme::kTextMuted, mpCaption);
            ImGui::SetCursorScreenPos(ImVec2(mpLeft, std::round(middle - frame * 0.5f)));
            ImGui::SetNextItemWidth(mpWidth);
            std::string mpText = mpEditing_ == index ? mpEdit_
                                 : skill.mp > 0      ? std::to_string(skill.mp)
                                                     : std::string();
            const bool mpEntered = ImGui::InputTextWithHint("##mp", "\xE2\x80\x93", &mpText,
                                                            ImGuiInputTextFlags_EnterReturnsTrue |
                                                                ImGuiInputTextFlags_CharsDecimal |
                                                                ImGuiInputTextFlags_AutoSelectAll);
            const bool mpActive = ImGui::IsItemActive();
            if ((mpEntered || ImGui::IsItemDeactivatedAfterEdit()) && mpEditing_ == index) {
                int mp = 0;
                const char* first = mpText.data();
                const auto read = std::from_chars(first, first + mpText.size(), mp);
                mp = read.ec == std::errc() ? std::clamp(mp, 0, 99999) : 0; // Empty or not a number: not set.
                if (mp != skill.mp) {
                    skill.mp = mp;
                    pass.changed = true;
                }
                mpEditing_ = -1;
            } else if (mpActive) {
                mpEditing_ = index;
                mpEdit_ = mpText;
            } else if (mpEditing_ == index) {
                mpEditing_ = -1; // Left with Escape: the cost stays.
            }
            if (ImGui::IsItemHovered() && !mpActive && !dragging) {
                ImGui::SetTooltip("The MP it costs: Use Skill is done once that much was taken (not set: once any MP "
                                  "was). Refresh reads it off the game's tooltip.");
            }
        }
        drawList->AddText(ImVec2(captionLeft, textTop), theme::kTextMuted, caption);
        // The key on its hotbar slot, as read while it is ready (or typed: then learned from its slot).
        const bool partly = skill.key.find('?') != std::string::npos;
        ImGui::SetCursorScreenPos(ImVec2(keyLeft, std::round(middle - frame * 0.5f)));
        ImGui::SetNextItemWidth(keyWidth);
        std::string keyText = keyEditing_ == index ? keyEdit_ : skill.key;
        ImGui::PushStyleColor(ImGuiCol_Text, partly ? theme::kWarning : theme::kText);
        const bool keyEntered = ImGui::InputTextWithHint(
            "##key", "\xE2\x80\x93", &keyText,
            ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_CharsUppercase |
                ImGuiInputTextFlags_CharsNoBlank | ImGuiInputTextFlags_AutoSelectAll);
        ImGui::PopStyleColor();
        const bool keyActive = ImGui::IsItemActive();
        if ((keyEntered || ImGui::IsItemDeactivatedAfterEdit()) && keyEditing_ == index) {
            // Empty: the slot has no key. At most what a reading holds (GameGauge's key).
            std::string typed = keyText.empty() ? std::string("-") : keyText.substr(0, 15);
            if (typed != skill.key) {
                skill.key = typed;
                pass.changed = true;
                pass.typedKeys.emplace_back(skill.name, typed);
            }
            keyEditing_ = -1;
        } else if (keyActive) {
            keyEditing_ = index;
            keyEdit_ = keyText;
        } else if (keyEditing_ == index) {
            keyEditing_ = -1; // Left with Escape: the key stays.
        }
        if (ImGui::IsItemHovered() && !keyActive && !dragging) {
            ImGui::SetTooltip("%s", skill.key.empty() ? "Not read yet: it is read off its hotbar slot while it is ready."
                                    : skill.key == "-" ? "No key is written on its hotbar slot."
                                    : partly ? "Letters with ? are not known yet: type the key to teach them."
                                             : "The key on its hotbar slot, for the Pico to press. Follows the skill when "
                                               "you move it to another slot.");
        }
        ImGui::SetCursorScreenPos(ImVec2(right - frame, std::round(middle - frame * 0.5f)));
        if (crossButton("##remove", dpi)) {
            pass.removed = index;
        }
        // The card's own background, under its fields (they came first, so they keep the mouse): dragged by it to the
        // other column or another place; six grip dots at its left say so under the mouse.
        ImGui::SetCursorScreenPos(min);
        ImGui::InvisibleButton("##card", ImVec2(width, cardHeight));
        if (ImGui::IsItemActivated()) {
            dragging_ = index;
            dragMoved_ = false;
        }
        const bool hovered = !dragging && ImGui::IsWindowHovered() && ImGui::IsMouseHoveringRect(min, max);
        if (hovered || dragged) {
            drawGrip(drawList, ImVec2(min.x + grip * 0.5f + std::round(1.0f * dpi), std::round((min.y + max.y) * 0.5f)),
                     dragged ? theme::kText : theme::kTextMuted, dpi);
        }
        column.cards.push_back({index, min.y, max.y});
        ImGui::PopID();
        y = max.y + std::round(6.0f * dpi);
    }
    column.bottom = y;
    return y;
}

int SkillsView::dropColumn(const Pass& pass, bool sideBySide, int* slot) const {
    // Anywhere in the tile: side by side, by which half the mouse is in; else above the Utility header or below it.
    const ImVec2 mouse = ImGui::GetMousePos();
    const ImVec2 windowMin = ImGui::GetWindowPos();
    const ImVec2 windowMax(windowMin.x + ImGui::GetWindowSize().x, windowMin.y + ImGui::GetWindowSize().y);
    if (!ImGui::IsMouseHoveringRect(windowMin, windowMax, false)) {
        return -1; // Out of the tile: let go there, nothing moves.
    }
    const Column& utility = pass.columns[1];
    const int column = sideBySide ? (mouse.x < (pass.columns[0].right + utility.left) * 0.5f ? 0 : 1)
                                  : (mouse.y < utility.top ? 0 : 1);
    // Its place among the column's other cards: after each whose middle is above the mouse.
    *slot = 0;
    for (const Card& card : pass.columns[column].cards) {
        if (card.skill != dragging_ && (card.top + card.bottom) * 0.5f < mouse.y) {
            ++*slot;
        }
    }
    return column;
}

void SkillsView::drawDrag(const theme::Fonts& fonts, const std::vector<statetree::Skill>& skills,
                          const SkillIcons& icons, const Pass& pass, bool sideBySide, float dpi) const {
    ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);
    int slot = 0;
    const int column = dropColumn(pass, sideBySide, &slot);
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    if (column >= 0) {
        // A line where it lands: above the slot's card, under the last, or under the header of an empty column.
        const Column& to = pass.columns[column];
        std::vector<const Card*> others;
        for (const Card& card : to.cards) {
            if (card.skill != dragging_) {
                others.push_back(&card);
            }
        }
        const float half = std::round(3.0f * dpi);
        const float y = others.empty()                              ? to.cardsTop - half
                        : slot < static_cast<int>(others.size()) ? others[static_cast<std::size_t>(slot)]->top - half
                                                                    : others.back()->bottom + half;
        drawList->AddLine(ImVec2(to.left, std::round(y)), ImVec2(to.right, std::round(y)), theme::kAccent,
                          std::round(2.0f * dpi));
    }
    // The card by the mouse: its icon and name.
    const statetree::Skill& skill = skills[static_cast<std::size_t>(dragging_)];
    ImDrawList* front = ImGui::GetForegroundDrawList();
    const ImVec2 mouse = ImGui::GetMousePos();
    const float pad = std::round(6.0f * dpi);
    const float side = std::round(24.0f * dpi);
    ImGui::PushFont(fonts.medium, 0.0f);
    const ImVec2 nameSize = ImGui::CalcTextSize(skill.name.c_str());
    const ImVec2 min(mouse.x + std::round(14.0f * dpi), mouse.y + std::round(10.0f * dpi));
    const ImVec2 max(min.x + pad + side + pad + nameSize.x + pad * 1.5f, min.y + side + pad * 2.0f);
    front->AddRectFilled(min, max, theme::kSurfaceActive, std::round(6.0f * dpi));
    front->AddRect(min, max, column >= 0 ? theme::kAccent : theme::kBorder, std::round(6.0f * dpi));
    const ImVec2 iconMin(min.x + pad, min.y + pad);
    const ImVec2 iconMax(iconMin.x + side, iconMin.y + side);
    if (!icons.draw(front, skill.name, iconMin, iconMax, std::round(3.0f * dpi), theme::kSurfaceActive)) {
        front->AddRectFilled(iconMin, iconMax, theme::kSurfaceHover, std::round(3.0f * dpi));
    }
    front->AddText(ImGui::GetFont(), ImGui::GetFontSize(),
                   ImVec2(iconMax.x + pad, std::round((min.y + max.y - ImGui::GetFontSize()) * 0.5f)), theme::kText,
                   skill.name.c_str());
    ImGui::PopFont();
}

void SkillsView::drop(std::vector<statetree::Skill>& skills, const Pass& pass, bool sideBySide, bool* changed) {
    int slot = 0;
    const int column = dropColumn(pass, sideBySide, &slot);
    if (column < 0 || dragging_ < 0 || dragging_ >= static_cast<int>(skills.size())) {
        return;
    }
    // Out of the list, then in before the slot-th skill of its column (in the list's order), else after its column's
    // last, else at the end.
    std::vector<statetree::Skill> list = skills;
    statetree::Skill moving = std::move(list[static_cast<std::size_t>(dragging_)]);
    list.erase(list.begin() + dragging_);
    moving.active = column == 0;
    std::size_t at = list.size();
    std::size_t afterLast = list.size();
    int seen = 0;
    for (std::size_t i = 0; i < list.size(); ++i) {
        if (list[i].active != moving.active) {
            continue;
        }
        if (seen == slot) {
            at = i;
            break;
        }
        ++seen;
        afterLast = i + 1;
    }
    if (at == list.size() && seen > 0) {
        at = afterLast;
    }
    list.insert(list.begin() + static_cast<std::ptrdiff_t>(at), std::move(moving));
    if (list != skills) {
        skills = std::move(list);
        *changed = true;
    }
}

void SkillsView::drawAddSkill(const theme::Fonts& fonts, ID3D11Device* device, StateTreeView& profile,
                              const MiniatureSource& source, float dpi) {
    // The miniatures, once the finder answers.
    if (!shown_ && asked_ != 0 && source.newest) {
        if (const UiMiniatures* newest = source.newest(); newest != nullptr && newest->request == asked_) {
            shown_ = std::make_unique<UiMiniatures>(*newest);
            if (shown_->count > 0) {
                try {
                    if (!atlas_) {
                        atlas_ = std::make_unique<ScreenImage>(device, UiMiniatures::kMax * UiMiniatures::kSide,
                                                               UiMiniatures::kSide);
                    }
                    atlas_->upload(shown_->bgra.data(), UiMiniatures::kPitch);
                } catch (const std::exception& e) {
                    atlas_.reset();
                    error_ = e.what();
                }
            }
        }
    }

    ImGui::PushFont(fonts.medium, 0.0f);
    ImGui::TextUnformatted(addActive_ ? "Add Active Skill" : "Add Utility Skill");
    ImGui::PopFont();
    ImGui::TextDisabled("Pick its miniature while the skill is ready, then name it.");
    ImGui::Dummy(ImVec2(0.0f, std::round(2.0f * dpi)));
    if (asked_ == 0) {
        ImGui::TextDisabled("The screen is not being looked at.");
    } else if (!shown_) {
        ImGui::TextDisabled("Looking\xE2\x80\xA6");
    } else if (shown_->count == 0 || !atlas_) {
        ImGui::TextDisabled("No miniatures on the screen.");
    } else {
        // The miniatures, the hotbar's first, eight a row.
        constexpr int kPerRow = 8;
        const float cell = std::round(46.0f * dpi);
        const float inset = std::round(3.0f * dpi);
        const float gap = std::round(6.0f * dpi);
        ImDrawList* drawList = ImGui::GetWindowDrawList();
        const auto atlasWidth = static_cast<float>(UiMiniatures::kMax * UiMiniatures::kSide);
        for (int i = 0; i < shown_->count; ++i) {
            if (i % kPerRow != 0) {
                ImGui::SameLine(0.0f, gap);
            }
            ImGui::PushID(i);
            const ImVec2 min = ImGui::GetCursorScreenPos();
            if (ImGui::InvisibleButton("##miniature", ImVec2(cell, cell))) {
                picked_ = i;
                focusName_ = true;
            }
            const bool hovered = ImGui::IsItemHovered();
            ImGui::PopID();
            const ImVec2 max(min.x + cell, min.y + cell);
            const bool picked = picked_ == i;
            const float rounding = std::round(5.0f * dpi);
            drawList->AddRectFilled(min, max, picked ? theme::kSurfaceOpenHover : theme::kSurfaceActive, rounding);
            const StatsArea& box = shown_->boxes[static_cast<std::size_t>(i)];
            const auto side = static_cast<float>(box.x1 - box.x0);
            const float left = static_cast<float>(i * UiMiniatures::kSide);
            atlas_->draw(drawList, ImVec2(min.x + inset, min.y + inset), ImVec2(max.x - inset, max.y - inset),
                         std::round(2.0f * dpi), picked ? theme::kSurfaceOpenHover : theme::kSurfaceActive,
                         ImVec2(left / atlasWidth, 0.0f),
                         ImVec2((left + side) / atlasWidth, side / static_cast<float>(UiMiniatures::kSide)));
            if (picked || hovered) {
                drawList->AddRect(min, max, picked ? theme::kAccent : theme::kBorderHover, rounding, 0,
                                  picked ? std::round(2.0f * dpi) : 1.0f);
            }
        }
    }

    ImGui::Dummy(ImVec2(0.0f, std::round(4.0f * dpi)));
    ImGui::SetNextItemWidth(std::round(240.0f * dpi));
    if (focusName_) {
        ImGui::SetKeyboardFocusHere();
        focusName_ = false;
    }
    const bool entered =
        ImGui::InputTextWithHint("##skill-name", addActive_ ? "Its name, e.g. Wind Strike" : "Its name, e.g. Pick Up",
                                 &name_, ImGuiInputTextFlags_EnterReturnsTrue);
    const std::string name = trimmed(name_);
    const std::string problem = nameProblem(name);
    std::vector<statetree::Skill>& skills = profile.skills();
    const auto same = std::find_if(skills.begin(), skills.end(),
                                   [&](const statetree::Skill& skill) { return sameText(skill.name, name); });
    const bool replaces = same != skills.end();
    const std::string replaced = replaces ? same->name : std::string();
    const bool can = shown_ && picked_ >= 0 && picked_ < shown_->count && !name.empty() && problem.empty();
    ImGui::SameLine();
    ImGui::BeginDisabled(!can);
    const bool clicked = ImGui::Button(replaces ? "Replace" : "Add");
    ImGui::EndDisabled();
    if ((clicked || entered) && can) {
        // Its colour pixels into the profile (shown as its icon; the reader takes them grey).
        const StatsArea& box = shown_->boxes[static_cast<std::size_t>(picked_)];
        statetree::Skill skill;
        skill.name = name;
        skill.active = addActive_; // The column whose + opened it.
        skill.side = box.x1 - box.x0;
        skill.bgr.resize(static_cast<std::size_t>(skill.side) * static_cast<std::size_t>(skill.side) * 3);
        for (int y = 0; y < skill.side; ++y) {
            const std::uint8_t* row = shown_->bgra.data() + static_cast<std::size_t>(y) * UiMiniatures::kPitch +
                                      static_cast<std::size_t>(picked_ * UiMiniatures::kSide) * 4;
            for (int x = 0; x < skill.side; ++x) {
                const std::size_t at = static_cast<std::size_t>(y * skill.side + x) * 3;
                skill.bgr[at] = row[x * 4];
                skill.bgr[at + 1] = row[x * 4 + 1];
                skill.bgr[at + 2] = row[x * 4 + 2];
            }
        }
        if (replaces) {
            *same = std::move(skill);
        } else {
            skills.push_back(std::move(skill));
        }
        profile.saveSkills();
        ImGui::CloseCurrentPopup();
    } else if (!problem.empty()) {
        ImGui::TextDisabled("%s", problem.c_str());
    } else if (replaces) {
        ImGui::TextDisabled("Replaces the skill %s.", replaced.c_str());
    }
    if (!error_.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, theme::kRecord);
        ImGui::TextUnformatted(error_.c_str());
        ImGui::PopStyleColor();
    }
}

} // namespace observex
