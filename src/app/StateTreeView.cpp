#include "app/StateTreeView.h"

#include "app/SkillsView.h" // SkillIcons, to draw a Use Skill task's miniature.
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

// windows.h (pulled in through app/SkillsView.h, for SkillIcons) defines `small` as a macro (char); this file uses
// `small` as a variable name, so undo it.
#ifdef small
#undef small
#endif

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
constexpr float kTaskHeight = 24.0f; // Each task's line inside its state's box, under the name.
constexpr float kRowGap = 6.0f;
constexpr float kIndent = 24.0f;
constexpr float kPadding = 12.0f;  // Each side of the name in its box.
constexpr float kRounding = 8.0f;  // As the top bar's buttons.
constexpr float kSplitGap = 6.0f;  // Between the tree and the details: their line, and room to grab it.
constexpr float kFieldWidth = 240.0f; // The details' name fields.
constexpr float kOptionIcon = 22.0f;  // A skill's miniature in the lists to pick one from.
// A transition after its state's box: the gap before the first, the tick before a When Tasks Done one's arrow and the
// gap after it, the arrow and the gap after it, then its target's name, and the gap before the next.
constexpr float kFirstTransitionGap = 10.0f;
constexpr float kDoneMarkWidth = 7.0f;
constexpr float kDoneMarkGap = 4.0f;
constexpr float kArrowLength = 16.0f;
constexpr float kArrowGap = 6.0f;
constexpr float kTransitionGap = 16.0f;
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

enum class Glyph : unsigned char { Cross, Plus };

// A square button at the cursor with a cross (take out) or a plus (add), `size` wide (0: the frame's height), its
// fill under the mouse `hoverFill`; dimmed and not clickable unless `enabled`. True when clicked.
bool glyphButton(const char* id, Glyph glyph, float size = 0.0f, bool enabled = true,
                 ImU32 hoverFill = theme::kSurfaceHover) {
    const float dpi = dpiScale();
    size = size > 0.0f ? size : ImGui::GetFrameHeight();
    const ImVec2 min = ImGui::GetCursorScreenPos();
    ImGui::BeginDisabled(!enabled);
    const bool clicked = ImGui::InvisibleButton(id, ImVec2(size, size));
    ImGui::EndDisabled();
    const bool hovered = enabled && ImGui::IsItemHovered();
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    if (hovered) {
        drawList->AddRectFilled(min, ImVec2(min.x + size, min.y + size), hoverFill, 6.0f * dpi);
    }
    const float half = std::round((glyph == Glyph::Cross ? 4.0f : 5.0f) * dpi);
    const ImVec2 centre(std::round(min.x + size * 0.5f), std::round(min.y + size * 0.5f));
    const ImU32 colour = !enabled ? theme::faded(theme::kTextMuted, 0.4f) : hovered ? theme::kText : theme::kTextMuted;
    if (glyph == Glyph::Cross) {
        drawList->AddLine(ImVec2(centre.x - half, centre.y - half), ImVec2(centre.x + half, centre.y + half), colour,
                          1.5f * dpi);
        drawList->AddLine(ImVec2(centre.x - half, centre.y + half), ImVec2(centre.x + half, centre.y - half), colour,
                          1.5f * dpi);
    } else {
        // On pixel centres, so the 1.5 px strokes stay as crisp as the cross.
        drawList->AddLine(ImVec2(centre.x - half, centre.y), ImVec2(centre.x + half, centre.y), colour, 1.5f * dpi);
        drawList->AddLine(ImVec2(centre.x, centre.y - half), ImVec2(centre.x, centre.y + half), colour, 1.5f * dpi);
    }
    return clicked;
}

// A square X, the frame's height; true when clicked.
bool crossButton(const char* id) {
    return glyphButton(id, Glyph::Cross);
}

// Six dots, two columns of three, round `centre`: a row that can be dragged up or down.
void drawGrip(ImDrawList* drawList, ImVec2 centre, ImU32 colour) {
    const float dpi = dpiScale();
    const float step = std::round(4.0f * dpi);
    const float radius = 1.25f * dpi;
    for (int column = 0; column < 2; ++column) {
        for (int line = -1; line <= 1; ++line) {
            drawList->AddCircleFilled(ImVec2(std::round(centre.x + (column - 0.5f) * step) + 0.5f,
                                             std::round(centre.y + line * step) + 0.5f),
                                      radius, colour, 8);
        }
    }
}

// A small chevron pointing down, its middle at `centre`: the row opens a list to pick from.
void drawChevron(ImDrawList* drawList, ImVec2 centre, ImU32 colour) {
    const float dpi = dpiScale();
    const float half = std::round(4.0f * dpi);
    const float drop = std::round(2.0f * dpi);
    const float x = std::round(centre.x) + 0.5f;
    const float y = std::round(centre.y) + 0.5f;
    const ImVec2 points[] = {ImVec2(x - half, y - drop), ImVec2(x, y + drop), ImVec2(x + half, y - drop)};
    drawList->AddPolyline(points, 3, colour, ImDrawFlags_None, 1.5f * dpi);
}

