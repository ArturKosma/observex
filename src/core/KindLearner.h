#pragma once

#include "core/Detection.h"
#include "core/Features.h"
#include "core/Marks.h"
#include "core/ModelInfo.h"

#include <cstddef>
#include <span>
#include <vector>

namespace observex {

// Re-fits a model's kind classifier (its KindHead: the last step from a guess's features to its kinds) to the user's
// answers so far in a review, so each next image's guesses use what the user corrected (the user asked, 2026-11-08:
// "better auto-guesses with each picture in a review"; e.g. a party member the model takes for a monster on 40 images
// in a row). Only the classifier learns, from the model's own weights each time, pulled back towards them so a few
// answers cannot turn it upside down; the boxes stay the model's. A fit takes the answers of a whole recording.
class KindLearner {
public:
    // An answer that is not in the category ("no"): every kind's target is 0.
    static constexpr int kNo = -1;

    // How it learns, as simulated in Python on the tenth model (2026-11-08; PyTorch's Adam, full batch): this many
    // steps of this size, and how strongly the weights are pulled back towards the model's own (times the sum of
    // their squared change, added to the mean binary cross-entropy of every answer's every kind; biases are free).
    // The pull 0.01 -> 0.003 on 2026-11-08 with core/Marks' kPresumeLearned (the numbers there): it learns more from
    // each answer. 0.001 learned more still but pre-filled more wrong.
    static constexpr int kSteps = 60;
    static constexpr float kStepSize = 0.01f;
    static constexpr float kPull = 0.003f;
    // The kind update's pace (engine/KindUpdater: every reviewed answer of every recording at once, between trainings;
    // the user asked, 2026-11-08), the same step size. Simulated in Python on the tenth model (2026-11-08, the yes and
    // no answers of its guesses at 30% and up on the eleventh's training images, ~9,000): used to pick the kind live
    // (pickKinds), the right kind on the 343 held-out images 95.0% -> 95.9%, on the new place it never trained on
    // (2026-11-07) 82% -> 92%, found and false alarms unchanged (1887 / 136). With that many answers no pull is
    // needed; fitting only the newest answers forgets older places (97% -> 77% right there), so it fits them all.
    // PyTorch took 1.2 s on one CPU thread.
    static constexpr int kUpdateSteps = 300;
    static constexpr float kUpdatePull = 0.0f;

    explicit KindLearner(KindHead own);

    int kinds() const { return own_.kinds(); }
    int features() const { return own_.features(); } // Numbers per feature.

    // Fits the classifier to `answers`, one per feature in `features` (answers.size() features, one after another):
    // the kind the user answered (an index into the head's kinds), or kNo. With no answers it is the model's own.
    // `steps` and `pull`: the review's pace above, or the kind update's.
    void fit(std::span<const float> features, std::span<const int> answers, int steps = kSteps, float pull = kPull);
    // How many answers the last fit learned from.
    std::size_t learnedFrom() const { return learnedFrom_; }

    // How sure the model's own classifier (`own`) and the re-fit one (`learned`) are of each kind (0 to 1, kinds()
    // each) of a guess with `feature`.
    void sure(std::span<const float> feature, std::span<float> own, std::span<float> learned) const;

    const KindHead& own() const { return own_; }
    const KindHead& learned() const { return learned_; }

private:
    KindHead own_;
    KindHead learned_;
    std::size_t learnedFrom_ = 0;
};

// Gives `thing` (found by the model, with `feature` its features) the kinds the kind update's classifier (`head`)
// sees, keeping how sure the model is of a character (`thing.sure`), so what is found stays the model's: its kind and
// second kind are the head's surest two, and how sure of each is `thing.sure` times the head's share of it (that
// kind's sigmoid over the sum of all kinds'). So it is never surer of a kind than of a character. Simulated in Python
// ("pick+own", 2026-11-08) on the tenth's 343 held-out images with an update from ~9,000 answers: the right kind
// 95.0% -> 95.9% (on the new place 82% -> 92%: party players taken for monsters 29 -> about 11), found and false
// alarms unchanged, the review's pre-filled answers about as before (1384 right / 66 wrong / 762 asked against 1370 /
// 67 / 775), images with a thing unsure which kind (core/Detection's kindsClose) 89 -> 46. Leaves `thing` as it is if
// the head has no kinds or more than kMaxKinds, or `feature` is not its size. No heap allocation.
void pickKinds(const KindHead& head, std::span<const float> feature, Detection& thing);

// What a fit learns from on one recording: each guess (`guesses`, with their `features`) on each reviewed image (in
// `answers`, every kind checked), its features appended to `sampleFeatures` and its answer to `sampleAnswers`: the kind
// of the yes answer it matches by overlap (core/Marks' matchGuesses), else KindLearner::kNo. By overlap, not the same
// box: once a new model makes the guesses again, their boxes are a little off those the user answered, and the
// answers must still count (the review matched the same box only until 2026-11-08: after the eleventh went live it
// learned from 2 of 9 answers). Images answered before kinds and not checked since teach nothing, nor a guess
// matching a yes of a kind past `kinds`, nor one without features. The review and the kind update both learn so.
// Returns how many answers it appended.
std::size_t addAnsweredGuesses(const ImageMarks& guesses, const GuessFeatures& features, const ImageMarks& answers,
                               int kinds, std::vector<float>& sampleFeatures, std::vector<int>& sampleAnswers);

} // namespace observex
