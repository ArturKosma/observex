#include "app/StateTreeView.h"

#include "app/Widgets.h"

#include <imgui_internal.h>        // ImGui::SetKeyOwner, to keep the keys the tile takes from the review and clips.
#include <misc/cpp/imgui_stdlib.h> // ImGui::InputText on a std::string.

#include <algorithm>
#include <cctype>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <functional>
#include <iterator>
#include <string_view>
#include <system_error>
#include <vector>

namespace observex {

namespace {

using statetree::Compare;
using statetree::Condition;
using statetree::Join;
using statetree::Kind;
using statetree::SkillIs;
using statetree::Operand;
using statetree::Row;
using statetree::State;
using statetree::Transition;
using statetree::Variable;

constexpr char kMenu[] = "##state-menu";
constexpr char kLoadPopup[] = "##load-brain";
constexpr char kKindsPopup[] = "##condition-kinds";
constexpr char kTasksPopup[] = "##task-kinds";
// Before DPI scaling.
constexpr float kBoxHeight = 32.0f;
constexpr float kBoxMinWidth = 96.0f;
constexpr float kRowGap = 6.0f;
constexpr float kIndent = 24.0f;
constexpr float kPadding = 12.0f;  // Each side of the name in its box.
constexpr float kRounding = 8.0f;  // As the top bar's buttons.
constexpr float kSplitGap = 6.0f;  // Between the tree and the details: their line, and room to grab it.
constexpr float kFieldWidth = 240.0f; // The details' name fields.
constexpr float kPi = 3.14159265f;

float dpiScale() {
    return ImGui::GetStyle().FontScaleDpi;
}

std::string trimmed(std::string_view text) {
    const std::size_t first = text.find_first_not_of(" \t");
    if (first == std::string_view::npos) {
        return {};
    }
    return std::string(text.substr(first, text.find_last_not_of(" \t") - first + 1));
}

std::filesystem::path pathOf(const std::string& utf8) {
    return std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(utf8.data()), utf8.size()));
}

std::string utf8Of(const std::filesystem::path& path) {
    const std::u8string text = path.u8string();
    return std::string(reinterpret_cast<const char*>(text.data()), text.size());
}

// A square X, the frame's height; true when clicked.
bool crossButton(const char* id) {
    const float dpi = dpiScale();
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

// The arrow before a transition's target, from `x`, its middle at `y`; returns its width. Straight on, or (`back`)
// turning back, for a transition to the state itself or one above it (as UE's). A 1 px line on pixel centres and a
// filled head, so it stays crisp this small (the user, 2026-11-13: the 1.5 px open chevron was "weird and ugly").
float drawArrow(ImDrawList* drawList, float x, float y, bool back, ImU32 colour) {
    const float dpi = dpiScale();
    const float length = std::round(16.0f * dpi);
    const float head = std::round(5.0f * dpi);  // The head's length,
    const float half = std::round(3.5f * dpi);  // and half its height.
    const float middle = std::floor(y) + 0.5f;
    if (!back) {
        drawList->AddLine(ImVec2(x, middle), ImVec2(x + length - head, middle), colour, 1.0f);
        drawList->AddTriangleFilled(ImVec2(x + length - head, middle - half), ImVec2(x + length, middle),
                                    ImVec2(x + length - head, middle + half), colour);
        return length;
    }
    // Right along the bottom, round, and back left along the top to a head pointing back.
    const float radius = std::round(4.0f * dpi);
    const float turn = x + length - radius;
    // (The arc starts at (turn, middle + radius): a point of its own there too would blur the bottom line.)
    drawList->PathLineTo(ImVec2(x, middle + radius));
    drawList->PathArcTo(ImVec2(turn, middle), radius, 0.5f * kPi, -0.5f * kPi, 12);
    drawList->PathLineTo(ImVec2(x + head, middle - radius));
    drawList->PathStroke(colour, 0, 1.0f);
    drawList->AddTriangleFilled(ImVec2(x + head, middle - radius - half), ImVec2(x, middle - radius),
                                ImVec2(x + head, middle - radius + half), colour);
    return length;
}

} // namespace

