#pragma once

#include "app/ScreenImage.h"
#include "app/Theme.h"
#include "core/Marks.h"
#include "engine/Finder.h"

#include <imgui.h>

#include <cstddef>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace observex {

// What a review looks for. Only monsters for now; party members, terrain, UI and more come later, each as one
// more entry in kCategories.
struct Category {
    const char* id;          // In the review's file names, e.g. "monster" for monster-answers.txt.
    const char* name;        // "Monsters".
    const char* noun;        // One of them, in running text: "monster".
    const char* nouns;       // Several, in running text: "monsters".
    const char* description; // One line for the category chooser.
    const char* yes;         // The label of a mark answered yes: "Monster".
    const char* no;          // The label of a mark answered no: "Not a monster".
};

inline constexpr Category kCategories[] = {
    {"monster", "Monsters", "monster", "monsters", "Creatures you can fight", "Monster", "Not a monster"},
};

// The review screen. Shows a recording's images one at a time with the things marked on them. The user answers
// for each mark whether it belongs to the category (e.g. is a monster), and drags a box around anything it missed.
//
// It reads and writes two files in the recording's folder, in the format of core/Marks.h (<id> is the category's):
// - <id>-guesses.txt: the marks the finder made, with how sure it was. Read only.
// - <id>-answers.txt: the user's answers. An image is in it once the user moved on from it with Next; it is then
//   "reviewed": everything in the category on it is marked yes. Unanswered marks count as no when moving on.
//   Saved on every change to a reviewed image.
// Answers carry forward: a mark on the next image that overlaps an answered one gets the same answer. Guesses the
// finder is sure of (75% or more) then start as yes, so the user only answers the unsure ones and flips wrong ones.
// If a trained model exists (models/<id>/model.onnx) and the recording's guesses are missing or older than it, the
// Finder writes them in the background while the review is open; they show as soon as it is done.
class Review {
public:
    // The image files of a recording (.jpg or .png), in name order. Empty if there are none.
    static std::vector<std::string> listImages(const std::filesystem::path& folder);

    // Opens the recording in `folder` at its first image not reviewed yet, and starts the finder if needed. Throws if
    // the folder holds no images.
    Review(ID3D11Device* device, std::filesystem::path folder, const Category& category);

    Review(const Review&) = delete;
    Review& operator=(const Review&) = delete;

    const std::filesystem::path& folder() const { return folder_; }
    const Category& category() const { return category_; }

    // The image shown (0-based) among all `imageCount()`, and how many images are reviewed.
    std::size_t index() const { return index_; }
    std::size_t imageCount() const { return images_.size(); }
    std::size_t reviewedCount() const { return answers_.size(); }

    bool canGoBack() const { return index_ > 0; }
    bool canGoOn() const { return index_ + 1 < images_.size() || !reviewed_; }
    void previous();
    // Marks the image as reviewed (unanswered marks become no), saves, and shows the next one.
    void next();

    // Follows the finder, and shows its guesses once it is done. Call on every pass of the main loop, outside an
    // ImGui frame, also while the window is minimized.
    void update();

    // Draws the image with its marks, the answer box of the selected mark and a status line, into the rest of the
    // current window. Handles the mouse and keys (call before anything else draws the image this frame).
    void draw(const theme::Fonts& fonts);

private:
    void show(std::size_t index);
    // The marks of the image shown: its answers, its draft, or the finder's guesses with answers carried forward.
    void loadMarks();
    void loadGuesses();
    void loadImage();
    // Call after marks_ changed: keeps the change, and saves it if the image is reviewed.
    void changed();
    void save();

    void answer(Answer answer);
    void removeSelected();
    // The first unanswered mark after `after` (wrapping around), or -1.
    int nextUnanswered(int after) const;
    // The smallest mark containing the image pixel, or -1.
    int markAt(ImVec2 pixel) const;

    void handleKeys();
    void handleMouse();
    void drawMarks(ImDrawList* drawList, const theme::Fonts& fonts) const;
    void drawAnswerBox(const theme::Fonts& fonts, ImVec2 viewMin, ImVec2 viewMax);
    void drawStatus(ImDrawList* drawList, ImVec2 min, ImVec2 max) const;

    ImVec2 toScreen(int x, int y) const;
    ImVec2 toImage(ImVec2 screen) const; // Clamped to the image.
    Box dragBox() const;

    ID3D11Device* device_;
    std::filesystem::path folder_;
    const Category& category_;
    std::vector<std::string> images_; // File names, in name order.
    std::unique_ptr<Finder> finder_;  // While it writes this recording's guesses.
    std::string finderError_;         // Why it could not.
    bool hasGuesses_ = false;         // Whether the finder has looked at this recording.
    ImageMarks guesses_;              // From <id>-guesses.txt.
    ImageMarks answers_;              // Reviewed images; what <id>-answers.txt holds.
    ImageMarks drafts_;               // Answers on images not reviewed yet; kept until the review closes.

    std::size_t index_ = 0;   // The image shown.
    std::vector<Mark> marks_; // Its marks.
    bool reviewed_ = false;   // Whether it is in answers_.
    std::unique_ptr<ScreenImage> image_;
    std::string imageError_; // Why the image could not be shown.
    std::string saveError_;  // Why the answers could not be saved.

    int selected_ = -1; // The mark whose answer box is open.
    int hovered_ = -1;
    bool dragging_ = false; // Drawing a new mark.
    ImVec2 dragStart_;      // In image pixels.
    ImVec2 imageMin_;       // Where the image is drawn on screen, and its size there relative to its pixels.
    float scale_ = 1.0f;
};

} // namespace observex
