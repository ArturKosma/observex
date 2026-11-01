#include "app/Review.h"

#include "app/Widgets.h"
#include "engine/ImageFile.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cwctype>
#include <exception>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <system_error>
#include <utility>

namespace observex {

namespace {

constexpr ImGuiWindowFlags kAnswerBox = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                                        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_AlwaysAutoResize |
                                        ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav;

std::string toUtf8(const std::filesystem::path& path) {
    const std::u8string text = path.u8string();
    return std::string(reinterpret_cast<const char*>(text.data()), text.size());
}

std::filesystem::path fromUtf8(const std::string& text) {
    return std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(text.data()), text.size()));
}

std::string readTextFile(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

std::filesystem::path marksFile(const std::filesystem::path& folder, const Category& category, const char* kind) {
    return folder / (std::string(category.id) + "-" + kind + ".txt");
}

ImU32 markColor(const Mark& mark) {
    switch (mark.answer) {
    case Answer::Yes:
        return theme::kYes;
    case Answer::No:
        return theme::kTextMuted;
    case Answer::None:
        break;
    }
    return theme::kAccent;
}

} // namespace

std::vector<std::string> Review::listImages(const std::filesystem::path& folder) {
    std::vector<std::string> images;
    std::error_code error;
    for (std::filesystem::directory_iterator it(folder, error), end; !error && it != end; it.increment(error)) {
        std::wstring extension = it->path().extension().wstring();
        for (wchar_t& c : extension) {
            c = static_cast<wchar_t>(std::towlower(c));
        }
        if (extension == L".jpg" || extension == L".jpeg" || extension == L".png") {
            images.push_back(toUtf8(it->path().filename()));
        }
    }
    std::sort(images.begin(), images.end());
    return images;
}

Review::Review(ID3D11Device* device, std::filesystem::path folder, const Category& category)
    : device_(device), folder_(std::move(folder)), category_(category) {
    images_ = listImages(folder_);
    if (images_.empty()) {
        throw std::runtime_error("This folder holds no recorded images.");
    }
    loadGuesses();
    answers_ = parseMarks(readTextFile(marksFile(folder_, category_, "answers")));
    if (answers_.size() < images_.size() && Finder::needed(folder_, category_.id)) {
        try {
            finder_ = std::make_unique<Finder>(folder_, category_.id);
        } catch (const std::exception& e) {
            finderError_ = e.what();
        }
    }

    std::size_t start = 0;
    while (start + 1 < images_.size() && answers_.contains(images_[start])) {
        ++start;
    }
    show(start);
}

void Review::update() {
    if (!finder_) {
        return;
    }
    switch (finder_->poll()) {
    case Finder::State::Running:
        return;
    case Finder::State::Done:
        loadGuesses();
        // The image shown gets the guesses too, unless the user already started on it.
        if (!reviewed_ && !drafts_.contains(images_[index_]) && !dragging_) {
            loadMarks();
        }
        break;
    case Finder::State::Failed:
        finderError_ = finder_->error();
        break;
    }
    finder_.reset();
}

void Review::previous() {
    if (canGoBack()) {
        show(index_ - 1);
    }
}

void Review::next() {
    if (!canGoOn()) {
        return;
    }
    for (Mark& mark : marks_) {
        if (mark.answer == Answer::None) {
            mark.answer = Answer::No;
        }
    }
    reviewed_ = true;
    drafts_.erase(images_[index_]);
    changed();
    if (index_ + 1 < images_.size()) {
        show(index_ + 1);
    }
}

void Review::show(std::size_t index) {
    index_ = index;
    dragging_ = false;
    hovered_ = -1;
    loadMarks();
    loadImage();
}

void Review::loadMarks() {
    const std::string& name = images_[index_];
    reviewed_ = answers_.contains(name);
    if (reviewed_) {
        marks_ = answers_.at(name);
    } else if (const auto draft = drafts_.find(name); draft != drafts_.end()) {
        marks_ = draft->second;
    } else {
        const auto guessed = guesses_.find(name);
        marks_ = guessed != guesses_.end() ? guessed->second : std::vector<Mark>{};
        // The same things are usually still there, close to where they were: reuse the answers of the image
        // before, if it is reviewed.
        if (index_ > 0) {
            if (const auto before = answers_.find(images_[index_ - 1]); before != answers_.end()) {
                carryAnswers(before->second, marks_);
            }
        }
    }
    selected_ = nextUnanswered(-1);
}

