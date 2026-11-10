#include "app/Review.h"

#include "app/Settings.h"
#include "app/Widgets.h"
#include "core/Detection.h"
#include "core/ModelInfo.h"
#include "core/Recordings.h"
#include "core/ReviewLines.h"
#include "engine/ImageFile.h"

#include <algorithm>
#include <cfloat>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <exception>
#include <fstream>
#include <iterator>
#include <optional>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>

namespace observex {

namespace {

constexpr ImGuiWindowFlags kAnswerBox = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                                        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_AlwaysAutoResize |
                                        ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav;

// While a mark is selected, the others fade to this opacity, so it is clear which one the Yes / No box asks about
// when characters stand close together (the user could not tell, 2026-11-01).
constexpr float kOtherMarks = 0.35f;

// Near the mouse, the marks it is not on fade, so what is under them shows (the user asked, 2026-11-08: small
// characters standing close together were hidden under each other's tags and boxes until one was picked). Fully
// within kNearMouse screen pixels (times the DPI scale) of the mouse, not at all from kFarFromMouse, smoothly between:
// tags to kTagNearMouse (all but gone), boxes to kBoxNearMouse (still there to aim at). The mark under the mouse and
// the selected one stay as they are.
constexpr float kNearMouse = 24.0f;
constexpr float kFarFromMouse = 96.0f;
constexpr float kTagNearMouse = 0.08f;
constexpr float kBoxNearMouse = 0.3f;

// How long the zoom label stays after zooming back out to 100%, so the user sees it is the whole image again.
constexpr double kZoomLabelSeconds = 1.0;

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

std::vector<std::string_view> kindIds(const Category& category) {
    std::vector<std::string_view> ids;
    for (const Kind& kind : category.kinds) {
        ids.push_back(kind.id);
    }
    return ids;
}

// Reviewed: in the answers file, with every kind on it checked and every mark answered.
bool isReviewed(const ImageMarks& answers, const std::string& image) {
    const auto found = answers.find(image);
    return found != answers.end() && fullyAnswered(found->second);
}

} // namespace

ReviewCount Review::countReviewed(const std::filesystem::path& folder, const Category& category) {
    const std::vector<std::string_view> ids = kindIds(category);
    ReviewCount count;
    for (const auto& [name, marks] : parseMarks(readTextFile(marksFile(folder, category, "answers")), ids)) {
        ++(fullyAnswered(marks) ? count.reviewed : count.toCheck);
    }
    return count;
}

Review::Review(ID3D11Device* device, std::filesystem::path folder, const Category& category)
    : device_(device), folder_(std::move(folder)), category_(category), kindIds_(kindIds(category)) {
    images_ = listImages(folder_);
    if (images_.empty()) {
        throw std::runtime_error("This folder holds no recorded images.");
    }
    loadGuesses();
    answers_ = parseMarks(readTextFile(marksFile(folder_, category_, "answers")), kindIds_);
    openedAnswers_ = formatMarks(answers_, {}, kindIds_);
    const std::filesystem::path model = modelFolder(category_.id);
    // The model's kind classifier, to re-fit to the answers: for a model of the category's kinds, in their order (or
    // its first ones: the answers of a kind it does not know yet teach it nothing). The kind update's if there is a
    // valid one (it learned from every recording's answers since the model was trained).
    const std::vector<std::string> modelKinds = parseModelKinds(readTextFile(model / L"model.json"));
    KindHead head = kindHeadToUse(model);
    if (kindsFit(modelKinds, kindIds_) && head.kinds() == static_cast<int>(modelKinds.size())) {
        learner_ = std::make_unique<KindLearner>(std::move(head));
    }
    if (answers_.size() < images_.size() && Finder::needed(folder_, category_.id, model)) {
        try {
            finder_ = std::make_unique<Finder>(folder_, images_, category_.id, model);
        } catch (const std::exception& e) {
            finderError_ = e.what();
        }
    }

    // The log: which recording, model and classifier this review works with.
    log_ = std::make_unique<ReviewLog>(marksFile(folder_, category_, "review-log"));
    openedAt_ = std::chrono::steady_clock::now();
    reviewedAtOpen_ = reviewedCount();
    {
        const std::size_t toCheck = answers_.size() - reviewedAtOpen_;
        const KindUpdate update = loadKindUpdate(model);
        std::string text =
            "open review of " + toUtf8(folder_.filename()) + " for " + category_.id + ": " +
            std::to_string(images_.size()) + " images, " + std::to_string(reviewedAtOpen_) + " reviewed" +
            (toCheck > 0 ? ", " + std::to_string(toCheck) + " to check (kinds, or a guess added since)" : "") +
            "\nmodel " +
            toUtf8(model / L"model.onnx") + " of " + ReviewLog::fileTime(model / L"model.onnx") + "\nkinds from " +
            (update.head.kinds() > 0 ? "the kind update (" + std::to_string(update.answers) + " answers from " +
                                           std::to_string(update.recordings) + " recordings, " +
                                           ReviewLog::fileTime(model / kKindUpdateFile) + ")"
                                     : std::string("the model's own classifier")) +
            (learner_ ? "; re-fit to the answers as the user reviews"
                      : "; no re-fit (the model has no kind classifier)");
        if (finder_) {
            text += "\nthe finder is making this recording's guesses with this model";
        } else if (!finderError_.empty()) {
            text += "\nthe finder could not start: " + finderError_;
        }
        log_->write(text);
    }

    // The first image not reviewed: not answered, or answered before kinds and not checked since.
    std::size_t start = 0;
    while (start + 1 < images_.size() && isReviewed(answers_, images_[start])) {
        ++start;
    }
    show(start);
    logImage("opened");
}

Review::~Review() {
    // Changed: the answers file differs from when the review opened (so undoing back to it is no change, and answers
    // on an image never moved on from, which are not saved, are none either).
    try {
        if (log_) {
            const auto seconds =
                std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - openedAt_).count();
            char took[48];
            std::snprintf(took, sizeof(took), "%lld min %lld s", static_cast<long long>(seconds / 60),
                          static_cast<long long>(seconds % 60));
            log_->write("close after " + std::string(took) + ": " + std::to_string(reviewedCount()) + " of " +
                        std::to_string(images_.size()) + " images reviewed (" + std::to_string(reviewedAtOpen_) +
                        " when it opened)");
        }
        if (formatMarks(answers_, {}, kindIds_) == openedAnswers_) {
            return;
        }
        SYSTEMTIME now;
        GetLocalTime(&now);
        char time[24];
        std::snprintf(time, sizeof(time), "%04u-%02u-%02u %02u:%02u:%02u", now.wYear, now.wMonth, now.wDay, now.wHour,
                      now.wMinute, now.wSecond);
        writeReviewed(folder_, category_.id, time);
    } catch (const std::exception&) {
        // Out of memory while closing: the list just shows the time before.
    }
}