// The arrow before a transition's target, from `x`, its middle at `y`; returns its width. Straight on, or (`back`)
// turning back, only for a transition to the state itself (the user, 2026-11-15: Rest -> Root read as Rest going back
// to itself; a state above gets the straight arrow like any other). A 1 px line on pixel centres and a
// filled head, so it stays crisp this small (the user, 2026-11-13: the 1.5 px open chevron was "weird and ugly").
float drawArrow(ImDrawList* drawList, float x, float y, bool back, ImU32 colour) {
    const float dpi = dpiScale();
    const float length = std::round(kArrowLength * dpi);
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

// The tick before a When Tasks Done transition's arrow, from `x`, its middle at `y`; returns its width.
float drawDoneMark(ImDrawList* drawList, float x, float y, ImU32 colour) {
    const float dpi = dpiScale();
    const float width = std::round(kDoneMarkWidth * dpi);
    const float middle = std::floor(y) + 0.5f;
    const ImVec2 points[] = {ImVec2(x, middle), ImVec2(x + std::round(2.5f * dpi), middle + std::round(2.5f * dpi)),
                             ImVec2(x + width, middle - std::round(3.0f * dpi))};
    drawList->AddPolyline(points, 3, colour, ImDrawFlags_None, 1.5f * dpi);
    return width;
}

// The row under the mouse `mouseY` high (`maxes` the rows' boxes, `gap` between them): the gaps count to the nearer box;
// above the first, Root's; below the last, the last's.
std::size_t rowAt(const std::vector<ImVec2>& maxes, float mouseY, float gap) {
    std::size_t over = 0;
    while (over + 1 < maxes.size() && mouseY >= maxes[over].y + gap * 0.5f) {
        ++over;
    }
    return over;
}

// Where a state dragged in the tree lands (the user asked, 2026-11-15: drag and drop to change the order, seeing
// whether it lands above or below another): as Tree::move takes it, and the line that shows it, at the depth it lands
// at, in the gap it lands in.
struct Drop {
    int parent = 0;  // 0: nowhere (over the state dragged, or a state under it).
    int before = 0;  // 0: as the parent's last child.
    ImVec2 from{};   // The line: from here (the left end, the gap's middle),
    float to = 0.0f; // to this x.
};

// With the mouse `mouseY` high over the rows (`mins`, `maxes` their boxes, `gap` between them): over a box's upper half
// just before that state, over its lower half just after it and all under it, each time as its sibling; over Root, as
// Root's first child. The rows' gaps count to the nearer box; above the first, Root's; below the last, the last's.
Drop dropAt(const statetree::Tree& tree, const std::vector<Row>& rows, const std::vector<ImVec2>& mins,
            const std::vector<ImVec2>& maxes, int dragged, float mouseY, float gap, float indent) {
    const std::size_t over = rowAt(maxes, mouseY, gap);
    const State& state = *tree.find(rows[over].state);
    if (tree.within(state.id, dragged)) {
        return {};
    }
    Drop drop;
    drop.to = maxes[over].x;
    if (state.id == tree.root()) {
        drop.parent = state.id;
        drop.before = state.children.empty() ? 0 : state.children.front();
        drop.from = ImVec2(mins[over].x + indent, maxes[over].y + gap * 0.5f);
        drop.to = std::max(drop.to, drop.from.x + (maxes[over].x - mins[over].x));
        return drop;
    }
    const State& parent = *tree.find(state.parent);
    drop.parent = parent.id;
    drop.from.x = mins[over].x;
    if (mouseY < (mins[over].y + maxes[over].y) * 0.5f) {
        drop.before = state.id;
        drop.from.y = mins[over].y - gap * 0.5f;
        return drop;
    }
    const auto next = std::next(std::ranges::find(parent.children, state.id));
    drop.before = next == parent.children.end() ? 0 : *next;
    std::size_t last = over; // Its last row: it lands after all under it.
    while (last + 1 < rows.size() && rows[last + 1].depth > rows[over].depth) {
        ++last;
    }
    drop.from.y = maxes[last].y + gap * 0.5f;
    return drop;
}

// While something is dragged in the tree: near its top or bottom edge it scrolls, faster nearer (a tree taller than the
// tile).
void scrollNearEdges(float mouseY) {
    const float dpi = dpiScale();
    const float top = ImGui::GetWindowPos().y;
    const float bottom = top + ImGui::GetWindowHeight();
    const float edge = std::round(28.0f * dpi);
    const float scroll = mouseY < top + edge      ? -std::min(1.0f, (top + edge - mouseY) / edge)
                         : mouseY > bottom - edge ? std::min(1.0f, (mouseY - bottom + edge) / edge)
                                                  : 0.0f;
    if (scroll != 0.0f) {
        ImGui::SetScrollY(ImGui::GetScrollY() + scroll * 900.0f * dpi * ImGui::GetIO().DeltaTime);
    }
}

// The small accent box right of the mouse that what is dragged shows in (held under the mouse it hid the names of the
// states it was dropped by), its content `width` wide; returns where its content begins, on its middle row.
ImVec2 drawChip(ImDrawList* drawList, ImVec2 mouse, float width) {
    const float dpi = dpiScale();
    const float height = std::round(kTaskHeight * dpi);
    const float pad = std::round(10.0f * dpi);
    const ImVec2 min(std::round(mouse.x + 16.0f * dpi), std::round(mouse.y - height * 0.5f));
    const ImVec2 max(min.x + std::round(width) + pad * 2.0f, min.y + height);
    drawList->AddRectFilled(min, max, theme::kSurfaceActive, std::round(6.0f * dpi));
    drawList->AddRect(min, max, theme::kAccent, std::round(6.0f * dpi), 0, 1.0f);
    return ImVec2(min.x + pad, min.y + height * 0.5f);
}

// Where what is dragged lands: an accent line with a small ring at its start `from`, across to `to` (a state, in the
// gap between two rows) or, `upright`, down to it (a transition, in the gap between two).
void drawLanding(ImDrawList* drawList, ImVec2 from, float to, bool upright) {
    const float dpi = dpiScale();
    const float thick = std::max(2.0f, std::round(2.0f * dpi));
    const float radius = std::round(3.5f * dpi);
    if (upright) {
        const ImVec2 ring(std::round(from.x), from.y + radius);
        drawList->AddRectFilled(ImVec2(ring.x - thick * 0.5f, ring.y + radius),
                                ImVec2(ring.x + thick * 0.5f, std::max(to, ring.y + radius * 4.0f)), theme::kAccent,
                                thick * 0.5f);
        drawList->AddCircleFilled(ring, radius, theme::kBackground);
        drawList->AddCircle(ring, radius, theme::kAccent, 0, std::max(1.5f, std::round(1.5f * dpi)));
        return;
    }
    const ImVec2 ring(from.x + radius, std::round(from.y));
    drawList->AddRectFilled(ImVec2(ring.x + radius, ring.y - thick * 0.5f),
                            ImVec2(std::max(to, ring.x + radius * 4.0f), ring.y + thick * 0.5f), theme::kAccent,
                            thick * 0.5f);
    drawList->AddCircleFilled(ring, radius, theme::kBackground);
    drawList->AddCircle(ring, radius, theme::kAccent, 0, std::max(1.5f, std::round(1.5f * dpi)));
}

} // namespace

bool StateTreeView::draw(const theme::Fonts& fonts, const std::filesystem::path& folder, const SkillIcons& icons,
                         bool acting, const Replay* replay) {
    const float dpi = dpiScale();
    icons_ = &icons;
    acting_ = acting;
    // What is green: the runner's current state and task while the brain acts; with a clip open, the brain as it was
    // recorded at the frame shown (the user asked, 2026-11-15), by its state's names (ids change when a profile is read
    // again), with the frame's reading for the condition dots.
    shownState_ = 0;
    shownTask_ = -1;
    replayNote_.clear();
    if (replay != nullptr) {
        variables_ = replay->variables != nullptr ? *replay->variables : statetree::Variables{};
        if (!replay->recorded) {
            replayNote_ = "Brain not recorded in this clip";
        } else if (replay->brain != nullptr && replay->brain->acting) {
            const int id = statetree::stateAtPath(tree_, replay->brain->state);
            if (const statetree::State* state = tree_.find(id); state != nullptr) {
                shownState_ = id;
                shownTask_ = replay->brain->task < static_cast<int>(state->tasks.size()) ? replay->brain->task : -1;
            } else {
                replayNote_ = "The clip's brain was in " + replay->brain->state + ", not a state of this profile";
            }
        }
    } else {
        if (replaying_) {
            variables_ = liveVariables_; // The clip closed: the live reading again.
        }
        if (acting) {
            shownState_ = runner_.current(tree_);
            shownTask_ = runner_.task(tree_);
        }
    }
    replaying_ = replay != nullptr;
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
    // The Brain button, right of the profile's name (the user asked, 2026-11-15: the top bar's State tree button here,
    // in place of the status dot, named Brain, Active / Inactive): a click (or Win+Esc, anywhere) starts or stops it.
    // The name's field gives it room; in a tile too narrow even then, it goes under them. No tooltip (the user asked,
    // 2026-11-15); why it could not start goes into the Log.
    BarButton brainButton;
    brainButton.icon = acting ? Icon::Stop : Icon::Play;
    brainButton.caption = "Brain";
    brainButton.text = acting ? "Active" : "Inactive";
    brainButton.open = acting;
    brainButton.highlight = theme::kYes;
    const float brainGap = std::round(10.0f * dpi);
    const float brainWidth = brainButton.width(fonts);
    const float rightEdge = ImGui::GetCursorScreenPos().x + ImGui::GetContentRegionAvail().x;
    drawToolbar(fonts, folder, title_, brainGap + brainWidth);
    if (ImGui::GetItemRectMax().x + brainGap + brainWidth <= rightEdge) {
        ImGui::SameLine(0.0f, brainGap);
    }
    const bool brainClicked = brainButton.draw("##brain", fonts);
    if (!fileError_.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, theme::kRecord);
        ImGui::TextWrapped("%s", fileError_.c_str());
        ImGui::PopStyleColor();
    }
    if (!replayNote_.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, theme::kTextMuted);
        ImGui::TextWrapped("%s", replayNote_.c_str());
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
        keepKeys(); // Nothing selected: nothing on the right (the user asked, 2026-11-13).
        return brainClicked;
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
    return brainClicked;
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
    liveVariables_ = variables;
    if (!replaying_) {
        variables_ = variables;
    }
    runner_.tick(tree_, variables);
}