void Review::loadGuesses() {
    const std::filesystem::path file = marksFile(folder_, category_, "guesses");
    std::error_code error;
    hasGuesses_ = std::filesystem::exists(file, error);
    guesses_ = parseMarks(readTextFile(file));
}

void Review::loadImage() {
    imageError_.clear();
    try {
        const ImageData image = readImageFile(folder_ / fromUtf8(images_[index_]));
        if (!image_ || image_->width() != image.width || image_->height() != image.height) {
            image_ = std::make_unique<ScreenImage>(device_, image.width, image.height);
        }
        image_->upload(image.bgra.data(), image.width * 4);
    } catch (const std::exception& e) {
        imageError_ = e.what();
    }
}

void Review::changed() {
    const std::string& name = images_[index_];
    if (reviewed_) {
        answers_[name] = marks_;
        save();
    } else {
        drafts_[name] = marks_;
    }
}

void Review::save() {
    char comment[512];
    std::snprintf(comment, sizeof(comment),
                  "observex review answers, category: %s.\n"
                  "Each \"image\" line is an image the user reviewed; its marks follow, one per line:\n"
                  "yes or no (whether it is in the category), then x, y, width and height in image pixels.\n"
                  "On a reviewed image, everything in the category is marked yes.",
                  category_.id);
    const std::filesystem::path file = marksFile(folder_, category_, "answers");
    std::filesystem::path temporary = file;
    temporary += L".tmp";
    {
        std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
        out << formatMarks(answers_, comment);
        if (!out) {
            saveError_ = "Could not save the answers to " + toUtf8(file);
            return;
        }
    }
    // Replacing the file in one step: a crash never leaves it half written.
    std::error_code error;
    std::filesystem::rename(temporary, file, error);
    saveError_ = error ? "Could not save the answers to " + toUtf8(file) : std::string();
}

void Review::answer(Answer answer) {
    if (selected_ < 0) {
        selected_ = nextUnanswered(-1);
    }
    if (selected_ < 0) {
        return;
    }
    Mark& mark = marks_[static_cast<std::size_t>(selected_)];
    mark.answer = answer;
    mark.carried = false;
    changed();
    if (const int following = nextUnanswered(selected_); following >= 0) {
        selected_ = following;
    }
}

void Review::removeSelected() {
    // Only marks the user drew: the finder's guesses are answered no instead, which teaches it something.
    if (selected_ < 0 || marks_[static_cast<std::size_t>(selected_)].guess >= 0.0f) {
        return;
    }
    marks_.erase(marks_.begin() + selected_);
    selected_ = -1;
    hovered_ = -1;
    changed();
}

int Review::nextUnanswered(int after) const {
    const int count = static_cast<int>(marks_.size());
    for (int i = 1; i <= count; ++i) {
        const int candidate = ((after < 0 ? -1 : after) + i) % count;
        if (marks_[static_cast<std::size_t>(candidate)].answer == Answer::None) {
            return candidate;
        }
    }
    return -1;
}

int Review::markAt(ImVec2 pixel) const {
    int found = -1;
    long long foundArea = 0;
    for (std::size_t i = 0; i < marks_.size(); ++i) {
        const Box& box = marks_[i].box;
        const long long area = static_cast<long long>(box.width) * box.height;
        if (box.contains(static_cast<int>(pixel.x), static_cast<int>(pixel.y)) && (found < 0 || area < foundArea)) {
            found = static_cast<int>(i);
            foundArea = area;
        }
    }
    return found;
}

ImVec2 Review::toScreen(int x, int y) const {
    return ImVec2(imageMin_.x + static_cast<float>(x) * scale_, imageMin_.y + static_cast<float>(y) * scale_);
}

ImVec2 Review::toImage(ImVec2 screen) const {
    const float x = (screen.x - imageMin_.x) / scale_;
    const float y = (screen.y - imageMin_.y) / scale_;
    return ImVec2(std::clamp(x, 0.0f, static_cast<float>(image_->width())),
                  std::clamp(y, 0.0f, static_cast<float>(image_->height())));
}

Box Review::dragBox() const {
    const ImVec2 end = toImage(ImGui::GetMousePos());
    const int left = static_cast<int>(std::round(std::min(dragStart_.x, end.x)));
    const int top = static_cast<int>(std::round(std::min(dragStart_.y, end.y)));
    const int right = static_cast<int>(std::round(std::max(dragStart_.x, end.x)));
    const int bottom = static_cast<int>(std::round(std::max(dragStart_.y, end.y)));
    return Box{left, top, right - left, bottom - top};
}