std::size_t Review::reviewedCount() const {
    return static_cast<std::size_t>(
        std::count_if(answers_.begin(), answers_.end(), [](const auto& image) { return fullyAnswered(image.second); }));
}

std::size_t Review::toCheckCount() const {
    return answers_.size() - reviewedCount();
}

void Review::update() {
    if (!finder_) {
        return;
    }
    switch (finder_->state()) {
    case Finder::State::Running:
        return;
    case Finder::State::Done:
        loadGuesses();
        if (log_) {
            log_->write("guesses ready: the finder looked at this recording");
        }
        // The image shown gets the guesses too, unless the user already started on it.
        if (!reviewed_ && !drafts_.contains(images_[index_]) && !dragging_) {
            loadMarks();
            logImage("guesses ready");
        }
        break;
    case Finder::State::Failed:
        finderError_ = finder_->error();
        if (log_) {
            log_->write("finder failed: " + finderError_);
        }
        break;
    }
    finder_.reset();
}

void Review::previous() {
    if (canGoBack()) {
        remember(selected_);
        logLeave("back");
        show(index_ - 1);
        logImage("back");
    }
}

int Review::unansweredCount() const {
    return static_cast<int>(
        std::count_if(marks_.begin(), marks_.end(), [](const Mark& mark) { return mark.answer == Answer::None; }));
}

void Review::next() {
    moveOn(index_ + 1, "next");
}

void Review::nextToCheck() {
    // The first image after this one the user is not done with, else the first before it.
    std::size_t to = images_.size();
    for (std::size_t step = 1; step < images_.size() && to == images_.size(); ++step) {
        const std::size_t image = (index_ + step) % images_.size();
        if (!isReviewed(answers_, images_[image])) {
            to = image;
        }
    }
    if (to == images_.size() && log_) {
        log_->write("Shift+Right on " + images_[index_] + ": no other image to check");
    }
    moveOn(to, "Shift+Right, next to check");
}

void Review::moveOn(std::size_t to, const char* why) {
    if (unansweredCount() > 0) {
        selected_ = nextUnanswered(-1); // Shows its Yes / No: answer it first.
        if (log_) {
            log_->write("next blocked on " + images_[index_] + ": " + std::to_string(unansweredCount()) +
                        " marks unanswered, selected mark " + std::to_string(selected_));
        }
        return;
    }
    if (to >= images_.size() && !canGoOn()) {
        return;
    }
    remember(selected_); // One step: making the image reviewed, its kinds checked, and moving on.
    // Moving on says everything here is right, the suggested kinds too.
    for (Mark& mark : marks_) {
        mark.unchecked = false;
    }
    reviewed_ = true;
    drafts_.erase(images_[index_]);
    changed();
    logLeave("done");
    if (to < images_.size()) {
        show(to);
        logImage(why);
    }
}

void Review::show(std::size_t index) {
    index_ = index;
    dragging_ = false;
    pressedMark_ = -1;
    hovered_ = -1;
    loadImage(); // First: the nudged boxes are kept inside it.
    loadMarks();
}

void Review::loadMarks() {
    const std::string& name = images_[index_];
    fitText_.clear();
    shownOwn_.clear();
    shownLearned_.clear();
    shownGuessBoxes_.clear();
    reviewed_ = answers_.contains(name);
    if (reviewed_) {
        marks_ = answers_.at(name);
        // Answered before kinds: suggest the kinds the user checked on the image before for the same things.
        if (!kindsChecked(marks_) && index_ > 0) {
            if (const auto before = answers_.find(images_[index_ - 1]); before != answers_.end()) {
                carryKinds(before->second, marks_);
            }
        }
    } else if (const auto draft = drafts_.find(name); draft != drafts_.end()) {
        marks_ = draft->second;
    } else {
        const auto guessed = guesses_.find(name);
        marks_ = guessed != guesses_.end() ? guessed->second : std::vector<Mark>{};
        if (learning() || nudging()) {
            learn();
        }
        // Each guess's box nudged the way the user boxed the guesses so far (core/BoxLearner), before the answers of
        // the image before are carried onto them (by overlap with the user's boxes there).
        if (nudging()) {
            const Box image = image_ ? Box{0, 0, static_cast<int>(image_->width()), static_cast<int>(image_->height())}
                                     : Box{};
            for (std::size_t i = 0; i < marks_.size(); ++i) {
                shownGuessBoxes_.push_back(marks_[i].box);
                marks_[i].box = nudged(marks_[i].box, boxLearner_.nudge(features_.of(name, i)), image);
            }
        }
        // The kind each guess would start as yes of. Learning: each guess shows the kinds the re-fit to the answers
        // so far sees, and gets that kind from both the model and the re-fit.
        std::vector<int> presumed;
        if (learning()) {
            std::vector<float> own(static_cast<std::size_t>(learner_->kinds()));
            std::vector<float> learned(own.size());
            for (std::size_t i = 0; i < marks_.size(); ++i) {
                learner_->sure(features_.of(name, i), own, learned);
                showKinds(marks_[i], learned);
                presumed.push_back(presumedKind(own, learned));
                shownOwn_.push_back(own);
                shownLearned_.push_back(learned);
            }
        } else {
            for (const Mark& mark : marks_) {
                presumed.push_back(presumedKind(mark));
            }
        }
        // The same things are usually still there, close to where they were: reuse the answers of the image
        // before, if it is reviewed (but no "no" on a guess the review is sure of). Then the sure ones start as yes.
        if (index_ > 0) {
            if (const auto before = answers_.find(images_[index_ - 1]); before != answers_.end()) {
                carryAnswers(before->second, marks_, presumed);
            }
        }
        presumeKinds(marks_, presumed);
    }
    selected_ = nextUnanswered(-1);
}

void Review::loadGuesses() {
    const std::filesystem::path file = marksFile(folder_, category_, "guesses");
    std::error_code error;
    hasGuesses_ = std::filesystem::exists(file, error);
    guesses_ = parseMarks(readTextFile(file), kindIds_);

    // Their features, if written after them (by the same finder run) and after the model (not by an older one).
    features_ = freshFeatures(folder_, category_.id, modelFolder(category_.id), guesses_);
    relearn_ = true;
}

