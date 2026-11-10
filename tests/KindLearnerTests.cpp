#include "core/KindLearner.h"

#include "KindFitFixture.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <utility>
#include <vector>

using namespace observex;

namespace {

// Two kinds over eight numbers: the model takes anything with the numbers up for a monster (kind 0), from either half.
KindHead twoKinds() {
    KindHead head;
    head.biases = {0.0f, -2.0f};
    head.weights = {0.25f, 0.25f, 0.25f, 0.25f, 0.25f, 0.25f, 0.25f, 0.25f, // Monster.
                    0.0f,  0.0f,  0.0f,  0.0f,  0.0f,  0.0f,  0.0f,  0.0f}; // Player.
    return head;
}

// Something the model sees in the first half (a party member it takes for a monster), and something else, in the
// other half.
const std::vector<float> kPartyMember = {3.0f, 3.0f, 3.0f, 3.0f, 0.0f, 0.0f, 0.0f, 0.0f};
const std::vector<float> kOther = {0.0f, 0.0f, 0.0f, 0.0f, 3.0f, 3.0f, 3.0f, 3.0f};

std::vector<float> learnedSure(const KindLearner& learner, const std::vector<float>& feature) {
    std::vector<float> own(static_cast<std::size_t>(learner.kinds()));
    std::vector<float> learned(own.size());
    learner.sure(feature, own, learned);
    return learned;
}

} // namespace

TEST_CASE("a fit with no answers is the model's own classifier, exactly") {
    KindLearner learner(twoKinds());
    CHECK(learner.kinds() == 2);
    CHECK(learner.features() == 8);
    learner.fit({}, {});
    CHECK(learner.learned() == twoKinds());
    CHECK(learner.learnedFrom() == 0u);

    std::vector<float> own(2);
    std::vector<float> learned(2);
    learner.sure(kPartyMember, own, learned);
    CHECK(own == learned);
    CHECK(own[0] == doctest::Approx(1.0f / (1.0f + std::exp(-3.0f))));
    CHECK(own[1] == doctest::Approx(1.0f / (1.0f + std::exp(2.0f))));

    // Also after a fit with answers: each fit starts from the model's own.
    const int player = 1;
    learner.fit(kPartyMember, {&player, 1});
    CHECK(learner.learned() != twoKinds());
    learner.fit({}, {});
    CHECK(learner.learned() == twoKinds());
}

TEST_CASE("a corrected kind flips after a fit, and something unrelated keeps its kind") {
    KindLearner learner(twoKinds());
    REQUIRE(learnedSure(learner, kPartyMember)[0] > 0.9f); // Taken for a monster.
    const int player = 1;
    learner.fit(kPartyMember, {&player, 1});
    CHECK(learner.learnedFrom() == 1u);

    const std::vector<float> member = learnedSure(learner, kPartyMember);
    CHECK(member[1] > 0.9f);
    CHECK(member[0] < 0.1f);
    const std::vector<float> other = learnedSure(learner, kOther);
    CHECK(other[0] > 0.9f); // Still a monster: only the biases moved for it.
    CHECK(other[1] < 0.25f);
}

TEST_CASE("a no answer lowers every kind") {
    KindLearner learner(twoKinds());
    const int no = KindLearner::kNo;
    learner.fit(kPartyMember, {&no, 1});
    const std::vector<float> sure = learnedSure(learner, kPartyMember);
    CHECK(sure[0] < 0.1f);
    CHECK(sure[1] < 0.1f);
}

TEST_CASE("a fit stays near the model's own weights") {
    // Fifty answers saying the same: still no weight moves further than the steps go, and those the answers say
    // nothing about (the other half) not at all.
    std::vector<float> features;
    std::vector<int> answers(50, 1);
    for (int i = 0; i < 50; ++i) {
        features.insert(features.end(), kPartyMember.begin(), kPartyMember.end());
    }
    KindLearner learner(twoKinds());
    learner.fit(features, answers);
    const KindHead own = twoKinds();
    float moved = 0.0f;
    for (std::size_t i = 0; i < own.weights.size(); ++i) {
        const float change = std::abs(learner.learned().weights[i] - own.weights[i]);
        moved = std::max(moved, change);
        if (i % 8 >= 4) {
            CHECK(change == 0.0f);
        }
    }
    CHECK(moved > 0.3f);
    CHECK(moved <= KindLearner::kSteps * KindLearner::kStepSize * 1.01f);
}