void StateTreeView::draw(const theme::Fonts& fonts, const std::filesystem::path& folder) {
    const float dpi = dpiScale();
    if (folder != folder_) {
        openProfile(folder); // At start, when no profile was loaded, and when Settings' brains folder changes.
    }
    // Ctrl+Z undoes the last change to the tree, Ctrl+Y or Ctrl+Shift+Z redoes it (held, they repeat); a field being
    // typed in keeps them for its text. Any owner: the tile itself locks the keys held, at the end of each frame.
    if (ImGui::IsWindowFocused(ImGuiFocusedFlags_ChildWindows) && !ImGui::IsAnyItemActive()) {
        if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_Z, ImGuiInputFlags_Repeat, ImGuiKeyOwner_Any)) {
            stepped(history_.undo(&tree_));
        } else if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_Y, ImGuiInputFlags_Repeat, ImGuiKeyOwner_Any) ||
                   ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_Z, ImGuiInputFlags_Repeat,
                                            ImGuiKeyOwner_Any)) {
            stepped(history_.redo(&tree_));
        }
    }
    drawToolbar(fonts, folder);
    if (!fileError_.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, theme::kRecord);
        ImGui::TextWrapped("%s", fileError_.c_str());
        ImGui::PopStyleColor();
    }
    ImGui::Dummy(ImVec2(0.0f, std::round(2.0f * dpi)));

    // The tree on the left, the selected state's details on the right, the line between dragging. With nothing
    // selected the tree takes the whole width: no details, no space kept for them (the user asked, 2026-11-13: it cut
    // Root in a narrow tile). Decided before the tree, where a click may change the selection (shown next frame).
    const bool details = tree_.find(selected_) != nullptr;
    const ImVec2 area = ImGui::GetContentRegionAvail();
    const ImVec2 at = ImGui::GetCursorScreenPos();
    const float gap = std::round(kSplitGap * dpi);
    const float height = std::max(1.0f, area.y);
    const float treeWidth =
        details ? std::round(std::max(1.0f, (area.x - gap) * treeShare_)) : std::max(1.0f, area.x);
    ImGui::BeginChild("##tree", ImVec2(treeWidth, height), ImGuiChildFlags_None,
                      ImGuiWindowFlags_HorizontalScrollbar);
    drawTree(fonts);
    ImGui::EndChild();

    if (!details || tree_.find(selected_) == nullptr) {
        keepChanges();
        return keepKeys(); // Nothing selected: nothing on the right (the user asked, 2026-11-13).
    }
    ImGui::SetCursorScreenPos(ImVec2(at.x + treeWidth, at.y));
    ImGui::InvisibleButton("##split", ImVec2(gap, height));
    const bool dragged = ImGui::IsItemActive();
    const bool hovered = ImGui::IsItemHovered();
    if (hovered || dragged) {
        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
    }
    if (dragged && area.x > gap) {
        treeShare_ = std::clamp((ImGui::GetIO().MousePos.x - gap * 0.5f - at.x) / (area.x - gap), 0.15f, 0.85f);
    }
    const float lineX = std::round(at.x + treeWidth + gap * 0.5f);
    ImGui::GetWindowDrawList()->AddRectFilled(
        ImVec2(lineX, at.y), ImVec2(lineX + 1.0f, at.y + height),
        dragged ? theme::kAccent : hovered ? theme::kBorderHover : theme::kBorder);

    ImGui::SetCursorScreenPos(ImVec2(at.x + treeWidth + gap, at.y));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(std::round(10.0f * dpi), 0.0f));
    ImGui::BeginChild("##details", ImVec2(std::max(1.0f, area.x - treeWidth - gap), height),
                      ImGuiChildFlags_AlwaysUseWindowPadding);
    ImGui::PopStyleVar();
    drawDetails(fonts);
    ImGui::EndChild();
    keepChanges();
    keepKeys();
}

void StateTreeView::keepChanges() {
    // Every change goes into the profile as it is made (the user asked, 2026-11-13: "every node or detail added
    // triggers the save"), and is a step Ctrl+Z undoes; a field being typed in is one change, once it is left.
    if (!ImGui::IsAnyItemActive() && history_.keep(tree_)) {
        write();
    }
}

void StateTreeView::flush() {
    if (history_.keep(tree_)) { // A field still being typed in.
        write();
    }
}

void StateTreeView::stepped(bool changed) {
    if (changed) { // Undone or redone: the tree is the history's, and so is the file.
        renaming_ = 0;
        write();
    }
}

namespace {

// `name`, or, when another profile has it, the next one free (the user asked, 2026-11-13): the number at its end
// counted on ("Mage 2" -> "Mage 3"), or " 0" added ("Mage" -> "Mage 0", then "Mage 1"). `self`, the profile's own file,
// does not count as another.
std::string freeName(const std::filesystem::path& folder, const std::string& name,
                     const std::filesystem::path& self = {}) {
    const auto taken = [&](const std::string& candidate) {
        std::error_code error;
        const std::filesystem::path file = folder / pathOf(candidate + statetree::kTreeExtension);
        return std::filesystem::exists(file, error) && (self.empty() || !std::filesystem::equivalent(file, self, error));
    };
    if (!taken(name)) {
        return name;
    }
    std::size_t digits = name.size();
    while (digits > 0 && std::isdigit(static_cast<unsigned char>(name[digits - 1])) != 0) {
        --digits;
    }
    std::string base = name + " ";
    long long number = 0;
    if (digits < name.size() && name.size() - digits <= 9) {
        base = name.substr(0, digits);
        number = std::stoll(name.substr(digits)) + 1;
    }
    for (;; ++number) {
        const std::string candidate = base + std::to_string(number);
        if (!taken(candidate)) {
            return candidate;
        }
    }
}

} // namespace

void StateTreeView::openProfile(const std::filesystem::path& folder) {
    // Always a profile (the user asked, 2026-11-13: "we always work on a profile"): the newest one in the folder that
    // reads, or, with none, the tree as it is, saved at once as "Brain" ("Brain 0", ... if that file is there).
    folder_ = folder;
    std::vector<std::pair<std::filesystem::file_time_type, std::string>> found;
    std::error_code error;
    for (const auto& entry : std::filesystem::directory_iterator(folder, error)) {
        if (entry.is_regular_file(error) && entry.path().extension() == statetree::kTreeExtension) {
            found.emplace_back(entry.last_write_time(error), utf8Of(entry.path().stem()));
        }
    }
    std::ranges::sort(found, std::greater{});
    for (const auto& [time, name] : found) {
        if (load(folder, name)) {
            return;
        }
    }
    name_ = freeName(folder, "Brain");
    history_.reset(tree_);
    write();
}

void StateTreeView::write() {
    if (name_.empty()) {
        return;
    }
    std::error_code error;
    std::filesystem::create_directories(folder_, error);
    const std::filesystem::path file = folder_ / pathOf(name_ + statetree::kTreeExtension);
    std::filesystem::path temporary = file;
    temporary += L".tmp";
    {
        std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
        out << history_.text();
        out.flush();
        if (!out) {
            fileError_ = "Could not save into " + utf8Of(folder_);
            return;
        }
    }
    std::filesystem::rename(temporary, file, error); // In one go: a write cut short leaves the one before.
    if (error) {
        fileError_ = "Could not save " + name_ + statetree::kTreeExtension + ": " + error.message();
        return;
    }
    fileError_.clear();
}