bool Review::learning() const {
    return learner_ && features_.size == learner_->features();
}

bool Review::nudging() const {
    return features_.size > 0;
}

void Review::learn() {
    if (!relearn_ && boxFitFor_ == index_) {
        return;
    }
    // Every guess on a reviewed image, with the user's answer, matched by overlap as the kind update does: answers
    // given to an older model's guesses still count. Drawn boxes have no features.
    if (relearn_ && learning()) {
        std::vector<float> features;
        std::vector<int> answers;
        addAnsweredGuesses(guesses_, features_, answers_, learner_->kinds(), features, answers);
        const auto start = std::chrono::steady_clock::now();
        learner_->fit(features, answers);
        fitText_ = describeFit(
            *learner_, answers, kindIds_,
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());
    }
    relearn_ = false;
    // The boxes, from the same guesses, of those matching a yes answer: fitted for the image shown, as the images
    // nearer to it count more.
    if (nudging()) {
        BoxSamples samples;
        addAnsweredBoxes(guesses_, features_, answers_, images_, samples);
        const auto start = std::chrono::steady_clock::now();
        boxLearner_.fit(samples, static_cast<int>(index_));
        boxFitText_ = describeBoxFit(
            boxLearner_, std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());
        boxFitFor_ = index_;
    }
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
        relearn_ = true;
        save();
    } else {
        drafts_[name] = marks_;
    }
}

