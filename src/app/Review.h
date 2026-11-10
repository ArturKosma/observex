#pragma once

#include "app/ReviewLog.h"
#include "app/ScreenImage.h"
#include "app/Theme.h"
#include "core/BoxLearner.h"
#include "core/Features.h"
#include "core/ImageView.h"
#include "core/KindLearner.h"
#include "core/Marks.h"
#include "core/ReviewHistory.h"
#include "engine/Finder.h"

#include <imgui.h>

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace observex {

// One kind of thing in a category, e.g. a monster among characters. A yes answer says which kind it is.
struct Kind {
    std::string_view id; // In the answers file: "monster".
    const char* name;    // On marks and buttons: "Monster".
    ImU32 color;         // Of its marks.
};

// The kinds of characters (the user asked, 2026-11-02): the bot attacks monsters, not players or NPCs, and has to
// know which one is itself. The user's own character is a kind of its own: merging it into players later is one
// line in training, splitting it out later would mean checking every box again. The first is the default.
// "Can't tell" (2026-11-08): a character too far off to tell which kind, which the user could only guess at or answer
// no (teaching the model that a character is none). A model learns it as a kind of its own from these answers, so its
// "can't tell" follows what the user can tell. "Dead" (2026-11-08): a corpse, which the user answered no before. The
// ids and their order are core/Marks' kCharacterKindIds.
inline constexpr Kind kCharacterKinds[] = {
    {kCharacterKindIds[0], "Monster", theme::kYes},
    {kCharacterKindIds[1], "Player", theme::kPlayer},
    {kCharacterKindIds[2], "NPC", theme::kNpc},
    {kCharacterKindIds[3], "You", theme::kOwn},
    {kCharacterKindIds[4], "Can't tell", theme::kUnknown},
    {kCharacterKindIds[5], "Dead", theme::kDead},
};
static_assert(std::size(kCharacterKinds) == std::size(kCharacterKindIds));

// What a review looks for. Only characters for now; terrain, UI and more come later, each as one more entry in
// kCategories.
struct Category {
    const char* id;              // In the review's file names, e.g. "character" for character-answers.txt.
    const char* name;            // "Characters".
    const char* noun;            // One of them, in running text: "character".
    const char* nouns;           // Several, in running text: "characters".
    const char* description;     // One line for the category chooser.
    const char* yes;             // The label of a mark answered yes: "Character".
    const char* no;              // The label of a mark answered no: "Not a character".
    std::span<const Kind> kinds; // What a yes can be (keys 1, 2, ...); empty if a yes is just a yes.
};

inline constexpr Category kCategories[] = {
    {"character", "Characters", "character", "characters", "Monsters, NPCs, other players and you", "Character",
     "Not a character", kCharacterKinds},
};

// The review screen. Shows a recording's images one at a time with the things marked on them. The user answers
// for each mark whether it belongs to the category, and which kind it is (e.g. a monster, key 1), and drags a box
// around anything it missed.
//
// It reads and writes these files in the recording's folder, in the format of core/Marks.h (<id> is the category's):
// - <id>-guesses.txt: the marks the finder made, with how sure it was. Read only.
// - <id>-features.bin: what the model saw of each guess (core/Features), from the same finder run. Read only.
// - <id>-answers.txt: the user's answers. An image is in it once the user moved on from it with Next, which needs
//   every mark on it answered (the user asked, 2026-11-01); it is then "reviewed": everything in the category on it
//   is marked with its kind. Saved on every change to a reviewed image.
// Images answered before the category had kinds hold yes marks whose kind is not checked yet (a suggestion: the
// first kind, or one the file names). They count as not reviewed: the review opens at the first of them, the user
// fixes the wrong kinds, and Next checks them all. So do reviewed images holding a guess added since (2026-11-09, the
// user asked: a sweep for characters nobody marked, things the model was sure of that no answer covered): the review
// picks it, and it waits for its answer like any guess. Shift+Right (or Shift+D) moves on to the next image with
// something to check, past the reviewed ones; the bar says how many there are.
// A click on a mark picks it; a drag draws a new mark wherever it starts, also over a bigger mark (a character behind
// the user's own one, the user asked, 2026-11-01). An answer picks the next unanswered mark, or none once all are
// answered (2026-11-08: the user clicked beside the marks for that after nearly every last answer).
// The mouse wheel zooms in on the image (the user asked, 2026-11-08: a crowd of far-off characters was too small to
// answer) from 100%, the whole image, to 800%, keeping the pixel under the mouse where it is (core/ImageView); a drag
// with the right or middle button moves around, 0 fits the whole image again, and a label on the image says how far
// in it is. The zoom stays from image to image (a crowd stays a few images). Zoomed in, the review picks marks in
// view first (after an answer, Tab, on a new image), and when it picks one out of view (none left in view, Next with
// marks unanswered, Ctrl+Z) it moves the view to it, so nothing is answered unseen.
// Answers carry forward: a mark on the next image that overlaps an answered one gets the same answer and kind (also
// the suggested kinds on images answered before kinds); not onto a guess the review is sure is something else (a "no"
// onto a sure guess, another kind than the sure one), which is asked (core/Marks' carryAnswers: the camera may have
// moved, or two things swapped places). Guesses the finder is sure of (kPresumeYes of the kind) then
// start as yes, of the kind it guessed, so the user only answers the unsure ones and flips wrong ones. With a model
// trained with kinds, a guess whose two surest kinds are close (core/Detection's kindsClose) is unsure too: its tag
// names both ("Monster 55% or Player 40%?") and it waits for the user's answer.
// It learns while the user answers (the user asked, 2026-11-08), with a model that gives features and a kind head
// (model.json): before showing an image not reviewed yet, the model's kind classifier is re-fit to every answered
// guess of the recording so far (core/KindLearner), and the guesses' kinds and tags come from the re-fit (a party
// member corrected to Player shows "Player? 60%" on the next images, not "Monster? 90%"). A guess then starts as yes
// when the model and the re-fit agree on its kind, or the re-fit is very sure (core/Marks' presumedKind). The status
// line says how many answers it learned from. Without features (an older model) it is as before. With a kind update
// (kinds-update.json next to the model, learned from every recording's answers after a review, engine/KindUpdater),
// "the model's" classifier here is the update's: the re-fit starts from it and is pulled back to it.
// If a trained model exists (models/<id>/model.onnx) and the recording's guesses are missing or older than it, the
// Finder writes them in the background while the review is open; they show as soon as it is done.
// Ctrl+Z undoes the last answer, drawn or removed box, or move to another image (core/ReviewHistory), last first.
// Closing a review that changed the answers file saves when, in recording.observex, for the list.
// Everything the user does, with what the review showed and why, goes to <id>-review-log.txt (app/ReviewLog).

// How far a recording's review for one category got.
struct ReviewCount {
    std::size_t reviewed = 0; // Images reviewed, kinds checked and every mark answered.
    std::size_t toCheck = 0;  // Images answered before kinds or holding a guess added since, waiting for the user.
};

class Review {
public:
    // How many of a recording's images are reviewed for `category`.
    static ReviewCount countReviewed(const std::filesystem::path& folder, const Category& category);

    // Opens the recording in `folder` at its first image not reviewed yet, and starts the finder if needed. Throws if
    // the folder holds no images.
    Review(ID3D11Device* device, std::filesystem::path folder, const Category& category);
    // Closing the review: if it changed the answers (any answer, kind or box on a reviewed image, or an image newly
    // reviewed), saves the time in the recording's recording.observex (the user asked, 2026-11-07).
    ~Review();

    Review(const Review&) = delete;
    Review& operator=(const Review&) = delete;

    const std::filesystem::path& folder() const { return folder_; }
    const Category& category() const { return category_; }
    // Whether its finder is still writing this recording's guesses (the GPU is then the finder's).
    bool finding() const { return finder_ != nullptr; }

    // The image shown (0-based) among all `imageCount()`, how many images are reviewed (their kinds checked and every
    // mark answered too), and how many answered ones still wait for the user (kinds to check, a guess added since).
    std::size_t index() const { return index_; }
    std::size_t imageCount() const { return images_.size(); }
    std::size_t reviewedCount() const;
    std::size_t toCheckCount() const;

    // Marks on the image shown that are not answered yet. Moving on needs none.
    int unansweredCount() const;
    // Whether the image shown was answered before kinds, and its kinds wait for the user to check them.
    bool kindsToCheck() const { return !kindsChecked(marks_); }
    bool canGoBack() const { return index_ > 0; }
    bool canGoOn() const {
        return (index_ + 1 < images_.size() || !reviewed_ || kindsToCheck()) && unansweredCount() == 0;
    }
    void previous();
    // Marks the image as reviewed with every kind on it checked, saves, and shows the next one. With marks still
    // unanswered it picks the first of them instead.
    void next();
    // As next(), but shows the next image not reviewed yet (kinds to check, a guess to answer, or not answered at
    // all), past the reviewed ones; from the first image again after the last. Stays if there is none.
    void nextToCheck();

    // Follows the finder, and shows its guesses once it is done. Call on every pass of the main loop, outside an
    // ImGui frame, also while the window is minimized.
    void update();

    // Draws the image with its marks, the answer box of the selected mark and a status line, into the rest of the
    // current window. Handles the mouse and keys (call before anything else draws the image this frame).
    void draw(const theme::Fonts& fonts);

private:
    // next() and nextToCheck(): leaves the image shown as reviewed, then shows image `to` (stays if past the last).
    // `why` is for the log.
    void moveOn(std::size_t to, const char* why);
    void show(std::size_t index);
    // The marks of the image shown: its answers, its draft, or the finder's guesses with answers carried forward.
    void loadMarks();
    // The guesses, and their features if they are from the same finder run and the current model.
    void loadGuesses();
    void loadImage();
    // Whether the guesses' kinds come from the re-fit: the model has a kind head and the guesses have features.
    bool learning() const;
    // Whether the guesses' boxes are nudged the way the user boxes them (core/BoxLearner): the guesses have features.
    bool nudging() const;
    // Re-fits the kinds to every answered guess on the reviewed images, if the answers changed since the last fit, and
    // the box nudge to them for the image shown (the nearer images count more) if either changed.
    void learn();
    // Call after marks_ changed: keeps the change, and saves it if the image is reviewed.
    void changed();
    void save();

    // Answers the selected mark (or the first unanswered one). A yes takes `kind` (an index into the category's
    // kinds), or keeps the kind it shows if -1. `how`, for the log: "key 1", "button", ...
    void answer(Answer answer, int kind, const char* how);
    void removeSelected();
    // Selects mark `mark` (-1: none) and logs it, `how` ("click", "Tab", ...), if that changes the selection.
    void select(int mark, const char* how);

    // The review log (app/ReviewLog, core/ReviewLines). The image shown, how it starts and why (`why`: "opened",
    // "next", ...); remembers it as shown, to log what the user changes on it.
    void logImage(const char* why);
    // Leaving the image shown (`how`: "done" with Next, "back"): what it holds and what the user changed on it.
    void logLeave(const char* how);
    // "2.8 s" since the image shown appeared.
    std::string sinceShown() const;
    // Call just before the user changes the image shown or moves on from it: a step Ctrl+Z can undo. `selected` is
    // the mark to select after undoing it.
    void remember(int selected);
    // Ctrl+Z: puts the image of the last step back as it was, and shows it.
    void undo();
    ImU32 markColor(const Mark& mark) const;
    // The first unanswered mark after `after` (wrapping around), those in view first when zoomed in; or -1.
    int nextUnanswered(int after) const;
    // Whether the middle of `box` is in the part of the image in view (always at 100%).
    bool inView(const Box& box) const;
    // The smallest mark containing the image pixel, or -1.
    int markAt(ImVec2 pixel) const;

    void handleKeys();
    // The wheel zooms, a right or middle drag moves the view. Call right after the image's invisible button.
    void handleZoom();
    void handleMouse();
    // Sets imageMin_ and scale_ from view_ and where the view is on screen.
    void placeImage();
    // The zoom label at the view's top right corner: while zoomed in, and a moment after coming back to 100%.
    void drawZoom(ImDrawList* drawList) const;
    // ", zoomed in 400%" for the log while zoomed in, else empty.
    std::string zoomText() const;
    // Returns where the selected mark's tag is (empty if none is selected), for the answer box to leave it free.
    theme::Area drawMarks(ImDrawList* drawList, const theme::Fonts& fonts) const;
    void drawAnswerBox(const theme::Fonts& fonts, ImVec2 viewMin, ImVec2 viewMax, theme::Area tag);
    void drawStatus(ImDrawList* drawList, ImVec2 min, ImVec2 max) const;

    ImVec2 toScreen(int x, int y) const;
    ImVec2 toImage(ImVec2 screen) const; // Clamped to the part of the image in view.
    Box dragBox() const;
    // Whether the mouse moved far enough while down to draw a mark, not just click.
    bool drawing() const;

    ID3D11Device* device_;
    std::filesystem::path folder_;
    const Category& category_;
    std::vector<std::string_view> kindIds_; // The category's kinds as the answers file names them.
    std::vector<std::string> images_;       // File names, in name order.
    std::unique_ptr<Finder> finder_;        // While it writes this recording's guesses.
    std::string finderError_;               // Why it could not.
    bool hasGuesses_ = false;               // Whether the finder has looked at this recording.
    ImageMarks guesses_;                    // From <id>-guesses.txt.
    GuessFeatures features_;                // Of guesses_, from <id>-features.bin; empty if there are none.
    std::unique_ptr<KindLearner> learner_;  // With a model whose model.json holds its kind head.
    bool relearn_ = true;                   // answers_ changed since the last fit.
    std::string fitText_;                   // The fit made for the image shown, for the log; empty if none.
    BoxLearner boxLearner_;                 // Nudges the guesses' boxes (nudging()).
    std::size_t boxFitFor_ = static_cast<std::size_t>(-1); // The image its last fit was for.
    std::string boxFitText_;                // That fit, for the log.
    ImageMarks answers_;                    // Reviewed images; what <id>-answers.txt holds.
    ImageMarks drafts_;                     // Answers on images not reviewed yet; kept until the review closes.
    std::string openedAnswers_;             // answers_ as the file held them when the review opened.
    ReviewHistory history_;                 // What Ctrl+Z undoes.

    std::unique_ptr<ReviewLog> log_;
    std::chrono::steady_clock::time_point openedAt_;
    std::size_t reviewedAtOpen_ = 0;
    std::vector<Mark> shownMarks_; // The marks as the image shown appeared, to log what the user changed.
    std::chrono::steady_clock::time_point shownAt_;
    // Per mark of a new image (learning only): how sure the model's kind classifier and the re-fit were of each kind.
    std::vector<std::vector<float>> shownOwn_;
    std::vector<std::vector<float>> shownLearned_;
    // Per mark of a new image (nudging only): the finder's box, before the nudge.
    std::vector<Box> shownGuessBoxes_;

    std::size_t index_ = 0;   // The image shown.
    std::vector<Mark> marks_; // Its marks.
    bool reviewed_ = false;   // Whether it is in answers_.
    std::unique_ptr<ScreenImage> image_;
    std::string imageError_; // Why the image could not be shown.
    std::string saveError_;  // Why the answers could not be saved.

    int selected_ = -1; // The mark whose answer box is open.
    int hovered_ = -1;
    // The mouse is over the image (or drawing on it): the marks near it fade.
    bool mouseOnImage_ = false;
    bool dragging_ = false; // The mouse is down on the image: a click (picks a mark) or a drag (draws one).
    int pressedMark_ = -1;  // The mark under the mouse when it went down.
    ImVec2 dragStart_;      // In image pixels.
    ImVec2 viewMin_;        // Where the image is shown on screen: the whole of it at 100%, a part zoomed in.
    ImVec2 viewMax_;
    ImVec2 imageMin_;    // Where the image's top left pixel is on screen (off the view when zoomed in),
    float scale_ = 1.0f; // and its size there relative to its pixels.

    ImageView view_;                // The zoom, kept from image to image while the review is open.
    float wheel_ = 0.0f;            // Wheel turns not zoomed by yet (a touchpad turns it in fractions).
    bool panning_ = false;          // A right or middle drag that started on the image is moving the view.
    double zoomChangedAt_ = -1e9;   // ImGui time of the last zoom change, for the label.
    int revealed_ = -1;             // The mark the view was last moved to show if needed,
    std::size_t revealedImage_ = 0; // on this image.
    bool clicked_ = false;          // The selection last changed by a click: the mark is in view, leave the view be.
};

} // namespace observex