void StateTreeView::keepKeys() {
    // While the tile has the keyboard, the keys held are its own for the frame: the review and a clip (drawn after it,
    // reading keys whichever window has them) do not act on them too.
    if (ImGui::IsWindowFocused(ImGuiFocusedFlags_ChildWindows)) {
        const ImGuiID owner = ImGui::GetID("##keys");
        for (int key = ImGuiKey_Keyboard_BEGIN; key < ImGuiKey_Keyboard_END; ++key) {
            if (ImGui::IsKeyDown(static_cast<ImGuiKey>(key))) {
                ImGui::SetKeyOwner(static_cast<ImGuiKey>(key), owner, ImGuiInputFlags_LockThisFrame);
            }
        }
    }
}

void StateTreeView::tick(const statetree::Variables& variables) {
    variables_ = variables;
    runner_.tick(tree_, variables);
}

void StateTreeView::stop() {
    runner_.reset(); // Root again, the next reading on.
    variables_ = {};
}

bool StateTreeView::load(const std::filesystem::path& folder, const std::string& name) {
    std::ifstream in(folder / pathOf(name + statetree::kTreeExtension), std::ios::binary);
    if (!in) {
        fileError_ = "Could not open " + name + statetree::kTreeExtension;
        return false;
    }
    const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    std::string why;
    std::optional<statetree::Tree> tree = statetree::readTree(text, &why);
    if (!tree) {
        fileError_ = name + statetree::kTreeExtension + " is not a brain observex can read. " + why;
        return false;
    }
    tree_ = std::move(*tree);
    runner_.reset(); // Root first.
    selected_ = 0;
    renaming_ = 0;
    name_ = name;
    folder_ = folder;
    history_.reset(tree_); // Nothing to undo into another profile.
    fileError_.clear();
    return true;
}

void StateTreeView::renameProfile(const std::string& name) {
    // The profile's title, typed in its field (the user asked, 2026-11-13: profiles need names, say "Basic Combat -
    // Mage", shown "inside a type box", which "also renames the file it saves to/from").
    const std::string clean = trimmed(name);
    if (clean == name_) {
        return;
    }
    if (clean.empty() || clean.find_first_of("\\/:*?\"<>|") != std::string::npos) {
        fileError_ = "A profile's name needs letters, and none of \\ / : * ? \" < > |";
        return;
    }
    std::error_code error;
    const std::filesystem::path from = folder_ / pathOf(name_ + statetree::kTreeExtension);
    const std::string target = freeName(folder_, clean, from); // Never over another profile: the next number.
    if (target == name_) {
        return;
    }
    if (std::filesystem::exists(from, error)) {
        std::filesystem::rename(from, folder_ / pathOf(target + statetree::kTreeExtension), error);
        if (error) {
            fileError_ = "Could not rename " + name_ + statetree::kTreeExtension + ": " + error.message();
            return;
        }
    }
    name_ = target;
    history_.keep(tree_); // A change not kept yet goes in too.
    write();
}

void StateTreeView::drawToolbar(const theme::Fonts& fonts, const std::filesystem::path& folder) {
    const float dpi = dpiScale();
    const float gap = std::round(8.0f * dpi);
    const float popupGap = std::round(6.0f * dpi);

    // Load: the trees saved in the folder.
    BarButton loadButton;
    loadButton.icon = Icon::Folder;
    loadButton.text = "Load";
    loadButton.arrow = true;
    loadButton.open = ImGui::IsPopupOpen(kLoadPopup);
    if (loadButton.draw("##load", fonts)) {
        saved_.clear();
        std::error_code error;
        for (const auto& entry : std::filesystem::directory_iterator(folder, error)) {
            if (entry.is_regular_file(error) && entry.path().extension() == statetree::kTreeExtension) {
                saved_.push_back(utf8Of(entry.path().stem()));
            }
        }
        std::ranges::sort(saved_);
        ImGui::OpenPopup(kLoadPopup);
    }
    const ImVec2 loadCorner(ImGui::GetItemRectMin().x, ImGui::GetItemRectMax().y + popupGap);

    // The profile's name, in a field after it: Enter, or leaving the field, renames the profile and its file; Escape
    // keeps the name it had. No Save: every change is saved by itself (the user asked, 2026-11-13; Add State, which
    // was here too, is under the right click).
    ImGui::SameLine(0.0f, gap);
    const float barTop = ImGui::GetItemRectMin().y;
    const float barBottom = ImGui::GetItemRectMax().y;
    ImGui::SetCursorScreenPos(ImVec2(ImGui::GetCursorScreenPos().x,
                                     std::round((barTop + barBottom - ImGui::GetFrameHeight()) * 0.5f)));
    ImGui::SetNextItemWidth(std::round(kFieldWidth * dpi));
    const bool entered = ImGui::InputText("##profile", &title_, ImGuiInputTextFlags_EnterReturnsTrue);
    if (entered || ImGui::IsItemDeactivatedAfterEdit()) {
        renameProfile(title_);
    }
    if (!ImGui::IsItemActive()) {
        title_ = name_; // The profile's name whenever it is not being typed in (a refused one goes back).
    }

    ImGui::SetNextWindowPos(loadCorner);
    if (ImGui::BeginPopup(kLoadPopup)) {
        if (saved_.empty()) {
            ImGui::TextDisabled("Nothing saved yet");
        }
        for (const std::string& name : saved_) {
            if (ImGui::Selectable(name.c_str(), name == name_)) {
                load(folder, name);
                ImGui::CloseCurrentPopup();
            }
        }
        ImGui::Dummy(ImVec2(0.0f, std::round(2.0f * dpi)));
        ImGui::TextDisabled("In %s", utf8Of(folder).c_str());
        ImGui::EndPopup();
    }
}