void Review::save() {
    std::string kinds;
    for (const std::string_view id : kindIds_) {
        kinds += kinds.empty() ? "" : ", ";
        kinds += id;
    }
    char comment[800];
    if (kinds.empty()) {
        std::snprintf(comment, sizeof(comment),
                      "observex review answers, category: %s.\n"
                      "Each \"image\" line is an image the user reviewed; its marks follow, one per line:\n"
                      "yes or no (whether it is in the category), then x, y, width and height in image pixels.\n"
                      "On a reviewed image, everything in the category is marked yes.",
                      category_.id);
    } else {
        std::snprintf(comment, sizeof(comment),
                      "observex review answers, category: %s.\n"
                      "Each \"image\" line is an image the user reviewed; its marks follow, one per line:\n"
                      "its kind (%s) or no (not in the category), then x, y, width and height in image pixels.\n"
                      "\"yes\" is in the category, answered before kinds: its kind (the first, or the word after\n"
                      "yes) is a suggestion the user has not checked yet.\n"
                      "On a reviewed image, everything in the category is marked.",
                      category_.id, kinds.c_str());
    }
    const std::filesystem::path file = marksFile(folder_, category_, "answers");
    std::filesystem::path temporary = file;
    temporary += L".tmp";
    {
        std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
        out << formatMarks(answers_, comment, kindIds_);
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

void Review::answer(Answer answer, int kind, const char* how) {
    if (selected_ < 0) {
        selected_ = nextUnanswered(-1);
        // Zoomed in on other marks: shown first (the view moves to it), not answered unseen.
        if (selected_ >= 0 && !inView(marks_[static_cast<std::size_t>(selected_)].box)) {
            return;
        }
    }
    if (selected_ < 0) {
        return;
    }
    remember(selected_);
    Mark& mark = marks_[static_cast<std::size_t>(selected_)];
    const Mark before = mark;
    mark.answer = answer;
    if (answer == Answer::Yes && kind >= 0 && static_cast<std::size_t>(kind) < category_.kinds.size()) {
        mark.kind = static_cast<std::uint8_t>(kind);
    }
    mark.carried = false;
    mark.presumed = false;
    mark.unchecked = false; // The user answered it, kind and all.
    changed();
    if (log_) {
        std::string text = "answer on " + images_[index_] + " mark " + std::to_string(selected_) + " " +
                           describeWhere(mark) + ": " + describeMark(before, kindIds_) + " -> " +
                           describeMark(mark, kindIds_) + " (" + how + ", " + sinceShown() +
                           " after the image appeared" + zoomText() + ")";
        for (const std::string& line : describeOverlaps(marks_, static_cast<std::size_t>(selected_), kindIds_)) {
            text += "\noverlaps " + line;
        }
        log_->write(text);
    }
    if (const int following = nextUnanswered(selected_); following >= 0) {
        selected_ = following;
    } else {
        // All answered: nothing stays picked, so every mark shows at full strength and no buttons cover the image
        // (the user clicked beside the marks for that after 131 of 143 last answers on 2026-11-08).
        select(-1, "all answered");
    }
}

void Review::removeSelected() {
    // Only marks the user drew: the finder's guesses are answered no instead, which teaches it something.
    if (selected_ < 0 || marks_[static_cast<std::size_t>(selected_)].guess >= 0.0f) {
        return;
    }
    remember(selected_);
    if (log_) {
        const Mark& removed = marks_[static_cast<std::size_t>(selected_)];
        log_->write("remove on " + images_[index_] + " mark " + std::to_string(selected_) + " " +
                    describeWhere(removed) + ": was " + describeMark(removed, kindIds_) + " (" + sinceShown() +
                    " after the image appeared)");
    }
    marks_.erase(marks_.begin() + selected_);
    selected_ = -1;
    hovered_ = -1;
    changed();
}

void Review::select(int mark, const char* how) {
    if (mark == selected_) {
        return;
    }
    selected_ = mark;
    if (!log_) {
        return;
    }
    if (mark < 0) {
        log_->write("select none on " + images_[index_] + " (" + how + ")");
        return;
    }
    const Mark& picked = marks_[static_cast<std::size_t>(mark)];
    log_->write("select on " + images_[index_] + " mark " + std::to_string(mark) + " " + describeWhere(picked) + ": " +
                describeMark(picked, kindIds_) + " (" + how + ", " + sinceShown() + " after the image appeared)");
}

void Review::logImage(const char* why) {
    shownMarks_ = marks_;
    shownAt_ = std::chrono::steady_clock::now();
    if (!log_) {
        return;
    }
    const std::string& name = images_[index_];
    const bool fresh = !reviewed_ && !drafts_.contains(name); // As the finder guessed, pre-filled by the review.
    std::string state;
    if (reviewed_) {
        state = kindsToCheck()            ? "answered before kinds, its kinds to check"
                : unansweredCount() > 0 ? "reviewed before, with a guess added since to answer"
                                        : "reviewed before, showing its answers";
    } else if (!fresh) {
        state = "answered in part before (not moved on), showing those answers";
    } else if (guesses_.contains(name)) {
        state = "new, the finder's guesses";
    } else {
        state = finder_ ? "new, the finder is still looking" : "new, no guesses";
    }
    std::string text = "image " + name + " (" + std::to_string(index_ + 1) + " of " + std::to_string(images_.size()) +
                       ", " + why + "): " + state + "; " + std::to_string(marks_.size()) +
                       " marks: " + describeCounts(marks_, kindIds_);
    if (fresh && learning()) {
        text +=
            "\nre-fit " + (fitText_.empty() ? std::string("as for the image before (no new answers since)") : fitText_);
    }
    if (fresh && nudging()) {
        text += "\nbox nudge " + boxFitText_;
    }
    for (std::size_t i = 0; i < marks_.size(); ++i) {
        const Mark& mark = marks_[i];
        text += "\nmark " + std::to_string(i) + " " + describeWhere(mark);
        if (fresh && i < shownGuessBoxes_.size() && shownGuessBoxes_[i] != mark.box) {
            text += ", nudged from " + describeBox(shownGuessBoxes_[i]);
        }
        text += ": ";
        std::span<const float> own;
        std::span<const float> learned;
        std::vector<float> model; // Without learning: the model's two surest kinds, as the guesses file has them.
        if (fresh && i < shownOwn_.size()) {
            own = shownOwn_[i];
            learned = shownLearned_[i];
            text += "model " + describeSure(own, kindIds_) + " | re-fit " + describeSure(learned, kindIds_) + " -> ";
        } else if (fresh && mark.guess >= 0.0f && mark.secondGuess >= 0.0f && mark.kind < kindIds_.size() &&
                   mark.secondKind < kindIds_.size()) {
            model.assign(kindIds_.size(), 0.0f);
            model[mark.kind] = kindGuessOf(mark);
            model[mark.secondKind] = mark.secondGuess;
            text += "model " + describeSure(model, kindIds_) + " -> ";
        }
        text += describeMark(mark, kindIds_);
        if (fresh && (mark.presumed || mark.answer == Answer::None)) {
            text += ": " + describePrefill(mark, own, learned);
        }
    }
    log_->write(text);
}

void Review::logLeave(const char* how) {
    if (!log_) {
        return;
    }
    std::string text = std::string(how) + " " + images_[index_] + " after " + sinceShown() + ": " +
                       std::to_string(marks_.size()) + " marks, " + describeCounts(marks_, kindIds_);
    if (!reviewed_) {
        text += drafts_.contains(images_[index_]) ? " (not moved on: its answers so far are kept)"
                                                  : " (not moved on, nothing answered)";
    }
    for (const std::string& line : describeChanges(shownMarks_, marks_, kindIds_)) {
        text += "\n" + line;
    }
    log_->write(text);
}

std::string Review::sinceShown() const {
    char text[32];
    std::snprintf(text, sizeof(text), "%.1f s",
                  std::chrono::duration<double>(std::chrono::steady_clock::now() - shownAt_).count());
    return text;
}

void Review::remember(int selected) {
    history_.push(index_, images_[index_], selected, answers_, drafts_);
}

void Review::undo() {
    const std::size_t indexBefore = index_;
    const std::vector<Mark> marksBefore = marks_;
    const std::optional<ReviewHistory::Undone> undone = history_.undo(answers_, drafts_);
    if (!undone) {
        if (log_) {
            log_->write("undo on " + images_[index_] + ": nothing left to undo");
        }
        return;
    }
    if (undone->answersChanged) {
        save(); // So the file holds what is shown again.
        relearn_ = true;
    }
    if (undone->index != index_) {
        show(undone->index);
    } else {
        hovered_ = -1;
        loadMarks();
    }
    selected_ = undone->selected < static_cast<int>(marks_.size()) ? undone->selected : -1;
    if (index_ != indexBefore) {
        if (log_) {
            log_->write("undo on " + images_[indexBefore] + ": back to " + images_[index_]);
        }
        logImage("after Ctrl+Z");
    } else if (log_) {
        std::string text = "undo on " + images_[index_] + " (the lines below are what the undo did)";
        for (const std::string& line : describeChanges(marksBefore, marks_, kindIds_)) {
            text += "\n" + line;
        }
        log_->write(text);
    }
}

ImU32 Review::markColor(const Mark& mark) const {
    switch (mark.answer) {
    case Answer::Yes:
        return mark.kind < category_.kinds.size() ? category_.kinds[mark.kind].color : theme::kYes;
    case Answer::No:
        return theme::kTextMuted;
    case Answer::None:
        break;
    }
    return theme::kAccent;
}

int Review::nextUnanswered(int after) const {
    // Zoomed in, those in view first: answering a crowd up close does not jump away from it.
    const int count = static_cast<int>(marks_.size());
    for (const bool anywhere : {false, true}) {
        for (int i = 1; i <= count; ++i) {
            const int candidate = ((after < 0 ? -1 : after) + i) % count;
            const Mark& mark = marks_[static_cast<std::size_t>(candidate)];
            if (mark.answer == Answer::None && (anywhere || inView(mark.box))) {
                return candidate;
            }
        }
    }
    return -1;
}

bool Review::inView(const Box& box) const {
    if (!image_ || view_.zoom <= 1.0f) {
        return true;
    }
    const float x = static_cast<float>(box.x) + static_cast<float>(box.width) * 0.5f;
    const float y = static_cast<float>(box.y) + static_cast<float>(box.height) * 0.5f;
    return x >= view_.left && x <= view_.left + static_cast<float>(image_->width()) / view_.zoom && y >= view_.top &&
           y <= view_.top + static_cast<float>(image_->height()) / view_.zoom;
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
    const float x = (std::clamp(screen.x, viewMin_.x, viewMax_.x) - imageMin_.x) / scale_;
    const float y = (std::clamp(screen.y, viewMin_.y, viewMax_.y) - imageMin_.y) / scale_;
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

bool Review::drawing() const {
    if (!dragging_) {
        return false;
    }
    // On screen, so a click that wobbles a few pixels is still a click (and a far character's box can be small).
    const float smallest = 6.0f * ImGui::GetStyle().FontScaleDpi;
    const Box box = dragBox();
    return static_cast<float>(box.width) * scale_ >= smallest && static_cast<float>(box.height) * scale_ >= smallest;
}

void Review::handleKeys() {
    if (ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId) || ImGui::GetIO().WantTextInput) {
        return;
    }
    const ImGuiIO& io = ImGui::GetIO();
    if (io.KeyCtrl) {
        // Ctrl+Z undoes (held down, it repeats); not while drawing a box, which is no step yet. The other keys do
        // nothing with Ctrl, so Ctrl+Y (redo elsewhere) does not answer yes.
        if (io.KeyMods == ImGuiMod_Ctrl && ImGui::IsKeyPressed(ImGuiKey_Z) && !dragging_) {
            undo();
        }
        return;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Y, false)) {
        answer(Answer::Yes, -1, "key Y"); // Yes, of the kind it shows.
    }
    if (ImGui::IsKeyPressed(ImGuiKey_N, false)) {
        answer(Answer::No, -1, "key N");
    }
    // 1, 2, ...: yes, of that kind (the digits on top and on the number pad).
    static const char* const kKeys[] = {"key 1", "key 2", "key 3", "key 4", "key 5",
                                        "key 6", "key 7", "key 8", "key 9"};
    for (std::size_t i = 0; i < category_.kinds.size() && i < std::size(kKeys); ++i) {
        const int offset = static_cast<int>(i) + 1;
        if (ImGui::IsKeyPressed(static_cast<ImGuiKey>(ImGuiKey_0 + offset), false) ||
            ImGui::IsKeyPressed(static_cast<ImGuiKey>(ImGuiKey_Keypad0 + offset), false)) {
            answer(Answer::Yes, static_cast<int>(i), kKeys[i]);
        }
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Delete, false) || ImGui::IsKeyPressed(ImGuiKey_Backspace, false)) {
        removeSelected();
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Tab, false) && !marks_.empty()) {
        const int following = nextUnanswered(selected_);
        select(following >= 0 ? following : (selected_ + 1) % static_cast<int>(marks_.size()), "Tab");
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
        select(-1, "Esc");
    }
    // 0: the whole image again (100%).
    if ((ImGui::IsKeyPressed(ImGuiKey_0, false) || ImGui::IsKeyPressed(ImGuiKey_Keypad0, false)) && view_.zoom > 1.0f) {
        view_ = ImageView{};
        zoomChangedAt_ = ImGui::GetTime();
    }
    if (ImGui::IsKeyPressed(ImGuiKey_RightArrow) || ImGui::IsKeyPressed(ImGuiKey_D)) {
        if (io.KeyShift) {
            nextToCheck();
        } else {
            next();
        }
    } else if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow) || ImGui::IsKeyPressed(ImGuiKey_A)) {
        previous();
    }
}