TEST_CASE("a fit matches PyTorch's on real samples") {
    using namespace kind_fit;
    KindHead head;
    head.weights.assign(std::begin(kWeights), std::end(kWeights));
    head.biases.assign(std::begin(kBiases), std::end(kBiases));
    KindLearner learner(head);
    REQUIRE(learner.features() == kFeatures);
    REQUIRE(learner.kinds() == kKinds);
    learner.fit({kSampleFeatures, static_cast<std::size_t>(kLearned * kFeatures)}, kAnswers);

    float ownApart = 0.0f;
    float learnedApart = 0.0f;
    for (int sample = 0; sample < kSamples; ++sample) {
        std::vector<float> own(kKinds);
        std::vector<float> learned(kKinds);
        learner.sure({kSampleFeatures + sample * kFeatures, kFeatures}, own, learned);
        for (int kind = 0; kind < kKinds; ++kind) {
            ownApart = std::max(ownApart, std::abs(own[kind] - kOwnSure[sample * kKinds + kind]));
            learnedApart = std::max(learnedApart, std::abs(learned[kind] - kLearnedSure[sample * kKinds + kind]));
        }
    }
    MESSAGE("furthest from PyTorch: own " << ownApart << ", learned " << learnedApart);
    CHECK(ownApart < 1e-5f);
    CHECK(learnedApart < 1e-3f);
}

TEST_CASE("the review's pace is the default, and the kind update's learns more from the same answers") {
    const int player = 1;
    KindLearner review(twoKinds());
    review.fit(kPartyMember, {&player, 1});
    KindLearner same(twoKinds());
    same.fit(kPartyMember, {&player, 1}, KindLearner::kSteps, KindLearner::kPull);
    CHECK(same.learned() == review.learned());

    KindLearner update(twoKinds());
    update.fit(kPartyMember, {&player, 1}, KindLearner::kUpdateSteps, KindLearner::kUpdatePull);
    CHECK(update.learnedFrom() == 1u);
    CHECK(learnedSure(update, kPartyMember)[1] > learnedSure(review, kPartyMember)[1]);
    // More steps and no pull: its weights go further from the model's.
    const auto moved = [](const KindLearner& learner) {
        float most = 0.0f;
        for (std::size_t i = 0; i < learner.own().weights.size(); ++i) {
            most = std::max(most, std::abs(learner.learned().weights[i] - learner.own().weights[i]));
        }
        return most;
    };
    CHECK(moved(update) > moved(review));
}

TEST_CASE("pickKinds takes the head's kinds and keeps how sure the model is of a character") {
    // The head takes the party member for a monster: sigmoid(3) for monster, sigmoid(-2) for player.
    Detection thing{100.0f, 50.0f, 20.0f, 40.0f, 0.8f, 1};
    thing.secondKind = 0;
    thing.second = 0.3f;
    thing.kindSure = 0.6f;
    thing.candidate = 17;
    pickKinds(twoKinds(), kPartyMember, thing);
    const float monster = 1.0f / (1.0f + std::exp(-3.0f));
    const float player = 1.0f / (1.0f + std::exp(2.0f));
    CHECK(thing.kind == 0);
    CHECK(thing.secondKind == 1);
    CHECK(thing.kindSure == doctest::Approx(0.8f * monster / (monster + player)));
    CHECK(thing.second == doctest::Approx(0.8f * player / (monster + player)));
    CHECK(thing.sure == 0.8f); // What is found stays the model's.
    CHECK(thing.x == 100.0f);
    CHECK(thing.height == 40.0f);
    CHECK(thing.candidate == 17);
    CHECK(eitherKind(thing.kindSure, thing.second) <= thing.sure); // Never surer of a kind than of a character.

    // One that has learnt it is a player.
    KindHead learned = twoKinds();
    learned.biases = {-2.0f, 0.0f};
    for (std::size_t i = 0; i < 8; ++i) {
        std::swap(learned.weights[i], learned.weights[8 + i]);
    }
    pickKinds(learned, kPartyMember, thing);
    CHECK(thing.kind == 1);
    CHECK(thing.secondKind == 0);
    CHECK(thing.kindSure == doctest::Approx(0.8f * monster / (monster + player)));
}