void StateTreeView::drawTree(const theme::Fonts& fonts) {
    const float dpi = dpiScale();

    // Where each state's box goes: one under another, indented by depth.
    const std::vector<Row> rows = tree_.rows();
    const float height = std::round(kBoxHeight * dpi);
    const float gap = std::round(kRowGap * dpi);
    const float indent = std::round(kIndent * dpi);
    const float padding = std::round(kPadding * dpi);
    const float fontSize = ImGui::GetFontSize();
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    std::vector<ImVec2> mins(rows.size());
    std::vector<ImVec2> maxes(rows.size());
    for (std::size_t i = 0; i < rows.size(); ++i) {
        const std::string& name = tree_.find(rows[i].state)->name;
        const float textWidth = fonts.medium->CalcTextSizeA(fontSize, FLT_MAX, 0.0f, name.c_str()).x;
        mins[i] = ImVec2(origin.x + indent * static_cast<float>(rows[i].depth),
                         origin.y + (height + gap) * static_cast<float>(i));
        maxes[i] = ImVec2(mins[i].x + std::round(std::max(textWidth + padding * 2.0f, kBoxMinWidth * dpi)),
                          mins[i].y + height);
    }

    // The lines first, under the boxes: from under each parent's box down and across to each of its children.
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    const float thickness = std::round(dpi);
    for (std::size_t i = 1; i < rows.size(); ++i) {
        std::size_t parent = i - 1;
        while (rows[parent].depth >= rows[i].depth) { // The nearest row above, one less deep: its parent.
            --parent;
        }
        const float x = std::round(mins[parent].x + indent * 0.5f) + 0.5f;
        const float y = std::round((mins[i].y + maxes[i].y) * 0.5f) + 0.5f;
        drawList->AddLine(ImVec2(x, maxes[parent].y), ImVec2(x, y), theme::kBorderHover, thickness);
        drawList->AddLine(ImVec2(x, y), ImVec2(mins[i].x, y), theme::kBorderHover, thickness);
    }

    // The boxes, and after each its transitions: a faint arrow and the target's name. A click on a box selects, a
    // double click renames, a right click opens the menu.
    bool openMenu = false;
    int renamingRow = -1;
    float right = origin.x;
    const ImU32 arrowColour = theme::faded(theme::kTextMuted, 0.75f);
    for (std::size_t i = 0; i < rows.size(); ++i) {
        const State& state = *tree_.find(rows[i].state);
        const float middle = std::round((mins[i].y + maxes[i].y) * 0.5f);
        float x = maxes[i].x + std::round(10.0f * dpi);
        for (const Transition& transition : state.transitions) {
            const State* target = tree_.find(transition.target);
            if (target == nullptr) {
                continue;
            }
            x += drawArrow(drawList, x, middle, tree_.within(state.id, target->id), arrowColour) + std::round(6.0f * dpi);
            drawList->AddText(ImVec2(x, std::round(middle - fontSize * 0.5f)), theme::kTextMuted,
                              target->name.c_str());
            x += ImGui::CalcTextSize(target->name.c_str()).x + std::round(16.0f * dpi);
        }
        right = std::max(right, x);

        if (state.id == renaming_) {
            renamingRow = static_cast<int>(i); // Its name field goes there instead, taking the mouse.
            continue;
        }
        ImGui::SetCursorScreenPos(mins[i]);
        ImGui::PushID(state.id);
        ImGui::InvisibleButton("##state", ImVec2(maxes[i].x - mins[i].x, height));
        ImGui::PopID();
        const bool hovered = ImGui::IsItemHovered();
        if (ImGui::IsItemClicked(ImGuiMouseButton_Left) || ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
            selected_ = state.id;
        }
        if (hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
            rename(state.id);
        }
        if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
            menuState_ = state.id;
            openMenu = true;
        }
        const bool chosen = state.id == selected_;
        const bool active = isCurrent(state.id);
        const ImU32 fill = chosen && hovered ? theme::kSurfaceOpenHover
                           : chosen          ? theme::kSurfaceActive
                           : hovered         ? theme::kSurfaceHover
                                             : theme::kSurface;
        const ImU32 border = chosen ? theme::kAccent : active ? theme::kYes : hovered ? theme::kBorderHover
                                                                                     : theme::kBorder;
        drawList->AddRectFilled(mins[i], maxes[i], fill, kRounding * dpi);
        if (active) {
            drawList->AddRectFilled(mins[i], maxes[i], theme::faded(theme::kYes, 0.16f), kRounding * dpi);
        }
        drawList->AddRect(mins[i], maxes[i], border, kRounding * dpi, 0, 1.0f);
        drawList->AddText(fonts.medium, fontSize, ImVec2(mins[i].x + padding, std::round(middle - fontSize * 0.5f)),
                          theme::kText, state.name.c_str());
    }
    if (renamingRow >= 0) {
        drawRename(fonts, mins[static_cast<std::size_t>(renamingRow)], maxes[static_cast<std::size_t>(renamingRow)]);
    }
    // The room it all takes, so the tree scrolls when it does not fit.
    ImGui::SetCursorScreenPos(origin);
    ImGui::Dummy(ImVec2(right - origin.x, (height + gap) * static_cast<float>(rows.size())));

    // A click on empty space: nothing selected; a right click there: the menu, to add under Root.
    if (ImGui::IsWindowHovered() && !ImGui::IsAnyItemHovered()) {
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            selected_ = 0;
        } else if (ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
            selected_ = 0;
            menuState_ = 0;
            openMenu = true;
        }
    }
    if (ImGui::IsWindowFocused() && renaming_ == 0 && tree_.find(selected_) != nullptr) {
        if (ImGui::IsKeyPressed(ImGuiKey_F2, false)) {
            rename(selected_);
        } else if (ImGui::IsKeyPressed(ImGuiKey_Delete, false)) {
            remove(selected_);
        }
    }
    drawMenu(openMenu);
}