void Review::placeImage() {
    scale_ = (viewMax_.x - viewMin_.x) / static_cast<float>(image_->width()) * view_.zoom;
    imageMin_ = ImVec2(viewMin_.x - view_.left * scale_, viewMin_.y - view_.top * scale_);
}

void Review::handleZoom() {
    const ImGuiIO& io = ImGui::GetIO();
    const auto width = static_cast<float>(image_->width());
    const auto height = static_cast<float>(image_->height());
    // The wheel over the image is the zoom's, not the window's.
    ImGui::SetItemKeyOwner(ImGuiKey_MouseWheelY);
    if ((ImGui::IsItemHovered() || dragging_) && io.MouseWheel != 0.0f) {
        wheel_ += io.MouseWheel;
        const int steps = static_cast<int>(wheel_); // Whole turns; a touchpad's fractions add up.
        wheel_ -= static_cast<float>(steps);
        if (const float zoom = steppedZoom(view_.zoom, steps); zoom != view_.zoom) {
            const ImVec2 pixel = toImage(io.MousePos);
            view_ = zoomedAt(view_, zoom, pixel.x, pixel.y, width, height);
            zoomChangedAt_ = ImGui::GetTime();
        }
    }
    // A right or middle drag that starts on the image moves the view with the mouse.
    if (ImGui::IsItemHovered() &&
        (ImGui::IsMouseClicked(ImGuiMouseButton_Right) || ImGui::IsMouseClicked(ImGuiMouseButton_Middle))) {
        panning_ = true;
    }
    if (panning_ && !ImGui::IsMouseDown(ImGuiMouseButton_Right) && !ImGui::IsMouseDown(ImGuiMouseButton_Middle)) {
        panning_ = false;
    }
    if (panning_ && (io.MouseDelta.x != 0.0f || io.MouseDelta.y != 0.0f)) {
        view_ = keptInside(
            ImageView{view_.zoom, view_.left - io.MouseDelta.x / scale_, view_.top - io.MouseDelta.y / scale_}, width,
            height);
    }
    // The review picked a mark (Tab, after an answer, Next, Ctrl+Z, a new image): if it is not all in view, the view
    // moves to it. Not after a click: the user sees what they click.
    if (selected_ != revealed_ || index_ != revealedImage_) {
        if (selected_ >= 0 && !clicked_) {
            view_ = showing(view_, marks_[static_cast<std::size_t>(selected_)].box, width, height);
        }
        revealed_ = selected_;
        revealedImage_ = index_;
    }
    clicked_ = false;
    placeImage();
}

