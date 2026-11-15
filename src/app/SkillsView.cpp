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
    // The profile at the top left as in the State tile, + Add Skill and Refresh after it (under it in a narrow tile).
    const float left = ImGui::GetCursorScreenPos().x;
    const float rowTop = ImGui::GetCursorScreenPos().y;
    profile.drawProfileBar(fonts, folder);
    BarButton add;
    add.icon = Icon::Plus;
    add.text = "Add Skill";
    add.open = ImGui::IsPopupOpen(kAddPopup);
    BarButton again;
    again.icon = Icon::Refresh;
    again.text = "Refresh";
    again.open = refresh.running; // Highlighted while it runs, how far along its bottom; a click stops it.
    again.progress = refresh.running ? refresh.progress : -1.0f;
    const float gap = std::round(12.0f * dpi);
    const float buttonGap = std::round(8.0f * dpi);
    ImGui::SameLine(0.0f, gap);
    if (add.width(fonts) + buttonGap + again.width(fonts) <= ImGui::GetContentRegionAvail().x) {
        ImGui::SetCursorScreenPos(ImVec2(ImGui::GetCursorScreenPos().x, rowTop));
    } else {
        ImGui::SetCursorScreenPos(ImVec2(left, rowTop + std::round(BarButton::kHeight * dpi) + std::round(8.0f * dpi)));
    }
    if (add.draw("##add-skill-button", fonts)) {
        asked_ = source.ask ? source.ask() : 0;
        shown_.reset();
        picked_ = -1;
        name_.clear();
        cooldown_ = true;
        error_.clear();
        focusName_ = false;
        ImGui::OpenPopup(kAddPopup);
    }
    const ImVec2 popupAt(ImGui::GetItemRectMin().x, ImGui::GetItemRectMax().y + std::round(6.0f * dpi));
    const float barBottom = ImGui::GetItemRectMax().y;
    ImGui::SetCursorScreenPos(ImVec2(ImGui::GetItemRectMax().x + buttonGap, ImGui::GetItemRectMin().y)); // Right of it.
    if (again.draw("##refresh-skills", fonts) && refresh.click) {
        refresh.click();
    }
    ImGui::SetNextWindowPos(popupAt);
    if (ImGui::BeginPopup(kAddPopup)) {
        drawAddSkill(fonts, device, profile, source, dpi);
        ImGui::EndPopup();
    }
    ImGui::SetCursorScreenPos(ImVec2(left, barBottom + std::round(8.0f * dpi)));
    if (!profile.fileError().empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, theme::kRecord);
        ImGui::TextWrapped("%s", profile.fileError().c_str());
        ImGui::PopStyleColor();
    }
    ImGui::SetCursorScreenPos(ImVec2(left, ImGui::GetCursorScreenPos().y + std::round(4.0f * dpi)));

    std::vector<statetree::Skill>& skills = profile.skills();
    if (skills.empty()) {
        ImGui::TextDisabled("No skills yet: Add Skill picks one from the screen.");
        return;
    }
    // Each a card the tile's width: its icon, its name, MP Cost, Shortcut, Has Cooldown, X.
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    const float width = ImGui::GetContentRegionAvail().x;
    const float pad = std::round(8.0f * dpi);
    const float iconSide = std::round(36.0f * dpi);
    const float rounding = std::round(8.0f * dpi);
    const float frame = ImGui::GetFrameHeight();
    const float inner = ImGui::GetStyle().ItemInnerSpacing.x;
    const char* label = "Has Cooldown";
    const char* caption = "Shortcut";
    const char* mpCaption = "MP Cost";
    const float checkWidth = frame + inner + ImGui::CalcTextSize(label).x;
    const float keyWidth = std::round(64.0f * dpi);
    const float mpWidth = std::round(52.0f * dpi);
    // From the card's right edge to MP Cost's caption: X, Has Cooldown, Shortcut and MP Cost.
    const float controlsWidth = pad + frame + pad + checkWidth + pad * 2.0f + keyWidth + inner +
                                ImGui::CalcTextSize(caption).x + pad * 2.0f + mpWidth + inner +
                                ImGui::CalcTextSize(mpCaption).x;
    ImGui::PushFont(fonts.medium, 0.0f);
    float widestName = 0.0f;
    for (const statetree::Skill& skill : skills) {
        widestName = std::max(widestName, ImGui::CalcTextSize(skill.name.c_str()).x);
    }
    ImGui::PopFont();
    // Every name whole left of the fields, or, in a tile too narrow for that, the fields on a line of their own under
    // the names (MP Cost had squeezed the names out of a narrow tile).
    const float lineGap = std::round(6.0f * dpi);
    const bool oneLine = pad + iconSide + pad * 1.5f + widestName + pad * 2.0f + controlsWidth <= width;
    const float cardHeight = (oneLine ? iconSide : std::max(iconSide, frame * 2.0f + lineGap)) + pad * 2.0f;
    int removed = -1;
    bool changed = false;
    std::vector<std::pair<std::string, std::string>> typedKeys; // Skill, key: learned once saved.
    for (std::size_t i = 0; i < skills.size(); ++i) {
        statetree::Skill& skill = skills[i];
        ImGui::PushID(static_cast<int>(i));
        const ImVec2 min = ImGui::GetCursorScreenPos();
        const ImVec2 max(min.x + width, min.y + cardHeight);
        drawList->AddRectFilled(min, max, theme::kSurface, rounding);
        drawList->AddRect(min, max, theme::kBorder, rounding);
        const ImVec2 iconMin(min.x + pad, min.y + pad);
        const ImVec2 iconMax(iconMin.x + iconSide, iconMin.y + iconSide);
        if (!icons.draw(drawList, skill.name, iconMin, iconMax, std::round(4.0f * dpi), theme::kSurface)) {
            drawList->AddRectFilled(iconMin, iconMax, theme::kSurfaceActive, std::round(4.0f * dpi));
        }
        // The name's line and the fields' line (one, in a wide tile).
        const float nameMiddle =
            oneLine ? std::round((min.y + max.y) * 0.5f) : std::round(min.y + pad + frame * 0.5f);
        const float middle = oneLine ? nameMiddle : std::round(nameMiddle + frame + lineGap);
        // MP Cost, Shortcut, Has Cooldown and X at the right; the name between the icon and them (cut short if it must
        // be).
        const float right = max.x - pad;
        const float checkLeft = right - frame - pad - checkWidth;
        const float keyLeft = checkLeft - pad * 2.0f - keyWidth;
        const float captionLeft = keyLeft - inner - ImGui::CalcTextSize(caption).x;
        const float mpLeft = captionLeft - pad * 2.0f - mpWidth;
        const float mpCaptionLeft = mpLeft - inner - ImGui::CalcTextSize(mpCaption).x;
        const float textTop = std::round(middle - ImGui::GetFontSize() * 0.5f);
        ImGui::PushFont(fonts.medium, 0.0f);
        const ImVec4 nameClip(iconMax.x, min.y, oneLine ? mpCaptionLeft - pad : right, max.y);
        drawList->AddText(ImGui::GetFont(), ImGui::GetFontSize(),
                          ImVec2(iconMax.x + pad * 1.5f, std::round(nameMiddle - ImGui::GetFontSize() * 0.5f)),
                          theme::kText, skill.name.c_str(), nullptr, 0.0f, &nameClip);
        ImGui::PopFont();
        // The MP it costs (the user asked, 2026-11-15: Use Skill is done once the game took it); empty: not set.
        drawList->AddText(ImVec2(mpCaptionLeft, textTop), theme::kTextMuted, mpCaption);
        ImGui::SetCursorScreenPos(ImVec2(mpLeft, std::round(middle - frame * 0.5f)));
        ImGui::SetNextItemWidth(mpWidth);
        std::string mpText = mpEditing_ == static_cast<int>(i) ? mpEdit_
                             : skill.mp > 0                    ? std::to_string(skill.mp)
                                                               : std::string();
        const bool mpEntered = ImGui::InputTextWithHint(
            "##mp", "\xE2\x80\x93", &mpText,
            ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_CharsDecimal | ImGuiInputTextFlags_AutoSelectAll);
        const bool mpActive = ImGui::IsItemActive();
        if ((mpEntered || ImGui::IsItemDeactivatedAfterEdit()) && mpEditing_ == static_cast<int>(i)) {
            int mp = 0;
            const char* first = mpText.data();
            const auto read = std::from_chars(first, first + mpText.size(), mp);
            mp = read.ec == std::errc() ? std::clamp(mp, 0, 99999) : 0; // Empty or not a number: not set.
            if (mp != skill.mp) {
                skill.mp = mp;
                changed = true;
            }
            mpEditing_ = -1;
        } else if (mpActive) {
            mpEditing_ = static_cast<int>(i);
            mpEdit_ = mpText;
        } else if (mpEditing_ == static_cast<int>(i)) {
            mpEditing_ = -1; // Left with Escape: the cost stays.
        }
        if (ImGui::IsItemHovered() && !mpActive) {
            ImGui::SetTooltip("The MP it costs: Use Skill is done once that much MP was taken (or its cast ended).");
        }
        drawList->AddText(ImVec2(captionLeft, textTop), theme::kTextMuted, caption);
        // The key on its hotbar slot, as read while it is ready (or typed: then learned from its slot).
        const bool partly = skill.key.find('?') != std::string::npos;
        ImGui::SetCursorScreenPos(ImVec2(keyLeft, std::round(middle - frame * 0.5f)));
        ImGui::SetNextItemWidth(keyWidth);
        std::string keyText = keyEditing_ == static_cast<int>(i) ? keyEdit_ : skill.key;
        ImGui::PushStyleColor(ImGuiCol_Text, partly ? theme::kWarning : theme::kText);
        const bool keyEntered = ImGui::InputTextWithHint(
            "##key", "\xE2\x80\x93", &keyText,
            ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_CharsUppercase |
                ImGuiInputTextFlags_CharsNoBlank | ImGuiInputTextFlags_AutoSelectAll);
        ImGui::PopStyleColor();
        const bool keyActive = ImGui::IsItemActive();
        if ((keyEntered || ImGui::IsItemDeactivatedAfterEdit()) && keyEditing_ == static_cast<int>(i)) {
            // Empty: the slot has no key. At most what a reading holds (GameGauge's key).
            std::string typed = keyText.empty() ? std::string("-") : keyText.substr(0, 15);
            if (typed != skill.key) {
                skill.key = typed;
                changed = true;
                typedKeys.emplace_back(skill.name, typed);
            }
            keyEditing_ = -1;
        } else if (keyActive) {
            keyEditing_ = static_cast<int>(i);
            keyEdit_ = keyText;
        } else if (keyEditing_ == static_cast<int>(i)) {
            keyEditing_ = -1; // Left with Escape: the key stays.
        }
        if (ImGui::IsItemHovered() && !keyActive) {
            ImGui::SetTooltip("%s", skill.key.empty() ? "Not read yet: it is read off its hotbar slot while it is ready."
                                    : skill.key == "-" ? "No key is written on its hotbar slot."
                                    : partly ? "Letters with ? are not known yet: type the key to teach them."
                                             : "The key on its hotbar slot, for the Pico to press. Follows the skill when "
                                               "you move it to another slot.");
        }
        ImGui::SetCursorScreenPos(ImVec2(checkLeft, std::round(middle - frame * 0.5f)));
        if (ImGui::Checkbox(label, &skill.cooldown)) {
            changed = true;
        }
        ImGui::SetCursorScreenPos(ImVec2(right - frame, std::round(middle - frame * 0.5f)));
        if (crossButton("##remove", dpi)) {
            removed = static_cast<int>(i);
        }
        ImGui::PopID();
        ImGui::SetCursorScreenPos(ImVec2(min.x, max.y + std::round(6.0f * dpi)));
    }
    ImGui::Dummy(ImVec2(width, 0.0f));
    if (removed >= 0) {
        skills.erase(skills.begin() + removed);
        changed = true;
    }
    if (changed) {
        profile.saveSkills();
    }
    for (const auto& [name, key] : typedKeys) {
        if (keyTyped) {
            keyTyped(name, key);
        }
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
    ImGui::TextUnformatted("Add Skill");
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
    const bool entered = ImGui::InputTextWithHint("##skill-name", "Its name, e.g. Wind Strike", &name_,
                                                  ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SameLine();
    ImGui::Checkbox("Has Cooldown", &cooldown_);
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
        skill.cooldown = cooldown_;
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