void StateTreeView::stop() {
    runner_.reset(); // Root again, the next reading on.
    liveVariables_ = {};
    if (!replaying_) {
        variables_ = {};
    }
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

void StateTreeView::drawProfileBar(const theme::Fonts& fonts, const std::filesystem::path& folder) {
    if (folder != folder_) {
        openProfile(folder);
    }
    drawToolbar(fonts, folder, barTitle_);
}

void StateTreeView::drawToolbar(const theme::Fonts& fonts, const std::filesystem::path& folder, std::string& title,
                                float reserve) {
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
    // kFieldWidth, narrower (to a least) where the tile leaves less with `reserve` kept after it.
    ImGui::SetNextItemWidth(std::round(std::clamp(ImGui::GetContentRegionAvail().x - reserve, 120.0f * dpi,
                                                  kFieldWidth * dpi)));
    const bool entered = ImGui::InputText("##profile", &title, ImGuiInputTextFlags_EnterReturnsTrue);
    if (entered || ImGui::IsItemDeactivatedAfterEdit()) {
        renameProfile(title);
    }
    if (!ImGui::IsItemActive()) {
        title = name_; // The profile's name whenever it is not being typed in (a refused one goes back).
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

    // Where each state's box goes: one under another, indented by depth. Its tasks go inside it, under its name (the
    // user asked, 2026-11-15: the tasks in the tree, a state looking unlike a task): each a smaller line, a marker (Use
    // Skill: the skill's miniature), its kind and, muted, what it works on; the box as tall as its name and its tasks.
    const std::vector<Row> rows = tree_.rows();
    const float height = std::round(kBoxHeight * dpi); // The name's row.
    const float gap = std::round(kRowGap * dpi);
    const float indent = std::round(kIndent * dpi);
    const float padding = std::round(kPadding * dpi);
    const float fontSize = ImGui::GetFontSize();
    const float taskHeight = std::round(kTaskHeight * dpi);
    const float taskSize = std::round(fontSize * 0.86f); // The tasks' text.
    const float taskInset = std::round(5.0f * dpi);     // A task's line in from the box's sides,
    const float taskPad = std::round(7.0f * dpi);       // its text in from the line's,
    const float markSide = std::round(14.0f * dpi);     // its marker,
    const float markGap = std::round(6.0f * dpi);       // and the gap after it.
    const float wordGap = std::round(6.0f * dpi);       // Between its kind and what it works on.
    const auto taskValue = [](const statetree::Task& task) -> std::string {
        switch (task.kind) {
        case statetree::TaskKind::UseSkill: {
            // Its skills, top to bottom: the first ready one is used.
            std::string text;
            for (const std::string& skill : task.skills) {
                text += (text.empty() ? "" : ", ") + skill;
            }
            return text.empty() ? std::string("no skill") : text;
        }
        case statetree::TaskKind::SelectTarget:
            return statetree::nameOf(task.target);
        case statetree::TaskKind::ClickMouse:
            return statetree::nameOf(task.button);
        case statetree::TaskKind::DebugPrint:
            return {}; // Nothing to choose.
        }
        return {};
    };
    const auto taskTextWidth = [&](const std::string& text) {
        return fonts.regular->CalcTextSizeA(taskSize, FLT_MAX, 0.0f, text.c_str()).x;
    };
    // Its enter conditions, muted after its name ("if Wind Strike ready"; the user asked, 2026-11-15).
    const auto enterText = [](const State& state) {
        return state.enter.empty() ? std::string() : "if " + statetree::describe(state.enter);
    };
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    std::vector<ImVec2> mins(rows.size());
    std::vector<ImVec2> maxes(rows.size());
    float top = origin.y;
    for (std::size_t i = 0; i < rows.size(); ++i) {
        const State& state = *tree_.find(rows[i].state);
        const std::string enter = enterText(state);
        float width = std::max(fonts.medium->CalcTextSizeA(fontSize, FLT_MAX, 0.0f, state.name.c_str()).x +
                                   padding * 2.0f + (enter.empty() ? 0.0f : wordGap * 1.5f + taskTextWidth(enter)),
                               kBoxMinWidth * dpi);
        for (const statetree::Task& task : state.tasks) {
            width = std::max(width, taskInset * 2.0f + taskPad * 2.0f + markSide + markGap +
                                        taskTextWidth(statetree::nameOf(task.kind)) + wordGap +
                                        taskTextWidth(taskValue(task)));
        }
        const float tasks = state.tasks.empty()
                                ? 0.0f
                                : taskHeight * static_cast<float>(state.tasks.size()) + taskInset;
        mins[i] = ImVec2(origin.x + indent * static_cast<float>(rows[i].depth), top);
        maxes[i] = ImVec2(mins[i].x + std::round(width), top + height + tasks);
        top = maxes[i].y + gap;
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
        const float y = std::round(mins[i].y + height * 0.5f) + 0.5f; // To its name's row.
        drawList->AddLine(ImVec2(x, maxes[parent].y), ImVec2(x, y), theme::kBorderHover, thickness);
        drawList->AddLine(ImVec2(x, y), ImVec2(mins[i].x, y), theme::kBorderHover, thickness);
    }

    // The boxes, and after each its transitions: a faint arrow (a tick before it for When Tasks Done) and the target's
    // name. A click on a box selects, a double click renames, a right click opens the menu. A transition can be
    // grabbed (the user asked, 2026-11-15: dragged as states are): under the mouse a faint pill behind it; held and
    // moved past ImGui's drag threshold it is dragged (drawTransitionDrag); a click selects its state.
    bool openMenu = false;
    int renamingRow = -1;
    float right = origin.x;
    const ImU32 arrowColour = theme::faded(theme::kTextMuted, 0.75f);
    std::vector<TransitionSlot> slots; // Where each transition is drawn, for dragging one.
    for (std::size_t i = 0; i < rows.size(); ++i) {
        const State& state = *tree_.find(rows[i].state);
        const float middle = std::round(mins[i].y + height * 0.5f); // Its name's row.
        float x = maxes[i].x + std::round(kFirstTransitionGap * dpi);
        ImGui::PushID(state.id);
        for (std::size_t t = 0; t < state.transitions.size(); ++t) {
            const Transition& transition = state.transitions[t];
            const State* target = tree_.find(transition.target);
            if (target == nullptr) {
                continue;
            }
            const bool done = transition.when == statetree::When::TasksDone;
            const float nameWidth = ImGui::CalcTextSize(target->name.c_str()).x;
            const float width =
                (done ? std::round(kDoneMarkWidth * dpi) + std::round(kDoneMarkGap * dpi) : 0.0f) +
                std::round(kArrowLength * dpi) + std::round(kArrowGap * dpi) + nameWidth;
            const float reach = std::round(4.0f * dpi); // Its grab area round it.
            const ImVec2 grabMin(x - reach, mins[i].y);
            const ImVec2 grabMax(x + width + reach, mins[i].y + height);
            ImGui::SetCursorScreenPos(grabMin);
            ImGui::PushID(static_cast<int>(t));
            // A click (let go on it, not dragged) selects its state; not on the press, as the details opening then
            // would move the tree under the mouse while it is dragged.
            if (ImGui::InvisibleButton("##transition", ImVec2(grabMax.x - grabMin.x, grabMax.y - grabMin.y)) &&
                dragFrom_ == 0) {
                selected_ = state.id;
            }
            ImGui::PopID();
            const bool hovered = ImGui::IsItemHovered() && dragFrom_ == 0;
            if (dragFrom_ == 0 && dragging_ == 0 && ImGui::IsItemActive() &&
                ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
                dragFrom_ = state.id;
                dragTransition_ = t;
            }
            if (hovered) {
                const float half = std::round(11.0f * dpi);
                drawList->AddRectFilled(ImVec2(grabMin.x, middle - half), ImVec2(grabMax.x, middle + half),
                                        theme::kSurfaceHover, std::round(6.0f * dpi));
            }
            const float from = x;
            const ImU32 colour = hovered ? theme::kTextMuted : arrowColour;
            if (done) {
                x += drawDoneMark(drawList, x, middle, colour) + std::round(kDoneMarkGap * dpi);
            }
            x += drawArrow(drawList, x, middle, target->id == state.id, colour) + std::round(kArrowGap * dpi);
            drawList->AddText(ImVec2(x, std::round(middle - fontSize * 0.5f)),
                              hovered ? theme::kText : theme::kTextMuted, target->name.c_str());
            x += nameWidth;
            slots.push_back({i, t, from, x});
            x += std::round(kTransitionGap * dpi);
        }
        ImGui::PopID();
        right = std::max(right, x);

        const bool renamingThis = state.id == renaming_;
        bool hovered = false;
        if (renamingThis) {
            renamingRow = static_cast<int>(i); // Its name field goes over its name's row, taking the mouse.
        } else {
            ImGui::SetCursorScreenPos(mins[i]);
            ImGui::PushID(state.id);
            ImGui::InvisibleButton("##state", ImVec2(maxes[i].x - mins[i].x, maxes[i].y - mins[i].y));
            ImGui::PopID();
            hovered = ImGui::IsItemHovered();
            // Held and moved past ImGui's drag threshold: taken, to drop elsewhere (Root stays first).
            if (dragging_ == 0 && state.id != tree_.root() && ImGui::IsItemActive() &&
                ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
                dragging_ = state.id;
            }
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
        }
        const bool chosen = state.id == selected_;
        const bool active = isCurrent(state.id);
        // A state above the current one is active too (its transitions count): its border green, no fill.
        const bool above = !active && isActive(state.id);
        const ImU32 fill = chosen && hovered ? theme::kSurfaceOpenHover
                           : chosen          ? theme::kSurfaceActive
                           : hovered         ? theme::kSurfaceHover
                                             : theme::kSurface;
        const ImU32 border = chosen                ? theme::kAccent
                             : active || above     ? theme::kYes
                             : hovered             ? theme::kBorderHover
                                                   : theme::kBorder;
        drawList->AddRectFilled(mins[i], maxes[i], fill, kRounding * dpi);
        if (active) {
            drawList->AddRectFilled(mins[i], maxes[i], theme::faded(theme::kYes, 0.16f), kRounding * dpi);
        }
        drawList->AddRect(mins[i], maxes[i], border, kRounding * dpi, 0, 1.0f);
        if (!renamingThis) {
            drawList->AddText(fonts.medium, fontSize,
                              ImVec2(mins[i].x + padding, std::round(middle - fontSize * 0.5f)), theme::kText,
                              state.name.c_str());
            if (const std::string enter = enterText(state); !enter.empty()) {
                const float nameWidth = fonts.medium->CalcTextSizeA(fontSize, FLT_MAX, 0.0f, state.name.c_str()).x;
                drawList->AddText(fonts.regular, taskSize,
                                  ImVec2(mins[i].x + padding + nameWidth + wordGap * 1.5f,
                                         std::round(middle - taskSize * 0.5f)),
                                  theme::kTextMuted, enter.c_str());
            }
        }

        // Its tasks: the one that runs (while the brain acts) green as the current state is, those done before it
        // fainter. A Parallel one (the user asked, 2026-11-15: "visible visually that it runs all the time") has two
        // upright bars for its marker, and is green the whole time its state is active, also while a child runs.
        const int runningTask = active ? shownTask_ : -1;
        const bool stateActive = active || above;
        for (std::size_t t = 0; t < state.tasks.size(); ++t) {
            const statetree::Task& task = state.tasks[t];
            const bool runs = task.parallel ? stateActive : static_cast<int>(t) == runningTask;
            const bool done =
                !task.parallel && active && (runningTask < 0 || static_cast<int>(t) < runningTask);
            const float lineTop = mins[i].y + height + taskHeight * static_cast<float>(t);
            const ImVec2 lineMin(mins[i].x + taskInset, lineTop);
            const ImVec2 lineMax(maxes[i].x - taskInset, lineTop + taskHeight - std::round(2.0f * dpi));
            if (runs) {
                drawList->AddRectFilled(lineMin, lineMax, theme::faded(theme::kYes, 0.22f), std::round(5.0f * dpi));
                drawList->AddRect(lineMin, lineMax, theme::kYes, std::round(5.0f * dpi), 0, 1.0f);
            }
            const float lineMiddle = std::round((lineMin.y + lineMax.y) * 0.5f);
            const ImVec2 markMin(lineMin.x + taskPad, std::round(lineMiddle - markSide * 0.5f));
            const ImVec2 markMax(markMin.x + markSide, markMin.y + markSide);
            const bool icon = !task.parallel && task.kind == statetree::TaskKind::UseSkill && icons_ != nullptr &&
                              !task.skills.empty() &&
                              icons_->draw(drawList, task.skills.front(), markMin, markMax, std::round(3.0f * dpi),
                                           theme::kSurface);
            const ImU32 textColour = done ? theme::faded(theme::kTextMuted, 0.7f) : theme::kText;
            const ImU32 valueColour = done ? theme::faded(theme::kTextMuted, 0.55f) : theme::kTextMuted;
            if (task.parallel) { // Two upright bars: runs alongside.
                const float c = std::round((markMin.x + markMax.x) * 0.5f);
                const float barWidth = std::max(2.0f, std::round(2.0f * dpi));
                const float barGap = std::round(2.0f * dpi);
                const float half = std::round(5.0f * dpi);
                const ImU32 barColour = runs ? theme::kYes : valueColour;
                drawList->AddRectFilled(ImVec2(c - barGap * 0.5f - barWidth, lineMiddle - half),
                                        ImVec2(c - barGap * 0.5f, lineMiddle + half), barColour);
                drawList->AddRectFilled(ImVec2(c + barGap * 0.5f, lineMiddle - half),
                                        ImVec2(c + barGap * 0.5f + barWidth, lineMiddle + half), barColour);
            } else if (!icon) { // A small diamond: a task, not a state.
                const ImVec2 c((markMin.x + markMax.x) * 0.5f, (markMin.y + markMax.y) * 0.5f);
                const float r = std::round(3.5f * dpi);
                drawList->AddQuadFilled(ImVec2(c.x, c.y - r), ImVec2(c.x + r, c.y), ImVec2(c.x, c.y + r),
                                        ImVec2(c.x - r, c.y), runs ? theme::kYes : valueColour);
            }
            const char* kind = statetree::nameOf(task.kind);
            const float textY = std::round(lineMiddle - taskSize * 0.5f);
            const float kindX = markMax.x + markGap;
            drawList->AddText(fonts.regular, taskSize, ImVec2(kindX, textY), textColour, kind);
            drawList->AddText(fonts.regular, taskSize, ImVec2(kindX + taskTextWidth(kind) + wordGap, textY),
                              valueColour, taskValue(task).c_str());
        }
    }
    if (renamingRow >= 0) {
        const auto row = static_cast<std::size_t>(renamingRow);
        drawRename(fonts, mins[row], ImVec2(maxes[row].x, mins[row].y + height)); // Over its name's row.
    }
    if (dragging_ != 0) {
        drawDrag(fonts, rows, mins, maxes);
    }
    if (dragFrom_ != 0) {
        drawTransitionDrag(rows, mins, maxes, slots);
    }
    // The room it all takes, so the tree scrolls when it does not fit (and a gap under the last box, for the line of a
    // state dropped after it).
    ImGui::SetCursorScreenPos(origin);
    ImGui::Dummy(ImVec2(right - origin.x, rows.empty() ? 0.0f : maxes.back().y + gap - origin.y));

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

void StateTreeView::drawDrag(const theme::Fonts& fonts, const std::vector<Row>& rows, const std::vector<ImVec2>& mins,
                             const std::vector<ImVec2>& maxes) {
    // A state dragged (the user asked, 2026-11-15: "visual feedback when dragging onto something so that i know if it
    // will land above or below"): it and all under it dimmed where they are, its box under the mouse, and an accent
    // line, from a small ring at the depth it lands at, in the gap where it lands. Let go there, it moves with all under
    // it (one change: Ctrl+Z undoes it); Escape, or nowhere to land (over itself, a state under it, off the tree), and
    // nothing moves.
    const State* dragged = tree_.find(dragging_);
    if (dragged == nullptr || ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
        dragging_ = 0;
        ImGui::ClearActiveID(); // Its box let go of: no drag again until it is pressed again.
        return;
    }
    const float dpi = dpiScale();
    const float gap = std::round(kRowGap * dpi);
    const float fontSize = ImGui::GetFontSize();
    const ImVec2 mouse = ImGui::GetIO().MousePos;
    const float mouseY = mouse.y;
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    scrollNearEdges(mouseY);

    const Drop drop = ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem)
                          ? dropAt(tree_, rows, mins, maxes, dragging_, mouseY, gap, std::round(kIndent * dpi))
                          : Drop{};

    for (std::size_t i = 0; i < rows.size(); ++i) {
        if (tree_.within(rows[i].state, dragging_)) {
            drawList->AddRectFilled(mins[i], maxes[i], theme::faded(theme::kBackground, 0.6f), kRounding * dpi);
        }
    }
    // Its name in a small box right of the mouse.
    const ImVec2 chip = drawChip(drawList, mouse,
                                 fonts.medium->CalcTextSizeA(fontSize, FLT_MAX, 0.0f, dragged->name.c_str()).x);
    drawList->AddText(fonts.medium, fontSize, ImVec2(chip.x, std::round(chip.y - fontSize * 0.5f)), theme::kText,
                      dragged->name.c_str());

    // Where it lands, over everything (the box held too).
    if (drop.parent != 0) {
        drawLanding(drawList, drop.from, drop.to, false);
    } else {
        ImGui::SetMouseCursor(ImGuiMouseCursor_NotAllowed);
    }

    if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) { // Let go.
        if (drop.parent != 0) {
            tree_.move(dragging_, drop.parent, drop.before);
        }
        dragging_ = 0;
    }
}

void StateTreeView::drawTransitionDrag(const std::vector<Row>& rows, const std::vector<ImVec2>& mins,
                                       const std::vector<ImVec2>& maxes, const std::vector<TransitionSlot>& slots) {
    // A transition dragged (the user asked, 2026-11-15: as states are): dimmed where it is, its arrow and target's name
    // right of the mouse, and an upright accent line, from a small ring at its top, in the gap where it lands in the row
    // under the mouse: before the first transition there whose middle is right of the mouse, else after the last (over a
    // box, or left of it: first). Its own state's row reorders it (they are tried in order), another state's takes it,
    // its target, When and conditions kept. Let go there, it moves (one change: Ctrl+Z undoes it); Escape, off the
    // tree, or just before or after itself, and nothing moves.
    const State* from = tree_.find(dragFrom_);
    const State* target = from != nullptr && dragTransition_ < from->transitions.size()
                              ? tree_.find(from->transitions[dragTransition_].target)
                              : nullptr;
    if (target == nullptr || ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
        dragFrom_ = 0;
        ImGui::ClearActiveID(); // Let go of: no drag again until it is pressed again.
        return;
    }
    const float dpi = dpiScale();
    const float gap = std::round(kRowGap * dpi);
    const float height = std::round(kBoxHeight * dpi);
    const float fontSize = ImGui::GetFontSize();
    const ImVec2 mouse = ImGui::GetIO().MousePos;
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    scrollNearEdges(mouse.y);

    // Where it lands: a state (0: nowhere), the index in its transitions it goes before (their count: last), and the
    // line's x in the gap there, on that state's name row.
    int to = 0;
    std::size_t before = 0;
    float lineX = 0.0f;
    float middle = 0.0f;
    if (ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem)) {
        const std::size_t over = rowAt(maxes, mouse.y, gap);
        const State& state = *tree_.find(rows[over].state);
        before = state.transitions.size();
        float end = maxes[over].x; // Where the gap before the next one begins: its box's side, then each one's end.
        bool placed = false;
        for (const TransitionSlot& slot : slots) {
            if (slot.row != over) {
                continue;
            }
            if (mouse.x < (slot.from + slot.to) * 0.5f) {
                before = slot.index;
                lineX = (end + slot.from) * 0.5f;
                placed = true;
                break;
            }
            end = slot.to;
        }
        if (!placed) {
            lineX = end + std::round((end == maxes[over].x ? kFirstTransitionGap : kTransitionGap) * dpi) * 0.5f;
        }
        const bool inPlace = state.id == dragFrom_ && (before == dragTransition_ || before == dragTransition_ + 1);
        to = inPlace ? 0 : state.id;
        middle = std::round(mins[over].y + height * 0.5f);
    }

    for (const TransitionSlot& slot : slots) {
        if (rows[slot.row].state == dragFrom_ && slot.index == dragTransition_) {
            const float y = std::round(mins[slot.row].y + height * 0.5f);
            const float half = std::round(11.0f * dpi);
            const float reach = std::round(4.0f * dpi);
            drawList->AddRectFilled(ImVec2(slot.from - reach, y - half), ImVec2(slot.to + reach, y + half),
                                    theme::faded(theme::kBackground, 0.6f), std::round(6.0f * dpi));
        }
    }
    // Its tick (When Tasks Done), arrow and target's name in a small box right of the mouse.
    const Transition& transition = from->transitions[dragTransition_];
    const bool done = transition.when == statetree::When::TasksDone;
    const float markWidth = done ? std::round(kDoneMarkWidth * dpi) + std::round(kDoneMarkGap * dpi) : 0.0f;
    const float arrowWidth = std::round(kArrowLength * dpi) + std::round(kArrowGap * dpi);
    const ImVec2 chip =
        drawChip(drawList, mouse, markWidth + arrowWidth + ImGui::CalcTextSize(target->name.c_str()).x);
    float x = chip.x;
    if (done) {
        x += drawDoneMark(drawList, x, chip.y, theme::kTextMuted) + std::round(kDoneMarkGap * dpi);
    }
    x += drawArrow(drawList, x, chip.y, target->id == from->id, theme::kTextMuted) + std::round(kArrowGap * dpi);
    drawList->AddText(ImVec2(x, std::round(chip.y - fontSize * 0.5f)), theme::kText, target->name.c_str());

    // Where it lands, over everything.
    if (to != 0) {
        const float half = std::round(height * 0.4f);
        drawLanding(drawList, ImVec2(lineX, middle - half), middle + half, true);
    } else {
        ImGui::SetMouseCursor(ImGuiMouseCursor_NotAllowed);
    }

    if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) { // Let go.
        if (to != 0 && tree_.moveTransition(dragFrom_, dragTransition_, to, before)) {
            selected_ = to; // Its state's details show it where it went.
        }
        dragFrom_ = 0;
    }
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

    // Its enter conditions (the user asked, 2026-11-15; not Root's, where the brain starts): it is entered only while
    // they pass, and of a parent's children the first that can be is entered.
    if (state->id != tree_.root()) {
        sectionHeader(fonts, "Enter Conditions", state->enter.size());
        ImGui::PushID("enter");
        drawEnter(*state, width);
        ImGui::PopID();
    }

    // Its transitions, tried in order while it is active: the first whose conditions pass goes.
    sectionHeader(fonts, "Transitions", state->transitions.size());
    // Transitions and tasks each in an ID scope of their own: both number their rows from 0, so the first transition's
    // "##target" (Go to) and the first task's (Select Target's) were one ID and ImGui stopped on the conflict.
    int removed = -1;
    ImGui::PushID("transitions");
    for (std::size_t i = 0; i < state->transitions.size(); ++i) {
        ImGui::PushID(static_cast<int>(i));
        if (drawTransition(*state, state->transitions[i], width)) {
            removed = static_cast<int>(i);
        }
        ImGui::PopID();
    }
    ImGui::PopID();
    if (removed >= 0) {
        state->transitions.erase(state->transitions.begin() + removed);
    }
    if (ImGui::Button("+ Transition")) {
        state->transitions.emplace_back();
    }

    // Its tasks, under the transitions (the user asked, 2026-11-13; 2026-11-14: kinds picked from a list, not typed):
    // each its kind, then what it needs (Use Skill: one of the profile's skills), then X. The one that runs is shown
    // green in the tree, not here (the user asked, 2026-11-15).
    sectionHeader(fonts, "Tasks", state->tasks.size());
    removed = -1;
    const float small = std::round(6.0f * dpi);
    const float frame = ImGui::GetFrameHeight();
    const ImGuiStyle& style = ImGui::GetStyle();
    // Each row its kind's label, then its control (and, for Use Skill, its miniature) and X; when the details are too
    // narrow for one task's control text after its label, every task's control goes on a line of its own under its
    // label (as a transition's conditions), so the rows stay alike.
    const auto valueOf = [](const statetree::Task& task) {
        return task.kind == statetree::TaskKind::UseSkill
                   ? (task.skills.empty() ? "Choose a skill" : task.skills.front().c_str())
               : task.kind == statetree::TaskKind::SelectTarget ? statetree::nameOf(task.target)
                                                                 : statetree::nameOf(task.button);
    };
    const auto iconOf = [&](const statetree::Task& task) {
        return task.kind == statetree::TaskKind::UseSkill && icons_ != nullptr ? frame + small : 0.0f;
    };
    // A kind that can run alongside has a Parallel checkbox before its X (the user asked, 2026-11-15: picked on the
    // task, as UE's tasks of an active state); Debug Print has nothing else to choose.
    const char* parallelLabel = "Parallel";
    const float parallelRoom = frame + style.ItemInnerSpacing.x + ImGui::CalcTextSize(parallelLabel).x + small;
    const auto parallelOf = [&](const statetree::Task& task) {
        return statetree::canRunParallel(task.kind) ? parallelRoom : 0.0f;
    };
    const auto inLineOf = [&](const statetree::Task& task) {
        return width - ImGui::CalcTextSize(statetree::nameOf(task.kind)).x - small - iconOf(task) - small - frame -
               parallelOf(task);
    };
    bool ownLines = false;
    for (const statetree::Task& task : state->tasks) {
        if (task.kind == statetree::TaskKind::DebugPrint) {
            continue; // No control: its checkbox and X fit after its name.
        }
        const float wanted =
            std::min(fieldWidth, ImGui::CalcTextSize(valueOf(task)).x + style.FramePadding.x * 2.0f + frame);
        ownLines = ownLines || inLineOf(task) < wanted;
    }
    ImGui::PushID("tasks");
    for (std::size_t i = 0; i < state->tasks.size(); ++i) {
        statetree::Task& task = state->tasks[i];
        ImGui::PushID(static_cast<int>(i));
        const char* kind = statetree::nameOf(task.kind);
        const float icon = iconOf(task);
        const float controlWidth = std::max(
            40.0f * dpi,
            std::min(fieldWidth, ownLines ? width - icon - small - frame - parallelOf(task) : inLineOf(task)));
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(kind);
        if (!ownLines) {
            ImGui::SameLine(0.0f, small);
        }
        if (task.kind == statetree::TaskKind::UseSkill) {
            // Its skills as an array, tried top to bottom (the user asked, 2026-11-15: as Unreal's arrays); the
            // array takes the picker's and the miniature's room, the task's X at its top right.
            drawSkillArray(task.skills, controlWidth + icon, theme::kSurface);
        } else if (task.kind == statetree::TaskKind::SelectTarget) {
            ImGui::SetNextItemWidth(controlWidth);
            if (ImGui::BeginCombo("##target", statetree::nameOf(task.target))) {
                for (int t = 0; t < statetree::kTargets; ++t) {
                    const auto target = static_cast<statetree::Target>(t);
                    if (ImGui::Selectable(statetree::nameOf(target), target == task.target)) {
                        task.target = target;
                    }
                }
                ImGui::EndCombo();
            }
        } else if (task.kind == statetree::TaskKind::ClickMouse) {
            ImGui::SetNextItemWidth(controlWidth);
            if (ImGui::BeginCombo("##button", statetree::nameOf(task.button))) {
                for (int b = 0; b < statetree::kClickButtons; ++b) {
                    const auto button = static_cast<statetree::ClickButton>(b);
                    if (ImGui::Selectable(statetree::nameOf(button), button == task.button)) {
                        task.button = button;
                    }
                }
                ImGui::EndCombo();
            }
        }
        if (statetree::canRunParallel(task.kind)) {
            if (task.kind != statetree::TaskKind::DebugPrint) { // After its control.
                ImGui::SameLine(0.0f, small);
            }
            ImGui::Checkbox(parallelLabel, &task.parallel);
        }
        ImGui::SameLine(0.0f, small);
        if (crossButton("##remove-task")) {
            removed = static_cast<int>(i);
        }
        ImGui::PopID();
    }
    ImGui::PopID();
    if (removed >= 0) {
        state->tasks.erase(state->tasks.begin() + removed);
    }
    if (ImGui::Button("+ Task")) {
        ImGui::OpenPopup(kTasksPopup);
    }
    // No forced position: ImGui places the popup at the click and keeps it on screen (the user asked, 2026-11-15: a
    // low + Task used to push the list off the bottom).
    if (ImGui::BeginPopup(kTasksPopup)) {
        for (int k = 0; k < statetree::kTaskKinds; ++k) {
            const auto kind = static_cast<statetree::TaskKind>(k);
            if (ImGui::Selectable(statetree::nameOf(kind))) {
                statetree::Task task;
                task.kind = kind;
                if (kind == statetree::TaskKind::UseSkill) {
                    const std::vector<std::string> names = skillNames();
                    if (!names.empty()) {
                        task.skills = {names.front()};
                    }
                }
                state->tasks.push_back(std::move(task));
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

    // Go to [a state] X, and under it When [Any time / Tasks done], the two fields lined up.
    const float labels = std::max(ImGui::CalcTextSize("Go to").x, ImGui::CalcTextSize("When").x);
    const float field = std::max(40.0f * dpi, inner - labels - small * 2.0f - frame);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Go to");
    ImGui::SameLine(0.0f, labels - ImGui::CalcTextSize("Go to").x + small);
    ImGui::SetNextItemWidth(field);
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

    // When it is tried: on every reading, or only once the current state's tasks are done (see statetree::When).
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("When");
    ImGui::SameLine(0.0f, labels - ImGui::CalcTextSize("When").x + small);
    ImGui::SetNextItemWidth(field);
    if (ImGui::BeginCombo("##when", statetree::nameOf(transition.when))) {
        for (int w = 0; w < statetree::kWhens; ++w) {
            const auto when = static_cast<statetree::When>(w);
            if (ImGui::Selectable(statetree::nameOf(when), when == transition.when)) {
                transition.when = when;
            }
        }
        ImGui::EndCombo();
    }

    // Its conditions; while the state is active (it or a state under it current), a dot by each.
    drawConditions(transition.conditions, isActive(state.id), inner, cardMin.x + pad * 0.5f);
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

void StateTreeView::drawEnter(State& state, float width) {
    const float dpi = dpiScale();
    const float small = std::round(6.0f * dpi);
    if (state.enter.empty()) {
        // None yet: just + Condition (an empty card would only frame the button).
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(small, small));
        drawConditions(state.enter, false, width, 0.0f);
        ImGui::PopStyleVar();
        return;
    }
    const float pad = std::round(10.0f * dpi);
    const float inner = std::max(1.0f, width - pad * 2.0f);
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    const ImVec2 cardMin = ImGui::GetCursorScreenPos();
    drawList->ChannelsSplit(2); // The card behind, drawn once its height is known.
    drawList->ChannelsSetCurrent(1);
    ImGui::SetCursorScreenPos(ImVec2(cardMin.x + pad, cardMin.y + pad));
    ImGui::BeginGroup();
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(small, small));
    // Dots while the brain acts (or acted, at a clip's frame): they say whether it could be entered now.
    drawConditions(state.enter, shownState_ != 0, inner, cardMin.x + pad * 0.5f);
    ImGui::PopStyleVar();
    ImGui::EndGroup();
    const ImVec2 cardMax(cardMin.x + width, ImGui::GetItemRectMax().y + pad);
    drawList->ChannelsSetCurrent(0);
    drawList->AddRectFilled(cardMin, cardMax, theme::kSurface, kRounding * dpi);
    drawList->AddRect(cardMin, cardMax, theme::kBorder, kRounding * dpi);
    drawList->ChannelsMerge();
    ImGui::SetCursorScreenPos(ImVec2(cardMin.x, cardMax.y + small));
}

void StateTreeView::drawConditions(std::vector<Condition>& conditions, bool checking, float inner, float dotX) {
    // The conditions: IF / AND / OR, left, compare, right, X. While `checking`, a dot by each: green while it holds.
    // Narrow: right and compare on a line of their own.
    const float dpi = dpiScale();
    const float small = std::round(6.0f * dpi);
    const float frame = ImGui::GetFrameHeight();
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    const bool narrow = inner < 300.0f * dpi;
    const float joinWidth = std::round(44.0f * dpi);
    const float compareWidth = std::round(46.0f * dpi);
    int removed = -1;
    for (std::size_t j = 0; j < conditions.size(); ++j) {
        Condition& condition = conditions[j];
        ImGui::PushID(static_cast<int>(j));
        const ImVec2 row = ImGui::GetCursorScreenPos();
        if (checking) {
            drawList->AddCircleFilled(ImVec2(dotX, std::round(row.y + frame * 0.5f)), 3.0f * dpi,
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
                const std::vector<std::string> names = skillNames();
                ImGui::SetNextWindowSizeConstraints(ImVec2(skillOptionsWidth(names), 0.0f), ImVec2(FLT_MAX, FLT_MAX));
                if (ImGui::BeginCombo("##skill", condition.text.empty() ? "Choose a skill" : condition.text.c_str())) {
                    if (names.empty()) {
                        ImGui::TextDisabled("No skills added yet (Skills tab)");
                    }
                    for (const std::string& name : names) {
                        if (skillOption(name, name == condition.text, false)) {
                            condition.text = name;
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
        } else if (condition.kind == Kind::SkillsReady) {
            // Is Skill Ready (the user asked, 2026-11-15): Any Skill / All Skills, and under it the skills as an
            // array (the user asked, 2026-11-15: rows as Unreal's, each added with +, not ticked in a list), from
            // under the combo to the X's right edge; on a narrow card from under IF, so the names fit.
            ImGui::SetNextItemWidth(avail);
            if (ImGui::BeginCombo("##need", statetree::nameOf(condition.need))) {
                for (int n = 0; n < statetree::kNeeds; ++n) {
                    const auto need = static_cast<statetree::Need>(n);
                    if (ImGui::Selectable(statetree::nameOf(need), need == condition.need)) {
                        condition.need = need;
                    }
                }
                ImGui::EndCombo();
            }
            cross();
            if (narrow) {
                drawSkillArray(condition.skills, inner, theme::kBackground);
            } else {
                nextLine();
                drawSkillArray(condition.skills, inner - joinWidth - small, theme::kBackground);
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
        conditions.erase(conditions.begin() + removed);
    }
    if (ImGui::Button("+ Condition")) {
        ImGui::OpenPopup(kKindsPopup);
    }
    ImGui::SetNextWindowPos(ImVec2(ImGui::GetItemRectMin().x, ImGui::GetItemRectMax().y + small));
    if (ImGui::BeginPopup(kKindsPopup)) {
        const auto add = [&](const char* label, Kind kind) {
            if (ImGui::Selectable(label)) {
                Condition& condition = conditions.emplace_back();
                condition.kind = kind;
                if (kind != Kind::Number) {
                    condition.compare = Compare::Equal; // Has a target; the name has the text.
                }
                const std::vector<std::string> names = skillNames();
                if (kind == Kind::Skill) {
                    condition.text = names.empty() ? std::string() : names.front();
                } else if (kind == Kind::SkillsReady && !names.empty()) {
                    condition.skills = {names.front()};
                }
            }
        };
        add("Compare Number", Kind::Number);
        add("Has Target", Kind::HasTarget);
        add("Target Name", Kind::TargetName);
        add("Skill", Kind::Skill);
        add("Is Skill Ready", Kind::SkillsReady);
        ImGui::EndPopup();
    }
}

void StateTreeView::drawSkillArray(std::vector<std::string>& skills, float width, ImU32 fill) {
    // As Unreal's arrays (the user asked, 2026-11-15: a vertical array of rows, a new one added with +, no ticks): a
    // well with "Skills" and how many at its top and a + that adds one (picked from a list under the well), then a row
    // per skill: its place (dragged, the skill moves up or down: Use Skill tries them top to bottom), its miniature, its
    // name and a chevron (a click picks another) and an X that takes it out. A skill is in it once: the lists show the
    // ones already in it dimmed.
    const float dpi = dpiScale();
    const float row = ImGui::GetFrameHeight();
    const float pad = std::round(4.0f * dpi);    // Round the rows, inside the well.
    const float gap = std::round(2.0f * dpi);    // Between rows.
    const float inset = std::round(3.0f * dpi);  // The miniature and the + inside their row.
    const float place = std::round(24.0f * dpi); // The place's column, also the handle to drag the row by.
    const float side = row - inset * 2.0f;       // The miniature.
    const float small = std::round(6.0f * dpi);
    const float textY = std::round((row - ImGui::GetFontSize()) * 0.5f);
    const float rounding = std::round(6.0f * dpi);
    ImDrawList* drawList = ImGui::GetWindowDrawList();

    const std::vector<std::string> names = skillNames();
    const auto listed = [&](const std::string& name) { return std::ranges::find(skills, name) != skills.end(); };
    const bool canAdd = std::ranges::any_of(names, [&](const std::string& name) { return !listed(name); });
    const bool noSkills = skills.empty() && names.empty(); // A muted line says where skills come from.
    const std::size_t lines = skills.size() + (noSkills ? 1 : 0);
    width = std::max(width, place + side + row * 3.0f);
    const float height =
        row + (lines > 0 ? pad * 2.0f + static_cast<float>(lines) * row + static_cast<float>(lines - 1) * gap : 0.0f);

    ImGui::PushID("##skill-array");
    const ImGuiID arrayId = ImGui::GetID("##rows");
    const ImGuiID addId = ImGui::GetID("##add");
    if (dragArray_ == arrayId &&
        (!ImGui::IsMouseDown(ImGuiMouseButton_Left) || dragIndex_ >= static_cast<int>(skills.size()))) {
        dragArray_ = 0; // Let go.
    }
    const bool dragging = dragArray_ == arrayId;
    const ImVec2 min = ImGui::GetCursorScreenPos();
    const ImVec2 max(min.x + width, min.y + height);
    ImGui::BeginGroup();
    drawList->AddRectFilled(min, max, fill, kRounding * dpi);
    drawList->AddRect(min, max, theme::kBorder, kRounding * dpi);

    // Its top: "Skills", how many (muted), and + at the right.
    const float titleX = min.x + std::round(10.0f * dpi);
    drawList->AddText(ImVec2(titleX, min.y + textY), theme::kTextMuted, "Skills");
    if (!skills.empty()) {
        char count[16];
        std::snprintf(count, sizeof(count), "%zu", skills.size());
        drawList->AddText(ImVec2(titleX + ImGui::CalcTextSize("Skills").x + small, min.y + textY),
                          theme::faded(theme::kTextMuted, 0.6f), count);
    }
    const float plus = row - inset * 2.0f;
    ImGui::SetCursorScreenPos(ImVec2(max.x - inset - plus, min.y + inset));
    if (glyphButton("##plus", Glyph::Plus, plus, canAdd)) {
        ImGui::OpenPopupEx(addId);
    }
    if (lines > 0) {
        const float lineY = std::floor(min.y + row) + 0.5f;
        drawList->AddLine(ImVec2(min.x + 1.0f, lineY), ImVec2(max.x - 1.0f, lineY), theme::kBorder, 1.0f);
    }
    const float rowsTop = min.y + row + pad;
    if (noSkills) {
        drawList->AddText(ImVec2(titleX, rowsTop + textY), theme::faded(theme::kTextMuted, 0.7f),
                          "No skills added yet (Skills tab)");
    }

    // A row per skill.
    const bool movable = skills.size() > 1;
    int removed = -1;
    for (std::size_t i = 0; i < skills.size(); ++i) {
        ImGui::PushID(static_cast<int>(i));
        const ImGuiID pickId = ImGui::GetID("##pick");
        const float y = rowsTop + static_cast<float>(i) * (row + gap);
        const ImVec2 rowMin(min.x + pad, y);
        const ImVec2 rowMax(max.x - pad, y + row);
        const bool dragged = dragging && dragIndex_ == static_cast<int>(i);
        const bool open = ImGui::IsPopupOpen(pickId, ImGuiPopupFlags_None);
        const bool hovered = !dragging && ImGui::IsWindowHovered() && ImGui::IsMouseHoveringRect(rowMin, rowMax);
        const ImU32 back = dragged ? theme::kSurfaceActive : (hovered || open) ? theme::kSurfaceHover : fill;
        if (back != fill) {
            drawList->AddRectFilled(rowMin, rowMax, back, rounding);
        }
        if (dragged) {
            drawList->AddRect(rowMin, rowMax, theme::kAccent, rounding);
        }

        // Its place (0 first, as Unreal's), or under the mouse the grip it is dragged by.
        const ImVec2 placeMiddle(rowMin.x + place * 0.5f, y + row * 0.5f);
        if (movable) {
            ImGui::SetCursorScreenPos(rowMin);
            ImGui::InvisibleButton("##place", ImVec2(place, row));
            if (ImGui::IsItemActivated()) {
                dragArray_ = arrayId;
                dragIndex_ = static_cast<int>(i);
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
            }
        }
        if (movable && (hovered || dragged)) {
            drawGrip(drawList, placeMiddle, dragged ? theme::kText : theme::kTextMuted);
        } else {
            char index[16];
            std::snprintf(index, sizeof(index), "%zu", i);
            drawList->AddText(ImVec2(std::round(placeMiddle.x - ImGui::CalcTextSize(index).x * 0.5f), y + textY),
                              theme::faded(theme::kTextMuted, 0.7f), index);
        }

        // Its miniature, name and chevron: a click opens the list to pick another.
        const float crossX = rowMax.x - row;
        const float pickX = rowMin.x + place;
        ImGui::SetCursorScreenPos(ImVec2(pickX, y));
        if (ImGui::InvisibleButton("##pick-row", ImVec2(std::max(1.0f, crossX - pickX), row))) {
            ImGui::OpenPopupEx(pickId);
        }
        const ImVec2 iconMin(pickX, y + inset);
        const ImVec2 iconMax(pickX + side, y + inset + side);
        if (icons_ == nullptr || !icons_->draw(drawList, skills[i], iconMin, iconMax, std::round(4.0f * dpi), back)) {
            drawList->AddRect(iconMin, iconMax, theme::kBorderHover, std::round(4.0f * dpi)); // Not in the profile.
        }
        const float chevronX = crossX - small - std::round(4.0f * dpi);
        const float nameX = iconMax.x + small;
        const float nameEnd = chevronX - std::round(10.0f * dpi);
        ImGui::RenderTextEllipsis(drawList, ImVec2(nameX, y + textY), ImVec2(nameEnd, y + row), nameEnd,
                                  skills[i].c_str(), nullptr, nullptr); // "Wind Str..." when too narrow.
        drawChevron(drawList, ImVec2(chevronX, y + row * 0.5f),
                    hovered || open ? theme::kText : theme::faded(theme::kTextMuted, 0.8f));

        // Its X.
        ImGui::SetCursorScreenPos(ImVec2(crossX, y));
        if (glyphButton("##remove", Glyph::Cross, row, true, theme::kSurfaceActive)) {
            removed = static_cast<int>(i);
        }

        // The list to pick another from, under the row as a combo's.
        ImGui::SetNextWindowSizeConstraints(ImVec2(skillOptionsWidth(names), 0.0f), ImVec2(FLT_MAX, FLT_MAX));
        if (ImGui::BeginComboPopup(pickId, ImRect(rowMin, rowMax), ImGuiComboFlags_None)) {
            for (const std::string& name : names) {
                if (skillOption(name, name == skills[i], name != skills[i] && listed(name))) {
                    skills[i] = name;
                }
            }
            ImGui::EndCombo();
        }
        ImGui::PopID();
    }

    // The list to add one from, under the well.
    ImGui::SetNextWindowSizeConstraints(ImVec2(skillOptionsWidth(names), 0.0f), ImVec2(FLT_MAX, FLT_MAX));
    if (ImGui::BeginComboPopup(addId, ImRect(min, max), ImGuiComboFlags_None)) {
        for (const std::string& name : names) {
            if (skillOption(name, false, listed(name))) {
                skills.push_back(name);
            }
        }
        ImGui::EndCombo();
    }

    if (removed >= 0) {
        skills.erase(skills.begin() + removed);
        dragArray_ = 0;
    } else if (dragging) {
        // The dragged skill goes to the row under the mouse; the others close up round it. Let go, it is one change.
        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
        const float under = (ImGui::GetMousePos().y - rowsTop + gap * 0.5f) / (row + gap);
        const int to = std::clamp(static_cast<int>(std::floor(under)), 0, static_cast<int>(skills.size()) - 1);
        if (to != dragIndex_) {
            std::string moved = std::move(skills[static_cast<std::size_t>(dragIndex_)]);
            skills.erase(skills.begin() + dragIndex_);
            skills.insert(skills.begin() + to, std::move(moved));
            dragIndex_ = to;
        }
    }

    ImGui::SetCursorScreenPos(min);
    ImGui::Dummy(ImVec2(width, height));
    ImGui::EndGroup();
    ImGui::PopID();
}

bool StateTreeView::skillOption(const std::string& name, bool selected, bool taken) {
    // A row as tall as a miniature: the skill's miniature, its name and, at the right, muted, how the last reading saw
    // it, or "added" when it is in the list already (then dimmed, not to be picked).
    const float dpi = dpiScale();
    const float side = std::round(kOptionIcon * dpi);
    const float small = std::round(8.0f * dpi);
    const float rounding = std::round(4.0f * dpi);
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    const ImVec2 at = ImGui::GetCursorScreenPos();
    const float right = at.x + ImGui::GetContentRegionAvail().x;
    ImGui::PushID(name.c_str());
    const bool picked = ImGui::Selectable("##option", selected,
                                          taken ? ImGuiSelectableFlags_Disabled : ImGuiSelectableFlags_None,
                                          ImVec2(0.0f, side));
    const bool hovered = ImGui::IsItemHovered();
    ImGui::PopID();
    // Under the miniature's rounded corners: what Selectable drew there.
    const ImU32 back = hovered ? (ImGui::IsMouseDown(ImGuiMouseButton_Left) ? theme::kSurfaceActive
                                                                            : theme::kSurfaceHover)
                       : selected ? theme::kSurfaceActive
                                  : theme::kSurface;
    const ImVec2 iconMax(at.x + side, at.y + side);
    if (icons_ == nullptr || !icons_->draw(drawList, name, at, iconMax, rounding, back)) {
        drawList->AddRect(at, iconMax, theme::kBorderHover, rounding);
    }
    if (taken) {
        drawList->AddRectFilled(at, iconMax, theme::faded(theme::kSurface, 0.6f), rounding);
    }
    const float textY = at.y + std::round((side - ImGui::GetFontSize()) * 0.5f);
    drawList->AddText(ImVec2(iconMax.x + small, textY), taken ? theme::faded(theme::kTextMuted, 0.7f) : theme::kText,
                      name.c_str());
    const std::string status = taken ? std::string("added") : skillStatus(name);
    if (!status.empty()) {
        drawList->AddText(ImVec2(right - ImGui::CalcTextSize(status.c_str()).x, textY),
                          theme::faded(theme::kTextMuted, taken ? 0.6f : 1.0f), status.c_str());
    }
    return picked && !taken;
}

float StateTreeView::skillOptionsWidth(const std::vector<std::string>& names) const {
    const float dpi = dpiScale();
    float widest = names.empty() ? ImGui::CalcTextSize("No skills added yet (Skills tab)").x : 0.0f;
    for (const std::string& name : names) {
        const std::string status = skillStatus(name);
        widest = std::max(widest, std::round(kOptionIcon * dpi) + std::round(8.0f * dpi) +
                                      ImGui::CalcTextSize(name.c_str()).x + std::round(24.0f * dpi) +
                                      std::max(ImGui::CalcTextSize(status.c_str()).x, ImGui::CalcTextSize("added").x));
    }
    return widest + ImGui::GetStyle().FramePadding.x * 2.0f; // A combo's list has the frame's padding each side.
}

std::string StateTreeView::skillStatus(const std::string& name) const {
    const statetree::SkillSeen* seen = variables_.skill(name);
    if (seen == nullptr) {
        return {};
    }
    if (!seen->found) {
        return "not seen";
    }
    if (statetree::isReady(variables_, name)) {
        return "ready";
    }
    char text[16];
    std::snprintf(text, sizeof(text), "%.0f%%", static_cast<double>(seen->cooldown));
    return text;
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