void Review::handleMouse() {
    // The image is one invisible button. A click picks the mark under it (the smallest there), or none; a drag draws
    // a new mark wherever it starts, so a missed character can be drawn over a bigger mark.
    const ImVec2 pixel = toImage(ImGui::GetMousePos());
    mouseOnImage_ = ImGui::IsItemHovered() || dragging_;
    hovered_ = ImGui::IsItemHovered() && !drawing() ? markAt(pixel) : -1;
    if (ImGui::IsItemActivated()) {
        dragging_ = true;
        dragStart_ = pixel;
        pressedMark_ = hovered_;
    }
    if (dragging_ && ImGui::IsItemDeactivated()) {
        if (drawing()) {
            remember(selected_);
            marks_.push_back(Mark{dragBox(), Answer::Yes});
            selected_ = static_cast<int>(marks_.size()) - 1;
            changed();
            if (log_) {
                const Mark& drawn = marks_.back();
                std::string text =
                    "draw on " + images_[index_] + " mark " + std::to_string(selected_) + " " + describeWhere(drawn) +
                    ": new box, a yes" +
                    (kindIds_.empty() ? std::string()
                                      : ", " + std::string(kindIds_[drawn.kind]) + " until you pick a kind") +
                    " (" + sinceShown() + " after the image appeared" + zoomText() + ")";
                for (const std::string& line : describeOverlaps(marks_, marks_.size() - 1, kindIds_)) {
                    text += "\noverlaps " + line;
                }
                log_->write(text);
            }
        } else {
            clicked_ = true;
            select(pressedMark_, pressedMark_ >= 0 ? "click" : "click on no mark");
        }
        dragging_ = false;
        pressedMark_ = -1;
    }
    if (panning_) {
        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);
    } else if (hovered_ >= 0) {
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
        // The view as large as fits, keeping the image's shape; the image in it, or the part zoomed in on.
        const auto width = static_cast<float>(image_->width());
        const auto height = static_cast<float>(image_->height());
        const float aspect = width / height;
        const ImVec2 size =
            area.x / area.y > aspect ? ImVec2(area.y * aspect, area.y) : ImVec2(area.x, area.x / aspect);
        viewMin_ =
            ImVec2(std::round(origin.x + (area.x - size.x) * 0.5f), std::round(origin.y + (area.y - size.y) * 0.5f));
        viewMax_ = ImVec2(viewMin_.x + std::round(size.x), viewMin_.y + std::round(size.y));
        view_ = keptInside(view_, width, height);
        placeImage();

        ImGui::SetCursorScreenPos(viewMin_);
        ImGui::InvisibleButton("##image", ImVec2(viewMax_.x - viewMin_.x, viewMax_.y - viewMin_.y));
        handleZoom();
        handleMouse();
        const float rounding = 6.0f * dpi;
        const float shown = 1.0f / view_.zoom;
        image_->draw(drawList, viewMin_, viewMax_, rounding, theme::kBackground,
                     ImVec2(view_.left / width, view_.top / height),
                     ImVec2(view_.left / width + shown, view_.top / height + shown));
        drawList->PushClipRect(viewMin_, viewMax_, true);
        const theme::Area selectedTag = drawMarks(drawList, fonts);
        drawList->PopClipRect();
        drawZoom(drawList);

        // A note at the top: the finder at work, why it failed, or that nothing is marked.
        char text[160] = {};
        char detail[200] = {};
        if (finder_) {
            std::snprintf(text, sizeof(text), "Looking for %s with the trained model", category_.nouns);
            const float preparing = finder_->preparing();
            if (finder_->done() > 0) {
                std::snprintf(detail, sizeof(detail), "Image %d of %d. Its marks show here when it is done.",
                              finder_->done(), finder_->total());
            } else if (preparing > 0.0f && preparing < 1.0f) {
                std::snprintf(detail, sizeof(detail), "Preparing the model for this GPU: %.0f%% (once per new model)",
                              static_cast<double>(preparing * 100.0f));
            } else {
                std::snprintf(detail, sizeof(detail), "Loading the model\xE2\x80\xA6");
            }
        } else if (!finderError_.empty()) {
            std::snprintf(text, sizeof(text), "The trained model could not look at this recording.");
            std::snprintf(detail, sizeof(detail), "%.190s", finderError_.c_str());
        } else if (!hasGuesses_ && marks_.empty()) {
            std::snprintf(text, sizeof(text), "Nothing is marked: the finder has not looked at this recording yet.");
            std::snprintf(detail, sizeof(detail), "Drag a box around each %s it should learn.", category_.noun);
        }
        if (text[0] != '\0' && !drawing()) {
            theme::drawLabel(drawList, ImVec2((viewMin_.x + viewMax_.x) * 0.5f, viewMin_.y + 12.0f * dpi),
                             ImVec2(0.5f, 0.0f), text, detail);
        }
        drawAnswerBox(fonts, origin, viewMax, selectedTag);
    }
    drawStatus(drawList, ImVec2(origin.x, viewMax.y), ImVec2(viewMax.x, origin.y + available.y));
}