void StateTreeView::drawDetails(const theme::Fonts& fonts) {
    const float dpi = dpiScale();
    State* state = tree_.find(selected_);
    if (state == nullptr) {
        return;
    }
    const float width = ImGui::GetContentRegionAvail().x;
    const float fieldWidth = std::min(width, std::round(kFieldWidth * dpi));

    // A very quiet "Details" over it all (the user asked, 2026-11-14), then its name, in a field, not the panel's
    // width (2026-11-13).
    ImGui::PushStyleColor(ImGuiCol_Text, theme::faded(theme::kTextMuted, 0.7f));
    ImGui::TextUnformatted("Details");
    ImGui::PopStyleColor();
    ImGui::SetNextItemWidth(fieldWidth);
    ImGui::InputText("##name", &state->name);
    keepName(state->name); // A state keeps a name.

    // Its transitions, tried in order while it is active: the first whose conditions pass goes.
    sectionHeader(fonts, "Transitions", state->transitions.size());
    int removed = -1;
    for (std::size_t i = 0; i < state->transitions.size(); ++i) {
        ImGui::PushID(static_cast<int>(i));
        if (drawTransition(*state, state->transitions[i], width)) {
            removed = static_cast<int>(i);
        }
        ImGui::PopID();
    }
    if (removed >= 0) {
        state->transitions.erase(state->transitions.begin() + removed);
    }
    if (ImGui::Button("+ Transition")) {
        state->transitions.emplace_back();
    }

    // Its tasks, under the transitions (the user asked, 2026-11-13; 2026-11-14: kinds picked from a list, not typed):
    // each its kind, then what it needs (Use Skill: one of the profile's skills), then X.
    sectionHeader(fonts, "Tasks", state->tasks.size());
    removed = -1;
    const float small = std::round(6.0f * dpi);
    for (std::size_t i = 0; i < state->tasks.size(); ++i) {
        statetree::Task& task = state->tasks[i];
        ImGui::PushID(static_cast<int>(i));
        ImGui::AlignTextToFramePadding();
        const char* kind = statetree::nameOf(task.kind);
        ImGui::TextUnformatted(kind);
        ImGui::SameLine(0.0f, small);
        const float comboWidth = std::max(40.0f * dpi, std::min(fieldWidth, width - ImGui::CalcTextSize(kind).x -
                                                                               ImGui::GetFrameHeight() - small * 2.0f));
        ImGui::SetNextItemWidth(comboWidth);
        if (ImGui::BeginCombo("##skill", task.skill.empty() ? "Choose a skill" : task.skill.c_str())) {
            const std::vector<std::string> names = skillNames();
            if (names.empty()) {
                ImGui::TextDisabled("No skills added yet (Skills tab)");
            }
            for (const std::string& name : names) {
                if (ImGui::Selectable(name.c_str(), name == task.skill)) {
                    task.skill = name;
                }
            }
            ImGui::EndCombo();
        }
        ImGui::SameLine(0.0f, small);
        if (crossButton("##remove-task")) {
            removed = static_cast<int>(i);
        }
        ImGui::PopID();
    }
    if (removed >= 0) {
        state->tasks.erase(state->tasks.begin() + removed);
    }
    if (ImGui::Button("+ Task")) {
        ImGui::OpenPopup(kTasksPopup);
    }
    ImGui::SetNextWindowPos(ImVec2(ImGui::GetItemRectMin().x, ImGui::GetItemRectMax().y + small));
    if (ImGui::BeginPopup(kTasksPopup)) {
        for (int k = 0; k < statetree::kTaskKinds; ++k) {
            const auto kind = static_cast<statetree::TaskKind>(k);
            if (ImGui::Selectable(statetree::nameOf(kind))) {
                const std::vector<std::string> names = skillNames();
                state->tasks.push_back({kind, names.empty() ? std::string() : names.front()});
            }
        }
        ImGui::EndPopup();
    }
}

void StateTreeView::sectionHeader(const theme::Fonts& fonts, const char* title, std::size_t count) {
    // A little room above, the title in medium weight, how many muted after it, and a faint line under: the details'
    // parts told apart without colours of their own (the user, 2026-11-14: "everything looks the same").
    const float dpi = dpiScale();
    ImGui::Dummy(ImVec2(0.0f, std::round(10.0f * dpi)));
    ImGui::PushFont(fonts.medium, 0.0f);
    ImGui::TextUnformatted(title);
    ImGui::PopFont();
    if (count > 0) {
        ImGui::SameLine(0.0f, std::round(6.0f * dpi));
        ImGui::TextDisabled("%zu", count);
    }
    const ImVec2 at = ImGui::GetCursorScreenPos();
    const float lineY = std::round(at.y - ImGui::GetStyle().ItemSpacing.y * 0.5f);
    ImGui::GetWindowDrawList()->AddLine(ImVec2(at.x, lineY + 0.5f),
                                        ImVec2(at.x + ImGui::GetContentRegionAvail().x, lineY + 0.5f),
                                        theme::kBorder, 1.0f);
    ImGui::Dummy(ImVec2(0.0f, std::round(2.0f * dpi)));
}

