#pragma once

#include "core/Marks.h"

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace observex {

// What Ctrl+Z undoes in a review (the user asked, 2026-11-07): every answer, drawn or removed box, and move to another
// image (Left / Right; a Right that also makes the image reviewed and checks its kinds is one step), last first.
// Each step keeps what the image shown held just before it, so undoing puts that back and shows that image again:
// undoing past a move returns to the image before, and the next undo undoes its last edit. What an image shows on
// entering it (answers carried forward, sure guesses pre-answered) is worked out from what it holds, so it needs no
// step of its own. One history per opened review, gone when it closes. No redo.
class ReviewHistory {
public:
    // Where an undo leaves the review.
    struct Undone {
        std::size_t index = 0; // The image to show (its place in the recording),
        int selected = -1;     // the mark to select (the one the step changed, or the one selected then; -1: none),
        bool answersChanged = false; // and whether `answers` changed, so the answers file must be saved again.
    };

    // Call just before the user changes image `index` (file name `image`) or moves on from it: keeps what `answers`
    // (reviewed images) and `drafts` (answers on images not reviewed yet) hold for it, and the mark to select after
    // undoing.
    void push(std::size_t index, const std::string& image, int selected, const ImageMarks& answers,
              const ImageMarks& drafts);

    // Undoes the last step: puts what its image held back into `answers` and `drafts`. Nothing if there are no steps.
    std::optional<Undone> undo(ImageMarks& answers, ImageMarks& drafts);

    std::size_t size() const { return steps_.size(); }

private:
    struct Step {
        std::size_t index = 0;
        std::string image;
        int selected = -1;
        std::optional<std::vector<Mark>> answered; // What answers held for it; none if it was not reviewed.
        std::optional<std::vector<Mark>> draft;    // What drafts held for it.
    };
    std::vector<Step> steps_;
};

} // namespace observex