void Review::handleKeys() {
    if (ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId) || ImGui::GetIO().WantTextInput) {
        return;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Y, false)) {
        answer(Answer::Yes);
    }
    if (ImGui::IsKeyPressed(ImGuiKey_N, false)) {
        answer(Answer::No);
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Delete, false) || ImGui::IsKeyPressed(ImGuiKey_Backspace, false)) {
        removeSelected();
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Tab, false) && !marks_.empty()) {
        const int following = nextUnanswered(selected_);
        selected_ = following >= 0 ? following : (selected_ + 1) % static_cast<int>(marks_.size());
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
        selected_ = -1;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_RightArrow) || ImGui::IsKeyPressed(ImGuiKey_D)) {
        next();
    } else if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow) || ImGui::IsKeyPressed(ImGuiKey_A)) {
        previous();
    }
}

void Review::handleMouse() {
    // The image is one invisible button: pressing on a mark selects it, pressing elsewhere starts a new mark.
    const ImVec2 pixel = toImage(ImGui::GetMousePos());
    hovered_ = ImGui::IsItemHovered() && !dragging_ ? markAt(pixel) : -1;
    if (ImGui::IsItemActivated()) {
        if (hovered_ >= 0) {
            selected_ = hovered_;
        } else {
            dragging_ = true;
            dragStart_ = pixel;
            selected_ = -1;
        }
    }
    if (dragging_ && ImGui::IsItemDeactivated()) {
        dragging_ = false;
        const Box box = dragBox();
        const float smallest = 4.0f * ImGui::GetStyle().FontScaleDpi; // On screen: a plain click is no mark.
        if (static_cast<float>(box.width) * scale_ >= smallest && static_cast<float>(box.height) * scale_ >= smallest) {
            marks_.push_back(Mark{box, Answer::Yes});
            selected_ = static_cast<int>(marks_.size()) - 1;
            changed();
        }
    }
    if (hovered_ >= 0) {
        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    }
}

void Review::draw(const theme::Fonts& fonts) {
    handleKeys(); // First: it may change the image.

    const float dpi = ImGui::GetStyle().FontScaleDpi;
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const ImVec2 available = ImGui::GetContentRegionAvail();
    const float statusHeight = std::round(ImGui::GetTextLineHeight() + 12.0f * dpi);
    const ImVec2 area(available.x, std::max(available.y - statusHeight, 1.0f));
    const ImVec2 viewMax(origin.x + area.x, origin.y + area.y);

    if (!imageError_.empty() || !image_) {
        const char* title = "This image cannot be shown.";
        const float middle = origin.y + area.y * 0.5f;
        drawList->AddText(ImVec2(std::round(origin.x + (area.x - ImGui::CalcTextSize(title).x) * 0.5f), middle),
                          theme::kText, title);
        drawList->AddText(ImVec2(std::round(origin.x + (area.x - ImGui::CalcTextSize(imageError_.c_str()).x) * 0.5f),
                                 middle + ImGui::GetTextLineHeightWithSpacing()),
                          theme::kTextMuted, imageError_.c_str());
    } else {
        // The image as large as fits, keeping its shape.
        const float aspect = static_cast<float>(image_->width()) / static_cast<float>(image_->height());
        const ImVec2 size =
            area.x / area.y > aspect ? ImVec2(area.y * aspect, area.y) : ImVec2(area.x, area.x / aspect);
        imageMin_ =
            ImVec2(std::round(origin.x + (area.x - size.x) * 0.5f), std::round(origin.y + (area.y - size.y) * 0.5f));
        const ImVec2 imageMax(imageMin_.x + std::round(size.x), imageMin_.y + std::round(size.y));
        scale_ = (imageMax.x - imageMin_.x) / static_cast<float>(image_->width());
        const float rounding = 6.0f * dpi;
        image_->draw(drawList, imageMin_, imageMax, rounding, theme::kBackground);

        ImGui::SetCursorScreenPos(imageMin_);
        ImGui::InvisibleButton("##image", ImVec2(imageMax.x - imageMin_.x, imageMax.y - imageMin_.y));
        handleMouse();
        drawList->PushClipRect(imageMin_, imageMax, true);
        drawMarks(drawList, fonts);
        drawList->PopClipRect();

        // A note at the top: the finder at work, why it failed, or that nothing is marked.
        char text[160] = {};
        char detail[200] = {};
        if (finder_) {
            std::snprintf(text, sizeof(text), "Looking for %s with the trained model", category_.nouns);
            if (finder_->total() > 0) {
                std::snprintf(detail, sizeof(detail), "Image %d of %d. Its marks show here when it is done.",
                              finder_->done(), finder_->total());
            } else {
                std::snprintf(detail, sizeof(detail), "Starting it (a few seconds).");
            }
        } else if (!finderError_.empty()) {
            std::snprintf(text, sizeof(text), "The trained model could not look at this recording.");
            std::snprintf(detail, sizeof(detail), "%.190s", finderError_.c_str());
        } else if (!hasGuesses_ && marks_.empty()) {
            std::snprintf(text, sizeof(text), "Nothing is marked: the finder has not looked at this recording yet.");
            std::snprintf(detail, sizeof(detail), "Drag a box around each %s it should learn.", category_.noun);
        }
        if (text[0] != '\0' && !dragging_) {
            theme::drawLabel(drawList, ImVec2((imageMin_.x + imageMax.x) * 0.5f, imageMin_.y + 12.0f * dpi),
                             ImVec2(0.5f, 0.0f), text, detail);
        }
        drawAnswerBox(fonts, origin, viewMax);
    }
    drawStatus(drawList, ImVec2(origin.x, viewMax.y), ImVec2(viewMax.x, origin.y + available.y));
}