void StateTreeView::keepName(std::string& name) {
    if (ImGui::IsItemActivated()) {
        nameBefore_ = name;
    }
    if (ImGui::IsItemDeactivated()) {
        name = trimmed(name);
        if (name.empty()) {
            name = nameBefore_;
        }
    }
}

bool StateTreeView::drawTransition(const State& state, Transition& transition, float width) {
    const float dpi = dpiScale();
    const float pad = std::round(10.0f * dpi);
    const float small = std::round(6.0f * dpi);
    const float frame = ImGui::GetFrameHeight();
    const float inner = std::max(1.0f, width - pad * 2.0f);
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    const ImVec2 cardMin = ImGui::GetCursorScreenPos();
    drawList->ChannelsSplit(2); // The card behind, drawn once its height is known.
    drawList->ChannelsSetCurrent(1);
    ImGui::SetCursorScreenPos(ImVec2(cardMin.x + pad, cardMin.y + pad));
    ImGui::BeginGroup();
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(small, small));

    // Go to [a state] X
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Go to");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(std::max(40.0f * dpi, inner - ImGui::CalcTextSize("Go to").x - small * 2.0f - frame));
    const State* target = tree_.find(transition.target);
    if (ImGui::BeginCombo("##target", target != nullptr ? target->name.c_str() : "Choose a state")) {
        for (const Row& row : tree_.rows()) {
            const std::string label = std::string(static_cast<std::size_t>(row.depth) * 5, ' ') +
                                      tree_.find(row.state)->name;
            ImGui::PushID(row.state);
            if (ImGui::Selectable(label.c_str(), row.state == transition.target)) {
                transition.target = row.state;
            }
            ImGui::PopID();
        }
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    const bool removeIt = crossButton("##remove");

    // The conditions: IF / AND / OR, left, compare, right, X. While the state is active, a dot by each: green while it
    // holds. Narrow: right and compare on a line of their own.
    const bool checking = isCurrent(state.id);
    const bool narrow = inner < 300.0f * dpi;
    const float joinWidth = std::round(44.0f * dpi);
    const float compareWidth = std::round(46.0f * dpi);
    int removed = -1;
    for (std::size_t j = 0; j < transition.conditions.size(); ++j) {
        Condition& condition = transition.conditions[j];
        ImGui::PushID(static_cast<int>(j));
        const ImVec2 row = ImGui::GetCursorScreenPos();
        if (checking) {
            drawList->AddCircleFilled(ImVec2(cardMin.x + pad * 0.5f, std::round(row.y + frame * 0.5f)), 3.0f * dpi,
                                      statetree::holds(condition, variables_) ? theme::kYes : theme::kBorderHover);
        }
        if (j == 0) {
            ImGui::Dummy(ImVec2(joinWidth, frame));
            drawList->AddText(ImVec2(row.x + std::round((joinWidth - ImGui::CalcTextSize("IF").x) * 0.5f),
                                     row.y + ImGui::GetStyle().FramePadding.y),
                              theme::kTextMuted, "IF");
        } else if (ImGui::Button(condition.join == Join::And ? "AND" : "OR", ImVec2(joinWidth, 0.0f))) {
            condition.join = condition.join == Join::And ? Join::Or : Join::And;
        }
        ImGui::SameLine();
        // Number: every compare; a name: = or != only.
        const auto compareCombo = [&](bool equalOnly) {
            ImGui::SetNextItemWidth(compareWidth);
            if (ImGui::BeginCombo("##compare", statetree::symbolOf(condition.compare), ImGuiComboFlags_NoArrowButton)) {
                for (int c = 0; c < statetree::kCompares; ++c) {
                    const auto compare = static_cast<Compare>(c);
                    if ((!equalOnly || compare == Compare::Equal || compare == Compare::NotEqual) &&
                        ImGui::Selectable(statetree::symbolOf(compare), compare == condition.compare)) {
                        condition.compare = compare;
                    }
                }
                ImGui::EndCombo();
            }
        };
        const auto cross = [&] {
            ImGui::SameLine();
            if (crossButton("##x")) {
                removed = static_cast<int>(j);
            }
        };
        // What the row has between IF and X.
        const float avail = std::max(30.0f * dpi, inner - joinWidth - frame - small * 2.0f);
        const auto nextLine = [&] { // Under the row's first line, from where its middle starts.
            ImGui::Dummy(ImVec2(joinWidth, 1.0f));
            ImGui::SameLine();
        };
        if (condition.kind == Kind::HasTarget) {
            const char* answers[] = {"Has a target", "Has no target"};
            const bool none = condition.compare == Compare::NotEqual;
            ImGui::SetNextItemWidth(avail);
            if (ImGui::BeginCombo("##has", answers[none ? 1 : 0])) {
                for (int a = 0; a < 2; ++a) {
                    if (ImGui::Selectable(answers[a], (a == 1) == none)) {
                        condition.compare = a == 1 ? Compare::NotEqual : Compare::Equal;
                    }
                }
                ImGui::EndCombo();
            }
            cross();
        } else if (condition.kind == Kind::TargetName) {
            ImGui::AlignTextToFramePadding();
            ImGui::TextDisabled("Target Name");
            ImGui::SameLine();
            compareCombo(true);
            const float textWidth = avail - ImGui::CalcTextSize("Target Name").x - compareWidth - small * 2.0f;
            if (!narrow) {
                ImGui::SameLine();
                ImGui::SetNextItemWidth(std::max(30.0f * dpi, textWidth));
                ImGui::InputTextWithHint("##text", "a part of it", &condition.text);
                cross();
            } else {
                cross();
                nextLine();
                ImGui::SetNextItemWidth(std::max(30.0f * dpi, inner - joinWidth - small));
                ImGui::InputTextWithHint("##text", "a part of it", &condition.text);
            }
        } else if (condition.kind == Kind::Skill) {
            const auto skillCombo = [&](float comboWidth) {
                ImGui::SetNextItemWidth(comboWidth);
                if (ImGui::BeginCombo("##skill", condition.text.empty() ? "Choose a skill" : condition.text.c_str())) {
                    const std::vector<std::string> names = skillNames();
                    if (names.empty()) {
                        ImGui::TextDisabled("No skills added yet (Skills tab)");
                    }
                    for (const std::string& name : names) {
                        if (ImGui::Selectable(name.c_str(), name == condition.text)) {
                            condition.text = name;
                        }
                        // How it is on the last reading.
                        if (const statetree::SkillSeen* seen = variables_.skill(name); seen != nullptr) {
                            ImGui::SameLine();
                            if (!seen->found) {
                                ImGui::TextDisabled("not seen");
                            } else if (seen->cooldown <= 0.0f) {
                                ImGui::TextDisabled("ready");
                            } else {
                                ImGui::TextDisabled("%.0f%%", static_cast<double>(seen->cooldown));
                            }
                        }
                    }
                    ImGui::EndCombo();
                }
            };
            const auto isCombo = [&](float comboWidth) {
                ImGui::SetNextItemWidth(comboWidth);
                if (ImGui::BeginCombo("##is", statetree::nameOf(condition.is))) {
                    for (int i = 0; i < statetree::kSkillIs; ++i) {
                        const auto is = static_cast<SkillIs>(i);
                        if (ImGui::Selectable(statetree::nameOf(is), is == condition.is)) {
                            condition.is = is;
                        }
                    }
                    ImGui::EndCombo();
                }
            };
            if (!narrow) {
                const float skillWidth = std::floor((avail - small) * 0.55f);
                skillCombo(skillWidth);
                ImGui::SameLine();
                isCombo(avail - skillWidth - small);
                cross();
            } else {
                skillCombo(avail);
                cross();
                nextLine();
                isCombo(std::max(30.0f * dpi, inner - joinWidth - small));
            }
        } else if (!narrow) {
            const float operand =
                std::max(30.0f * dpi, std::floor((inner - joinWidth - compareWidth - frame - small * 4.0f) * 0.5f));
            drawOperand("##left", condition.left, operand);
            ImGui::SameLine();
            compareCombo(false);
            ImGui::SameLine();
            drawOperand("##right", condition.right, operand);
            cross();
        } else {
            drawOperand("##left", condition.left, avail);
            cross();
            nextLine();
            compareCombo(false);
            ImGui::SameLine();
            drawOperand("##right", condition.right,
                        std::max(30.0f * dpi, inner - joinWidth - compareWidth - small * 2.0f));
        }
        ImGui::PopID();
    }
    if (removed >= 0) {
        transition.conditions.erase(transition.conditions.begin() + removed);
    }
    if (ImGui::Button("+ Condition")) {
        ImGui::OpenPopup(kKindsPopup);
    }
    ImGui::SetNextWindowPos(ImVec2(ImGui::GetItemRectMin().x, ImGui::GetItemRectMax().y + small));
    if (ImGui::BeginPopup(kKindsPopup)) {
        const auto add = [&](const char* label, Kind kind) {
            if (ImGui::Selectable(label)) {
                Condition& condition = transition.conditions.emplace_back();
                condition.kind = kind;
                if (kind != Kind::Number) {
                    condition.compare = Compare::Equal; // Has a target; the name has the text.
                }
                if (kind == Kind::Skill) {
                    const std::vector<std::string> names = skillNames();
                    condition.text = names.empty() ? std::string() : names.front();
                }
            }
        };
        add("Compare Number", Kind::Number);
        add("Has Target", Kind::HasTarget);
        add("Target Name", Kind::TargetName);
        add("Skill", Kind::Skill);
        ImGui::EndPopup();
    }
    ImGui::PopStyleVar();
    ImGui::EndGroup();

    const ImVec2 cardMax(cardMin.x + width, ImGui::GetItemRectMax().y + pad);
    drawList->ChannelsSetCurrent(0);
    drawList->AddRectFilled(cardMin, cardMax, theme::kSurface, kRounding * dpi);
    drawList->AddRect(cardMin, cardMax, theme::kBorder, kRounding * dpi);
    drawList->ChannelsMerge();
    ImGui::SetCursorScreenPos(ImVec2(cardMin.x, cardMax.y + small));
    return removeIt;
}

