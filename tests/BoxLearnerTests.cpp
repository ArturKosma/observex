#include "core/BoxLearner.h"

#include <doctest/doctest.h>

#include <cmath>
#include <string>
#include <vector>

using namespace observex;

namespace {

// Eight numbers per feature: a far-off character the user boxes tighter (the first half up), one near that they keep
// as boxed (the second half up), each a little different.
std::vector<float> far(int i) {
    const float v = 1.0f + 0.1f * static_cast<float>(i % 5);
    return {v, v, v, v, 0.0f, 0.0f, 0.0f, 0.1f * static_cast<float>(i % 3)};
}
std::vector<float> near(int i) {
    const float v = 1.0f + 0.1f * static_cast<float>(i % 4);
    return {0.0f, 0.1f * static_cast<float>(i % 3), 0.0f, 0.0f, v, v, v, v};
}

const Nudge kTighter{0.0f, -0.05f, std::log(0.7f), std::log(0.7f)};

void add(BoxSamples& samples, const std::vector<float>& feature, const Nudge& nudge, int image) {
    samples.size = static_cast<int>(feature.size());
    samples.features.insert(samples.features.end(), feature.begin(), feature.end());
    samples.nudges.push_back(nudge);
    samples.images.push_back(image);
}

bool close(const Nudge& a, const Nudge& b, float within) {
    return std::abs(a.x - b.x) <= within && std::abs(a.y - b.y) <= within && std::abs(a.width - b.width) <= within &&
           std::abs(a.height - b.height) <= within;
}

Mark guess(float sure, Box box) {
    Mark mark;
    mark.guess = sure;
    mark.box = box;
    return mark;
}

Mark answer(Answer what, Box box, std::uint8_t kind = 1) {
    Mark mark;
    mark.answer = what;
    mark.kind = kind;
    mark.box = box;
    return mark;
}

} // namespace

TEST_CASE("a nudge between two boxes nudges the one into the other") {
    const Box guess{100, 200, 40, 80};
    const Box yours{106, 218, 28, 56}; // 0.7 of the width and height, its middle 6 pixels down: 0.075 of the height.
    const Nudge nudge = nudgeBetween(guess, yours);
    CHECK(nudge.x == doctest::Approx(0.0f));
    CHECK(nudge.y == doctest::Approx(0.075f));
    CHECK(nudge.width == doctest::Approx(std::log(0.7f)));
    CHECK(nudge.height == doctest::Approx(std::log(0.7f)));
    CHECK(nudged(guess, nudge) == yours);
    CHECK(nudged(guess, Nudge{}) == guess);
    CHECK(nudgeBetween(guess, guess) == Nudge{});
}

TEST_CASE("a nudge is held to the most, and kept inside the image") {
    const Box guess{100, 100, 100, 100};
    // Moved and resized far past the most: half the width right, 1.65 times as wide, 0.37 times as tall.
    const Box box = nudged(guess, Nudge{3.0f, 0.0f, 2.0f, -4.0f});
    CHECK(box.width == static_cast<int>(std::lround(100.0f * std::exp(kMaxNudgeSize))));
    CHECK(box.height == static_cast<int>(std::lround(100.0f * std::exp(kMinNudgeSize))));
    CHECK(box.x + box.width / 2 == doctest::Approx(200).epsilon(0.01));

    // At the image's right edge, made wider: cut at the edge. Shrunk to nothing: one pixel.
    const Box image{0, 0, 2560, 1440};
    const Box wider = nudged(Box{2500, 100, 50, 50}, Nudge{0.0f, 0.0f, 0.5f, 0.0f}, image);
    CHECK(wider.x + wider.width == 2560);
    CHECK(nudged(Box{-20, 0, 10, 10}, Nudge{}, image) == Box{0, 0, 1, 10});
    CHECK(nudged(Box{10, 10, 1, 1}, Nudge{0.0f, 0.0f, -1.0f, -1.0f}).width == 1);
}

TEST_CASE("a box fit with no samples does not nudge") {
    BoxLearner learner;
    learner.fit({}, 0);
    CHECK(learner.learnedFrom() == 0u);
    CHECK(learner.nudge(far(0)) == Nudge{});
}