void Review::drawMarks(ImDrawList* drawList, const theme::Fonts& fonts) const {
    const float dpi = ImGui::GetStyle().FontScaleDpi;
    const ImVec2 imageMax(imageMin_.x + static_cast<float>(image_->width()) * scale_,
                          imageMin_.y + static_cast<float>(image_->height()) * scale_);

    // The selected mark last, so it is on top.
    for (int pass = 0; pass < 2; ++pass) {
        for (std::size_t i = 0; i < marks_.size(); ++i) {
            const int index = static_cast<int>(i);
            const bool selected = index == selected_;
            if (selected != (pass == 1)) {
                continue;
            }
            const Mark& mark = marks_[i];
            const bool lit = selected || index == hovered_;
            const ImU32 color = markColor(mark);
            const ImVec2 min = toScreen(mark.box.x, mark.box.y);
            const ImVec2 max = toScreen(mark.box.x + mark.box.width, mark.box.y + mark.box.height);
            const float thickness = (selected ? 3.0f : lit ? 2.5f : mark.answer == Answer::No ? 1.0f : 1.5f) * dpi;
            if (selected) {
                drawList->AddRectFilled(min, max, (color & 0x00FFFFFFu) | 0x22000000u);
            }
            theme::drawBox(drawList, min, max, color, thickness);

            // A tag above the box says what it is. Marks answered no only get one while pointed at.
            if (mark.answer == Answer::No && !lit) {
                continue;
            }
            char text[96];
            if (mark.answer == Answer::None) {
                if (mark.guess >= 0.0f) {
                    std::snprintf(text, sizeof(text), "%s? %.0f%%", category_.yes,
                                  static_cast<double>(mark.guess * 100.0f));
                } else {
                    std::snprintf(text, sizeof(text), "%s?", category_.yes);
                }
            } else {
                std::snprintf(text, sizeof(text), "%s%s", mark.answer == Answer::Yes ? category_.yes : category_.no,
                              mark.carried ? "  \xC2\xB7  copied" : "");
            }
            const bool dim = mark.answer == Answer::No;
            theme::drawTag(drawList, fonts.medium, min, thickness, imageMin_, imageMax, text,
                           dim ? theme::kSurfaceActive : color, dim ? theme::kText : theme::kBackground);
        }
    }

    if (dragging_) {
        const Box box = dragBox();
        const ImVec2 min = toScreen(box.x, box.y);
        const ImVec2 max = toScreen(box.x + box.width, box.y + box.height);
        theme::drawBox(drawList, min, max, theme::kYes, 2.0f * dpi);
    }
}