void StateTreeView::drawOperand(const char* id, Operand& operand, float width) {
    ImGui::PushID(id);
    // The game's numbers, each with its value on the last reading, then each skill's cooldown.
    const auto showValue = [&](const Operand& option) {
        ImGui::SameLine();
        if (const std::optional<float> value = statetree::valueOf(option, variables_)) {
            ImGui::TextDisabled(statetree::isCount(option.variable) ? "%.0f" : "%.0f%%", static_cast<double>(*value));
        } else {
            ImGui::TextDisabled("\xE2\x80\x94");
        }
    };
    const auto variables = [&] {
        for (int v = 1; v < statetree::kVariables; ++v) {
            const auto variable = static_cast<Variable>(v);
            if (variable == Variable::Cooldown) {
                continue;
            }
            if (ImGui::Selectable(statetree::nameOf(variable), operand.variable == variable)) {
                operand = {variable, 0.0f, {}};
            }
            showValue({variable, 0.0f, {}});
        }
        for (const std::string& name : skillNames()) {
            const std::string label = name + " cooldown";
            const bool chosen = operand.variable == Variable::Cooldown && operand.skill == name;
            if (ImGui::Selectable(label.c_str(), chosen)) {
                operand = {Variable::Cooldown, 0.0f, name};
            }
            showValue({Variable::Cooldown, 0.0f, name});
        }
    };
    if (operand.variable == Variable::None) {
        // A number to type, and the arrow to pick one of the game's numbers instead.
        const float arrow = ImGui::GetFrameHeight();
        ImGui::SetNextItemWidth(std::max(1.0f, width - arrow));
        ImGui::InputFloat("##number", &operand.number, 0.0f, 0.0f, "%g");
        ImGui::SameLine(0.0f, 0.0f);
        if (ImGui::BeginCombo("##pick", nullptr, ImGuiComboFlags_NoPreview | ImGuiComboFlags_PopupAlignLeft)) {
            variables();
            ImGui::EndCombo();
        }
    } else {
        ImGui::SetNextItemWidth(width);
        const std::string preview = operand.variable == Variable::Cooldown ? operand.skill + " cooldown"
                                                                           : statetree::nameOf(operand.variable);
        if (ImGui::BeginCombo("##pick", preview.c_str())) {
            if (ImGui::Selectable("Number")) { // Starting from the variable's value, when it is known.
                const std::optional<float> value = statetree::valueOf(operand, variables_);
                operand = {Variable::None, value ? std::round(*value) : 0.0f, {}};
            }
            variables();
            ImGui::EndCombo();
        }
    }
    ImGui::PopID();
}

