#include "core/ReviewHistory.h"

#include <utility>

namespace observex {

namespace {

std::optional<std::vector<Mark>> held(const ImageMarks& marks, const std::string& image) {
    const auto found = marks.find(image);
    return found != marks.end() ? std::optional<std::vector<Mark>>(found->second) : std::nullopt;
}

// Puts `kept` back as what `marks` holds for `image`. Returns whether that changed anything.
bool restore(ImageMarks& marks, const std::string& image, std::optional<std::vector<Mark>> kept) {
    if (held(marks, image) == kept) {
        return false;
    }
    if (kept) {
        marks[image] = std::move(*kept);
    } else {
        marks.erase(image);
    }
    return true;
}

} // namespace

void ReviewHistory::push(std::size_t index, const std::string& image, int selected, const ImageMarks& answers,
                         const ImageMarks& drafts) {
    steps_.push_back(Step{index, image, selected, held(answers, image), held(drafts, image)});
}

std::optional<ReviewHistory::Undone> ReviewHistory::undo(ImageMarks& answers, ImageMarks& drafts) {
    if (steps_.empty()) {
        return std::nullopt;
    }
    Step step = std::move(steps_.back());
    steps_.pop_back();
    Undone undone{step.index, step.selected};
    undone.answersChanged = restore(answers, step.image, std::move(step.answered));
    restore(drafts, step.image, std::move(step.draft));
    return undone;
}

} // namespace observex