TEST_CASE("pickKinds leaves a thing as it is for a head it cannot use") {
    Detection thing{1.0f, 2.0f, 3.0f, 4.0f, 0.7f, 2};
    thing.secondKind = 3;
    thing.second = 0.2f;
    thing.kindSure = 0.5f;
    const auto unchanged = [&](const Detection& after) {
        return after.kind == 2 && after.secondKind == 3 && after.second == 0.2f && after.kindSure == 0.5f &&
               after.sure == 0.7f;
    };
    Detection copy = thing;
    pickKinds(KindHead{}, kPartyMember, copy); // None.
    CHECK(unchanged(copy));
    pickKinds(twoKinds(), std::vector<float>(7, 1.0f), copy); // Features of another size.
    CHECK(unchanged(copy));
    KindHead many;
    many.biases.assign(kMaxKinds + 1, 0.0f);
    many.weights.assign(many.biases.size() * 8, 0.0f);
    pickKinds(many, kPartyMember, copy); // More kinds than a model can have.
    CHECK(unchanged(copy));
}

TEST_CASE("addAnsweredGuesses pairs each guess with its answer by overlap, on reviewed images only") {
    const auto mark = [](Answer what, std::uint8_t kind, Box box, float guess = -1.0f) {
        Mark made;
        made.answer = what;
        made.kind = kind;
        made.box = box;
        made.guess = guess;
        return made;
    };
    const Box thing{100, 100, 40, 80};
    // A newer model's guesses: their boxes a little off those the user answered (overlap 0.82).
    ImageMarks guesses;
    guesses["a.jpg"] = {mark(Answer::None, 0, Box{104, 100, 40, 80}, 0.9f),  // On the player answered yes.
                        mark(Answer::None, 0, Box{502, 100, 40, 80}, 0.8f),  // On a no.
                        mark(Answer::None, 0, Box{900, 100, 40, 80}, 0.4f)}; // On nothing answered: a no too.
    guesses["b.jpg"] = {mark(Answer::None, 0, thing, 0.9f)};
    guesses["c.jpg"] = {mark(Answer::None, 0, thing, 0.9f)};
    GuessFeatures features;
    features.size = 2;
    features.images["a.jpg"] = {1.0f, 1.0f, 2.0f, 2.0f, 3.0f, 3.0f};
    features.images["b.jpg"] = {4.0f, 4.0f};
    features.images["c.jpg"] = {5.0f, 5.0f};
    ImageMarks answers;
    answers["a.jpg"] = {mark(Answer::Yes, 1, thing), mark(Answer::No, 0, Box{500, 100, 40, 80})};
    Mark unchecked = mark(Answer::Yes, 0, thing);
    unchecked.unchecked = true;
    answers["b.jpg"] = {unchecked};                   // Answered before kinds, not checked since: nothing.
    answers["c.jpg"] = {mark(Answer::Yes, 5, thing)}; // A kind past the head's: nothing.
    answers["d.jpg"] = {mark(Answer::Yes, 0, thing)}; // No guesses there: nothing.

    std::vector<float> sampleFeatures = {9.0f, 9.0f}; // Appended to.
    std::vector<int> sampleAnswers = {0};
    CHECK(addAnsweredGuesses(guesses, features, answers, 2, sampleFeatures, sampleAnswers) == 3);
    CHECK(sampleFeatures == std::vector<float>{9.0f, 9.0f, 1.0f, 1.0f, 2.0f, 2.0f, 3.0f, 3.0f});
    CHECK(sampleAnswers == std::vector<int>{0, 1, KindLearner::kNo, KindLearner::kNo});

    // Without features (none, or an older model's), nothing.
    CHECK(addAnsweredGuesses(guesses, GuessFeatures{}, answers, 2, sampleFeatures, sampleAnswers) == 0);
    CHECK(sampleAnswers.size() == 4u);
}