std::vector<std::string> StateTreeView::skillNames() const {
    std::vector<std::string> names;
    for (const statetree::Skill& skill : tree_.skills()) {
        names.push_back(skill.name);
    }
    return names;
}

void StateTreeView::ensureProfile(const std::filesystem::path& folder) {
    if (folder != folder_) {
        openProfile(folder);
    }
}

void StateTreeView::saveSkills() {
    if (history_.keep(tree_)) {
        write();
    }
}

void StateTreeView::add(int parent) {
    const int id = tree_.add(parent, tree_.unusedName("New state"));
    if (id != 0) {
        selected_ = id;
        rename(id); // Named at once.
    }
}

void StateTreeView::rename(int state) {
    if (const State* found = tree_.find(state); found != nullptr) {
        renaming_ = state;
        renameText_ = found->name;
        renameFocus_ = true;
    }
}

void StateTreeView::remove(int state) {
    if (tree_.remove(state)) {
        selected_ = tree_.find(selected_) != nullptr ? selected_ : 0;
        renaming_ = tree_.find(renaming_) != nullptr ? renaming_ : 0;
    }
}

void StateTreeView::drawRename(const theme::Fonts& fonts, ImVec2 min, ImVec2 max) {
    const float dpi = dpiScale();
    const float fontSize = ImGui::GetFontSize();
    ImGui::SetCursorScreenPos(min);
    ImGui::PushFont(fonts.medium, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,
                        ImVec2(std::round(kPadding * dpi) - 1.0f, std::round((max.y - min.y - fontSize) * 0.5f)));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, kRounding * dpi);
    ImGui::PushStyleColor(ImGuiCol_FrameBg, theme::kSurfaceActive);
    ImGui::PushStyleColor(ImGuiCol_Border, theme::kAccent);
    ImGui::SetNextItemWidth(std::max(max.x - min.x, 200.0f * dpi));
    if (renameFocus_) {
        ImGui::SetKeyboardFocusHere();
        renameFocus_ = false;
    }
    ImGui::InputText("##rename", &renameText_, ImGuiInputTextFlags_AutoSelectAll);
    const bool done = ImGui::IsItemDeactivated(); // Enter, a click elsewhere, or Escape (which puts the old name back).
    ImGui::SetNavCursorVisible(false); // Focused from the code, not by keyboard navigation: no outline around it.
    ImGui::PopStyleColor(2);
    ImGui::PopStyleVar(2);
    ImGui::PopFont();
    if (done) {
        const std::string name = trimmed(renameText_);
        if (State* state = tree_.find(renaming_); state != nullptr && !name.empty()) {
            state->name = name; // An empty name is refused: the old one stays.
        }
        renaming_ = 0;
    }
}

void StateTreeView::drawMenu(bool open) {
    if (open) {
        ImGui::OpenPopup(kMenu);
    }
    if (!ImGui::BeginPopup(kMenu)) {
        return;
    }
    // What was picked, done once the menu is closed (adding a state moves the states in memory).
    enum class Pick : unsigned char { None, Add, Remove };
    Pick pick = Pick::None;
    const State* state = tree_.find(menuState_);
    if (ImGui::MenuItem("Add State")) {
        pick = Pick::Add;
    }
    if (state != nullptr) { // No Rename (the user asked, 2026-11-14: the name is typed in the details).
        if (ImGui::MenuItem("Delete", "Del", false, state->id != tree_.root())) {
            pick = Pick::Remove;
        }
    }
    ImGui::EndPopup();
    switch (pick) {
    case Pick::Add: add(state != nullptr ? menuState_ : tree_.root()); break;
    case Pick::Remove: remove(menuState_); break;
    case Pick::None: break;
    }
}

} // namespace observex