theme::Area Review::drawMarks(ImDrawList* drawList, const theme::Fonts& fonts) const {
    const float dpi = ImGui::GetStyle().FontScaleDpi;

    // The selected mark: a thin outline just outside the box (its halo included, so it covers none of the box) and a
    // thin white ring outside that, like keyboard focus. Inside the ring only the image shows (the user asked,
    // 2026-11-07: small characters were hard to see under thick borders and the other marks' boxes and tags).
    const float selectedThickness = 1.5f * dpi;
    const float ring = 1.5f * dpi;
    const float outlineOffset = std::round(selectedThickness * 0.5f + 1.0f * dpi);
    const float ringOffset = std::round(outlineOffset + selectedThickness * 0.5f + ring * 0.5f + 1.0f * dpi);

    // How much of a box or tag from `min` to `max` shows with the mouse where it is: `least` next to the mouse, all of
    // it far from it (kNearMouse, kFarFromMouse).
    const ImVec2 mouse = ImGui::GetMousePos();
    const auto nearMouse = [&](ImVec2 min, ImVec2 max, float least) {
        if (!mouseOnImage_) {
            return 1.0f;
        }
        const float dx = std::max({min.x - mouse.x, 0.0f, mouse.x - max.x});
        const float dy = std::max({min.y - mouse.y, 0.0f, mouse.y - max.y});
        const float away = std::clamp(
            (std::sqrt(dx * dx + dy * dy) - kNearMouse * dpi) / ((kFarFromMouse - kNearMouse) * dpi), 0.0f, 1.0f);
        return least + (1.0f - least) * away * away * (3.0f - 2.0f * away); // Smoothly.
    };

    // Draws mark `i`; returns where its tag is (empty if it has none).
    const auto drawMark = [&](std::size_t i) -> theme::Area {
        const int index = static_cast<int>(i);
        const bool selected = index == selected_;
        const Mark& mark = marks_[i];
        const bool lit = selected || index == hovered_;
        const float opacity = selected_ >= 0 && !lit ? kOtherMarks : 1.0f;
        const ImVec2 min = toScreen(mark.box.x, mark.box.y);
        const ImVec2 max = toScreen(mark.box.x + mark.box.width, mark.box.y + mark.box.height);
        // Zoomed in, a mark all out of view is not drawn: its tag would be kept inside the view, without its box.
        if (max.x <= viewMin_.x || min.x >= viewMax_.x || max.y <= viewMin_.y || min.y >= viewMax_.y) {
            return {};
        }
        const ImU32 color = theme::faded(markColor(mark), opacity * (lit ? 1.0f : nearMouse(min, max, kBoxNearMouse)));
        const float thickness =
            selected ? selectedThickness : (lit ? 2.5f : mark.answer == Answer::No ? 1.0f : 1.5f) * dpi;
        const float out = selected ? outlineOffset : 0.0f;
        theme::drawBox(drawList, ImVec2(min.x - out, min.y - out), ImVec2(max.x + out, max.y + out), color, thickness);
        // The selected one's tag sits on its ring.
        ImVec2 tagBoxMin = min;
        ImVec2 tagBoxMax = max;
        float tagBoxThickness = thickness;
        if (selected) {
            tagBoxMin = ImVec2(min.x - ringOffset, min.y - ringOffset);
            tagBoxMax = ImVec2(max.x + ringOffset, max.y + ringOffset);
            tagBoxThickness = ring;
            theme::drawBox(drawList, tagBoxMin, tagBoxMax, theme::kText, ring);
        }

        // A tag above the box (below it at the image's top edge) says what it is. Marks answered no only get one
        // while pointed at.
        if (mark.answer == Answer::No && !lit) {
            return {};
        }
        // A yes is named by its kind ("Monster"), if the category has kinds.
        const char* yes = mark.kind < category_.kinds.size() ? category_.kinds[mark.kind].name : category_.yes;
        char text[96];
        if (mark.answer == Answer::None) {
            const bool guessedKinds = mark.secondGuess >= 0.0f && mark.kind < category_.kinds.size() &&
                                      mark.secondKind < category_.kinds.size();
            if (guessedKinds && kindsClose(kindGuessOf(mark), mark.secondGuess)) {
                // Unsure which kind it is: both, so the user sees what to choose between.
                std::snprintf(text, sizeof(text), "%s %.0f%% or %s %.0f%%?", yes,
                              static_cast<double>(kindGuessOf(mark) * 100.0f), category_.kinds[mark.secondKind].name,
                              static_cast<double>(mark.secondGuess * 100.0f));
            } else if (mark.guess >= 0.0f) {
                // With a kind, how sure of that kind ("Monster? 70%"), the number the review pre-answers by.
                std::snprintf(text, sizeof(text), "%s? %.0f%%", guessedKinds ? yes : category_.yes,
                              static_cast<double>((guessedKinds ? kindGuessOf(mark) : mark.guess) * 100.0f));
            } else {
                std::snprintf(text, sizeof(text), "%s?", category_.yes);
            }
        } else if (mark.presumed) {
            // Yes because the finder was sure of its kind: its sureness says the user did not answer it.
            std::snprintf(text, sizeof(text), "%s %.0f%%", yes, static_cast<double>(kindGuessOf(mark) * 100.0f));
        } else {
            std::snprintf(text, sizeof(text), "%s%s", mark.answer == Answer::Yes ? yes : category_.no,
                          mark.carried ? "  \xC2\xB7  copied" : "");
        }
        const bool dim = mark.answer == Answer::No;
        const theme::Area tag =
            theme::placeTag(fonts.medium, tagBoxMin, tagBoxMax, tagBoxThickness, viewMin_, viewMax_, text);
        const float tagOpacity = opacity * (lit ? 1.0f : nearMouse(tag.min, tag.max, kTagNearMouse));
        theme::drawTagAt(drawList, fonts.medium, tag, text,
                         theme::faded(dim ? theme::kSurfaceActive : markColor(mark), tagOpacity),
                         theme::faded(dim ? theme::kText : theme::kBackground, tagOpacity));
        return tag;
    };

    theme::Area selectedTag;
    if (selected_ < 0) {
        for (std::size_t i = 0; i < marks_.size(); ++i) {
            drawMark(i);
        }
    } else {
        // The other marks are drawn only around the selected one's ring, never inside it: in four clip bands (above,
        // below, left, right) that do not overlap, so nothing is drawn twice. The mark under the mouse is the
        // exception, as a click picks it. The selected one goes last, on top.
        const Box& box = marks_[static_cast<std::size_t>(selected_)].box;
        const float outside = ringOffset + (ring + 2.0f * dpi) * 0.5f; // The ring's outer edge, its halo included.
        const ImVec2 boxMin = toScreen(box.x, box.y);
        const ImVec2 boxMax = toScreen(box.x + box.width, box.y + box.height);
        const ImVec2 zoneMin(std::floor(boxMin.x - outside), std::floor(boxMin.y - outside));
        const ImVec2 zoneMax(std::ceil(boxMax.x + outside), std::ceil(boxMax.y + outside));
        const ImVec2 clipMin = drawList->GetClipRectMin();
        const ImVec2 clipMax = drawList->GetClipRectMax();
        const ImVec4 bands[] = {
            {clipMin.x, clipMin.y, clipMax.x, zoneMin.y},
            {clipMin.x, zoneMax.y, clipMax.x, clipMax.y},
            {clipMin.x, zoneMin.y, zoneMin.x, zoneMax.y},
            {zoneMax.x, zoneMin.y, clipMax.x, zoneMax.y},
        };
        for (const ImVec4& band : bands) {
            if (band.z <= band.x || band.w <= band.y) {
                continue;
            }
            drawList->PushClipRect(ImVec2(band.x, band.y), ImVec2(band.z, band.w), true);
            for (std::size_t i = 0; i < marks_.size(); ++i) {
                if (static_cast<int>(i) != selected_ && static_cast<int>(i) != hovered_) {
                    drawMark(i);
                }
            }
            drawList->PopClipRect();
        }
        if (hovered_ >= 0 && hovered_ != selected_) {
            drawMark(static_cast<std::size_t>(hovered_));
        }
        selectedTag = drawMark(static_cast<std::size_t>(selected_));
    }

    if (drawing()) {
        const Box box = dragBox();
        const ImVec2 min = toScreen(box.x, box.y);
        const ImVec2 max = toScreen(box.x + box.width, box.y + box.height);
        theme::drawBox(drawList, min, max, theme::kYes, 2.0f * dpi);
    }
    return selectedTag;
}