TEST_CASE("a box fit learns how the user boxes far-off characters, and leaves near ones as boxed") {
    BoxSamples samples;
    for (int i = 0; i < 30; ++i) {
        add(samples, far(i), kTighter, i);
        add(samples, near(i), Nudge{}, i);
    }
    BoxLearner learner;
    learner.fit(samples, 30);
    CHECK(learner.learnedFrom() == 60u);
    CHECK(close(learner.nudge(far(7)), kTighter, 0.05f));
    CHECK(close(learner.nudge(near(7)), Nudge{}, 0.05f));
    // The average: half kept, half tighter.
    CHECK(learner.average().width == doctest::Approx(std::log(0.7f) / 2.0f).epsilon(0.01));
    // A feature of another size: no nudge.
    CHECK(learner.nudge(std::vector<float>(4, 1.0f)) == Nudge{});
}

TEST_CASE("a few samples nudge only by about their average, many by what each says") {
    BoxSamples few;
    add(few, far(0), kTighter, 0);
    add(few, near(0), Nudge{}, 0);
    BoxLearner learner;
    learner.fit(few, 1);
    // The pull holds the weights back: both nudged about halfway, as their average.
    const Nudge farNudge = learner.nudge(far(1));
    const Nudge nearNudge = learner.nudge(near(1));
    CHECK(farNudge.width < 0.0f);
    CHECK(nearNudge.width < 0.0f);
    CHECK(std::abs(farNudge.width - nearNudge.width) < 0.15f);
}

TEST_CASE("the images near the one fitted for count most") {
    BoxSamples samples;
    for (int i = 0; i < 20; ++i) {
        add(samples, far(i), kTighter, i);      // The user boxed far-off characters tighter early on,
        add(samples, far(i), Nudge{}, 80 + i);  // and kept them as boxed later.
    }
    BoxLearner learner;
    learner.fit(samples, 10);
    CHECK(close(learner.nudge(far(2)), kTighter, 0.05f));
    learner.fit(samples, 90);
    CHECK(close(learner.nudge(far(2)), Nudge{}, 0.05f));
    // With no half life all count the same: about halfway.
    learner.fit(samples, 10, BoxLearner::kPull, 0.0f);
    CHECK(learner.nudge(far(2)).width == doctest::Approx(std::log(0.7f) / 2.0f).epsilon(0.1));
}

TEST_CASE("addAnsweredBoxes takes each guess matching a yes answer, with the nudge to the user's box") {
    const std::vector<std::string> images = {"000001.jpg", "000002.jpg", "000003.jpg"};
    const Box kept{100, 100, 40, 80};
    const Box loose{300, 100, 60, 100};
    const Box tight{312, 115, 36, 70}; // Drawn inside the loose guess: overlap 0.42.
    const Box group{500, 100, 100, 100};
    ImageMarks guesses;
    guesses["000002.jpg"] = {guess(0.9f, kept), guess(0.8f, loose), guess(0.7f, group),
                             guess(0.6f, Box{800, 100, 40, 40})};
    guesses["000003.jpg"] = {guess(0.9f, kept)};
    guesses["000009.jpg"] = {guess(0.9f, kept)}; // Not among the images: left out.
    ImageMarks answers;
    answers["000002.jpg"] = {
        answer(Answer::Yes, kept),
        answer(Answer::No, loose),
        answer(Answer::No, group),
        answer(Answer::No, Box{800, 100, 40, 40}),
        answer(Answer::Yes, tight),
        answer(Answer::Yes, Box{505, 110, 40, 80}), // Two drawn inside the group's box: it matches neither.
        answer(Answer::Yes, Box{555, 110, 40, 80}),
    };
    answers["000009.jpg"] = {answer(Answer::Yes, kept)};
    GuessFeatures features;
    features.size = 2;
    features.images["000002.jpg"] = {1, 1, 2, 2, 3, 3, 4, 4};
    features.images["000003.jpg"] = {5, 5};
    features.images["000009.jpg"] = {6, 6};

    BoxSamples samples;
    CHECK(addAnsweredBoxes(guesses, features, answers, images, samples) == 2u);
    CHECK(samples.size == 2);
    CHECK(samples.features == std::vector<float>{1, 1, 2, 2});
    CHECK(samples.nudges[0] == Nudge{});
    CHECK(samples.nudges[1] == nudgeBetween(loose, tight));
    CHECK(samples.images == std::vector<int>{1, 1});

    // Features of another size than the samples': nothing.
    GuessFeatures other = features;
    other.size = 1;
    CHECK(addAnsweredBoxes(guesses, other, answers, images, samples) == 0u);
}