void Review::drawAnswerBox(const theme::Fonts& fonts, ImVec2 viewMin, ImVec2 viewMax) {
    if (selected_ < 0 || dragging_) {
        return;
    }
    const float dpi = ImGui::GetStyle().FontScaleDpi;
    const Mark& mark = marks_[static_cast<std::size_t>(selected_)];
    const ImVec2 min = toScreen(mark.box.x, mark.box.y);
    const ImVec2 max = toScreen(mark.box.x + mark.box.width, mark.box.y + mark.box.height);

    // Just the buttons: the mark's tag already asks the question ("Monster? 81%").
    BarButton yes;
    yes.text = "Yes";
    yes.detail = "Y";
    yes.open = mark.answer == Answer::Yes;
    yes.highlight = theme::kYes;
    BarButton no;
    no.text = "No";
    no.detail = "N";
    no.open = mark.answer == Answer::No;
    no.highlight = theme::kTextMuted;
    BarButton remove; // Only for marks the user drew.
    remove.text = "Remove";
    remove.detail = "Del";
    const bool drawn = mark.guess < 0.0f;
    const float spacing = std::round(6.0f * dpi);
    const ImVec2 padding(spacing, spacing);
    const float width = padding.x * 2.0f + yes.width(fonts) + spacing + no.width(fonts) +
                        (drawn ? spacing + remove.width(fonts) : 0.0f);
    const float height = padding.y * 2.0f + std::round(BarButton::kHeight * dpi);

    // Under the mark, so its tag stays readable; above the tag if there is no room below.
    const float gap = std::round(8.0f * dpi);
    const float tagHeight = std::round(ImGui::GetFontSize() * 0.87f) + std::round(8.0f * dpi);
    ImVec2 position(std::round((min.x + max.x - width) * 0.5f), max.y + gap);
    if (position.y + height > viewMax.y) {
        position.y = min.y - tagHeight - gap - height;
    }
    position.x = std::max(std::min(position.x, viewMax.x - width), viewMin.x);
    position.y = std::max(std::min(position.y, viewMax.y - height), viewMin.y);
    ImGui::SetNextWindowPos(ImVec2(std::round(position.x), std::round(position.y)));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, padding);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 10.0f * dpi);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, theme::kSurface);
    ImGui::Begin("##answer", nullptr, kAnswerBox);
    if (yes.draw("##yes", fonts)) {
        answer(Answer::Yes);
    }
    ImGui::SameLine(0.0f, spacing);
    if (no.draw("##no", fonts)) {
        answer(Answer::No);
    }
    if (drawn) {
        ImGui::SameLine(0.0f, spacing);
        if (remove.draw("##remove", fonts)) {
            removeSelected();
        }
    }
    ImGui::End();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(3);
}

void Review::drawStatus(ImDrawList* drawList, ImVec2 min, ImVec2 max) const {
    // Left: where we are and what is left on this image. Right: the keys, or why saving failed.
    const float dpi = ImGui::GetStyle().FontScaleDpi;
    const float textY = std::round(max.y - ImGui::GetTextLineHeight() - 2.0f * dpi);

    int unanswered = 0;
    for (const Mark& mark : marks_) {
        unanswered += mark.answer == Answer::None ? 1 : 0;
    }
    char where[400];
    int length = std::snprintf(where, sizeof(where), "%s  /  %s  \xC2\xB7  %s  \xC2\xB7  %zu %s",
                               toUtf8(folder_.filename()).c_str(), images_[index_].c_str(), category_.name,
                               marks_.size(), marks_.size() == 1 ? "mark" : "marks");
    if (unanswered > 0 && length > 0 && static_cast<std::size_t>(length) < sizeof(where)) {
        std::snprintf(where + length, sizeof(where) - static_cast<std::size_t>(length),
                      ", %d unanswered (no, once you move on)", unanswered);
    } else if (reviewed_ && length > 0 && static_cast<std::size_t>(length) < sizeof(where)) {
        std::snprintf(where + length, sizeof(where) - static_cast<std::size_t>(length), "  \xC2\xB7  reviewed");
    }
    drawList->AddText(ImVec2(min.x, textY), theme::kTextMuted, where);

    const char* keys = saveError_.empty()
                           ? "Drag: mark a missed one     Y / N: answer     Tab: next mark     Left / Right: images"
                           : saveError_.c_str();
    const float keysWidth = ImGui::CalcTextSize(keys).x;
    drawList->AddText(ImVec2(std::round(max.x - keysWidth), textY),
                      saveError_.empty() ? theme::kTextMuted : theme::kRecord, keys);
}

} // namespace observex