void Review::drawAnswerBox(const theme::Fonts& fonts, ImVec2 viewMin, ImVec2 viewMax, theme::Area tag) {
    if (selected_ < 0 || drawing()) {
        return;
    }
    const float dpi = ImGui::GetStyle().FontScaleDpi;
    const Mark& mark = marks_[static_cast<std::size_t>(selected_)];
    const ImVec2 min = toScreen(mark.box.x, mark.box.y);
    const ImVec2 max = toScreen(mark.box.x + mark.box.width, mark.box.y + mark.box.height);
    // Zoomed in, the user moved away from it: no buttons floating at the view's edge.
    if (max.x <= viewMin_.x || min.x >= viewMax_.x || max.y <= viewMin_.y || min.y >= viewMax_.y) {
        return;
    }

    // Just the buttons: the mark's tag already asks the question ("Character? 81%"). One yes per kind ("Monster 1",
    // "Player 2", ...), or a plain Yes for a category without kinds; then No.
    static const char* const kDigits[] = {"1", "2", "3", "4", "5", "6", "7", "8", "9"};
    std::vector<BarButton> yeses;
    if (category_.kinds.empty()) {
        BarButton yes;
        yes.text = "Yes";
        yes.detail = "Y";
        yes.open = mark.answer == Answer::Yes;
        yes.highlight = theme::kYes;
        yeses.push_back(yes);
    }
    for (std::size_t i = 0; i < category_.kinds.size() && i < std::size(kDigits); ++i) {
        BarButton yes;
        yes.text = category_.kinds[i].name;
        yes.detail = kDigits[i];
        yes.open = mark.answer == Answer::Yes && mark.kind == i;
        yes.highlight = category_.kinds[i].color;
        yeses.push_back(yes);
    }
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
    float width = padding.x * 2.0f + no.width(fonts) + (drawn ? spacing + remove.width(fonts) : 0.0f);
    for (const BarButton& yes : yeses) {
        width += yes.width(fonts) + spacing;
    }
    const float height = padding.y * 2.0f + std::round(BarButton::kHeight * dpi);

    // Under the mark and its tag (above or, at the image's top edge, below the box), so neither is covered; above them
    // if there is no room below.
    const float gap = std::round(8.0f * dpi);
    float top = min.y;
    float bottom = max.y;
    if (tag.max.y > tag.min.y) {
        top = std::min(top, tag.min.y);
        bottom = std::max(bottom, tag.max.y);
    }
    ImVec2 position(std::round((min.x + max.x - width) * 0.5f), bottom + gap);
    if (position.y + height > viewMax.y) {
        position.y = top - gap - height;
    }
    position.x = std::max(std::min(position.x, viewMax.x - width), viewMin.x);
    position.y = std::max(std::min(position.y, viewMax.y - height), viewMin.y);
    ImGui::SetNextWindowPos(ImVec2(std::round(position.x), std::round(position.y)));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, padding);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 10.0f * dpi);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, theme::kSurface);
    ImGui::Begin("##answer", nullptr, kAnswerBox);
    for (std::size_t i = 0; i < yeses.size(); ++i) {
        ImGui::PushID(static_cast<int>(i));
        if (yeses[i].draw("##yes", fonts)) {
            answer(Answer::Yes, category_.kinds.empty() ? -1 : static_cast<int>(i), "button");
        }
        ImGui::PopID();
        ImGui::SameLine(0.0f, spacing);
    }
    if (no.draw("##no", fonts)) {
        answer(Answer::No, -1, "button");
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

void Review::drawZoom(ImDrawList* drawList) const {
    if (view_.zoom <= 1.0f && ImGui::GetTime() - zoomChangedAt_ > kZoomLabelSeconds) {
        return;
    }
    const float dpi = ImGui::GetStyle().FontScaleDpi;
    char text[32];
    std::snprintf(text, sizeof(text), "Zoom %.0f%%", static_cast<double>(view_.zoom * 100.0f));
    theme::drawLabel(drawList, ImVec2(viewMax_.x - 12.0f * dpi, viewMin_.y + 12.0f * dpi), ImVec2(1.0f, 0.0f), text,
                     view_.zoom > 1.0f ? "Right drag: move     0: whole image" : nullptr);
}

std::string Review::zoomText() const {
    if (view_.zoom <= 1.0f) {
        return {};
    }
    char text[32];
    std::snprintf(text, sizeof(text), ", zoomed in %.0f%%", static_cast<double>(view_.zoom * 100.0f));
    return text;
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
                      ", %d to answer before moving on", unanswered);
    } else if (reviewed_ && kindsToCheck() && length > 0 && static_cast<std::size_t>(length) < sizeof(where)) {
        std::snprintf(where + length, sizeof(where) - static_cast<std::size_t>(length), "  \xC2\xB7  kinds to check");
    } else if (reviewed_ && length > 0 && static_cast<std::size_t>(length) < sizeof(where)) {
        std::snprintf(where + length, sizeof(where) - static_cast<std::size_t>(length), "  \xC2\xB7  reviewed");
    }
    // That it learns, and from how much (the user asked to see it, 2026-11-08). Not while the answers changed since
    // the last fit (an undo, a changed answer): the next new image learns them.
    length = static_cast<int>(std::strlen(where));
    if (learning() && !relearn_ && learner_->learnedFrom() > 0) {
        std::snprintf(where + length, sizeof(where) - static_cast<std::size_t>(length),
                      "  \xC2\xB7  learned from %zu answers", learner_->learnedFrom());
    }
    length = static_cast<int>(std::strlen(where));
    if (nudging() && !relearn_ && boxLearner_.learnedFrom() > 0 && !reviewed_) {
        std::snprintf(where + length, sizeof(where) - static_cast<std::size_t>(length),
                      "  \xC2\xB7  boxes nudged your way");
    }
    drawList->AddText(ImVec2(min.x, textY), theme::kTextMuted, where);

    char answerKeys[48] = "Y / N: answer";
    if (!category_.kinds.empty()) {
        std::snprintf(answerKeys, sizeof(answerKeys), "1-%zu: kind     N: no", category_.kinds.size());
    }
    // As many key hints as fit next to the left text; when the window is too narrow, the first ones (the most
    // obvious) go first, so the two never overlap.
    // While images wait for the user (kinds to check, a guess added since), Shift+Right goes to the next of them.
    const char* imageKeys =
        toCheckCount() > 0 ? "Left / Right: images, Shift+Right: next to check" : "Left / Right: images";
    const char* hints[] = {"Click: pick a mark", "Drag: draw a missed one", answerKeys, "Tab: next mark",
                           imageKeys,            "Wheel: zoom",             "Ctrl+Z: undo"};
    const float room = max.x - min.x - ImGui::CalcTextSize(where).x - 24.0f * dpi;
    std::string keysText;
    for (std::size_t first = 0; first <= std::size(hints); ++first) {
        keysText.clear();
        for (std::size_t i = first; i < std::size(hints); ++i) {
            keysText += keysText.empty() ? "" : "     ";
            keysText += hints[i];
        }
        if (ImGui::CalcTextSize(keysText.c_str()).x <= room) {
            break;
        }
    }
    const char* keys = saveError_.empty() ? keysText.c_str() : saveError_.c_str();
    const float keysWidth = ImGui::CalcTextSize(keys).x;
    drawList->AddText(ImVec2(std::round(max.x - keysWidth), textY),
                      saveError_.empty() ? theme::kTextMuted : theme::kRecord, keys);
}

} // namespace observex
