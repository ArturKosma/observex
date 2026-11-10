#include "core/ReviewLines.h"

#include <doctest/doctest.h>

#include <cmath>
#include <string>
#include <string_view>
#include <vector>

using namespace observex;

namespace {

const std::string_view kKindNames[] = {"monster", "player", "npc", "own"};
const KindIds kKinds(kKindNames);

Mark guess(Box box, float sure, std::uint8_t kind = 0) {
    Mark mark;
    mark.box = box;
    mark.guess = sure;
    mark.kind = kind;
    return mark;
}

Mark answered(Box box, Answer answer, std::uint8_t kind = 0) {
    Mark mark;
    mark.box = box;
    mark.answer = answer;
    mark.kind = kind;
    return mark;
}

} // namespace

TEST_CASE("a mark is described by its kind and where that came from") {
    CHECK(describeBox(Box{1, 2, 30, 40}) == "[1 2 30 40]");
    Mark mark = guess(Box{10, 20, 30, 40}, 0.724f, 1);
    CHECK(describeWhere(mark) == "[10 20 30 40] guess 72%");
    CHECK(describeMark(mark, kKinds) == "unanswered");
    mark.answer = Answer::Yes;
    mark.presumed = true;
    CHECK(describeMark(mark, kKinds) == "player (pre-filled)");
    mark.presumed = false;
    mark.carried = true;
    CHECK(describeMark(mark, kKinds) == "player (copied from the image before)");
    mark.carried = false;
    CHECK(describeMark(mark, kKinds) == "player (yours)");
    mark.unchecked = true;
    CHECK(describeMark(mark, kKinds) == "player (kind not checked)");
    mark = answered(Box{0, 0, 5, 5}, Answer::No);
    CHECK(describeMark(mark, kKinds) == "no (yours)");
    CHECK(describeWhere(mark) == "[0 0 5 5]"); // Drawn, or read back from the answers file.
    CHECK(describeMark(answered(Box{}, Answer::Yes), {}) == "yes (yours)"); // A category without kinds.
}

TEST_CASE("sureness lists the kinds surest first, from 1%") {
    const float sure[] = {0.12f, 0.85f, 0.001f, 0.03f};
    CHECK(describeSure(sure, kKinds) == "player 85%, monster 12%, own 3%");
    const float none[] = {0.0f, 0.001f, 0.0f, 0.0f};
    CHECK(describeSure(none, kKinds) == "nothing");
}

TEST_CASE("a pre-fill says which rule answered it") {
    CHECK(describePrefill(guess(Box{0, 0, 10, 10}, 0.5f, 0), {}, {}) == "asked");
    // Sure enough to pre-fill, yet unanswered: another answer at its place on the image before was not copied onto it.
    const std::string held = "asked, though sure enough to pre-fill: the image before had another answer here";
    Mark mark = guess(Box{0, 0, 10, 10}, 0.9f, 0);
    CHECK(describePrefill(mark, {}, {}) == held);
    const float sure[] = {0.05f, 0.95f, 0.0f, 0.0f};
    const float unsure[] = {0.4f, 0.5f, 0.0f, 0.0f};
    CHECK(describePrefill(mark, unsure, sure) == held); // Learning: the re-fit is sure.
    CHECK(describePrefill(mark, unsure, unsure) == "asked");
    mark.answer = Answer::Yes;
    mark.carried = true;
    CHECK(describePrefill(mark, {}, {}) == "copied from the image before");
    mark.carried = false;
    mark.presumed = true;
    CHECK(describePrefill(mark, {}, {}) == "the model is 78%+ sure");
    const float both[] = {0.9f, 0.05f, 0.0f, 0.0f};
    CHECK(describePrefill(mark, both, both) == "the model and the re-fit agree, both 78%+ sure");
    const float model[] = {0.1f, 0.6f, 0.0f, 0.0f};
    const float learned[] = {0.95f, 0.05f, 0.0f, 0.0f};
    CHECK(describePrefill(mark, model, learned) == "the re-fit is 90%+ sure");
}

TEST_CASE("overlaps name the marks a box shares area with, or lies inside") {
    const std::vector<Mark> marks = {
        guess(Box{100, 100, 100, 100}, 0.7f),             // One box over two characters.
        answered(Box{100, 100, 45, 100}, Answer::Yes, 1), // Drawn inside it, on the left one.
        answered(Box{300, 100, 50, 50}, Answer::No),      // Elsewhere.
        answered(Box{195, 100, 50, 100}, Answer::Yes, 0), // Touches its edge only a little.
    };
    const std::vector<std::string> lines = describeOverlaps(marks, 1, kKinds);
    REQUIRE(lines.size() == 1);
    CHECK(lines[0] == "mark 0 [100 100 100 100] guess 70% unanswered: overlap 45%, 100% of this box inside it, 45% of "
                      "it inside this box");
    CHECK(describeOverlaps(marks, 2, kKinds).empty());
    CHECK(describeOverlaps(marks, 9, kKinds).empty());
}

TEST_CASE("changes on an image: changed, confirmed, drawn, removed and kept marks") {
    Mark presumed = guess(Box{0, 0, 10, 10}, 0.9f, 0);
    presumed.answer = Answer::Yes;
    presumed.presumed = true;
    Mark asked = guess(Box{20, 0, 10, 10}, 0.5f, 0);
    Mark carried = guess(Box{40, 0, 10, 10}, 0.8f, 1);
    carried.answer = Answer::Yes;
    carried.carried = true;
    Mark keptOne = presumed;
    keptOne.box = Box{60, 0, 10, 10};
    const Mark drawnBefore = answered(Box{80, 0, 10, 10}, Answer::Yes, 2);
    const std::vector<Mark> before = {presumed, asked, carried, keptOne, drawnBefore};

    std::vector<Mark> after = before;
    after[0].kind = 1; // Pre-filled monster, the user says player.
    after[0].presumed = false;
    after[1].answer = Answer::No;                               // Asked, the user says no.
    after[2].carried = false;                                   // Copied player, the user answered player again.
    after.erase(after.begin() + 4);                             // Removed the box drawn before.
    after.push_back(answered(Box{5, 5, 4, 4}, Answer::Yes, 3)); // Drew a new one.

    const std::vector<std::string> lines = describeChanges(before, after, kKinds);
    const std::vector<std::string> expected = {
        "changed mark 0 [0 0 10 10] guess 90%: monster (pre-filled) -> player (yours)",
        "changed mark 1 [20 0 10 10] guess 50%: unanswered -> no (yours)",
        "confirmed mark 2 [40 0 10 10] guess 80%: player (copied from the image before) -> player (yours)",
        "drew mark 4 [5 5 4 4]: own (yours)",
        "removed mark 4 [80 0 10 10]: npc (yours)",
        "kept 1 mark as shown",
    };
    CHECK(lines == expected);
    CHECK(describeChanges(before, before, kKinds) == std::vector<std::string>{"kept all 5 marks as shown"});
    CHECK(describeChanges({}, {}, kKinds).empty());
}

TEST_CASE("counts per kind, then no and unanswered") {
    const std::vector<Mark> marks = {answered(Box{}, Answer::Yes, 0), answered(Box{}, Answer::Yes, 0),
                                     answered(Box{}, Answer::Yes, 3), answered(Box{}, Answer::No), guess(Box{}, 0.4f)};
    CHECK(describeCounts(marks, kKinds) == "monster 2, own 1, no 1, unanswered 1");
    CHECK(describeCounts({}, kKinds) == "no marks");
}

TEST_CASE("a fit says what it learned from and how far it moved the classifier") {
    KindHead head;
    head.biases = {0.0f, -1.0f};
    head.weights = {1.0f, 0.0f, 0.0f, 1.0f};
    KindLearner learner(head);
    const std::string_view twoNames[] = {"monster", "player"};
    const KindIds two(twoNames);
    learner.fit({}, {});
    CHECK(describeFit(learner, {}, two, 0.0) == "learned from no answers yet: the model's own kinds");

    const std::vector<float> features = {2.0f, 0.0f, 2.0f, 0.0f, 0.0f, 2.0f};
    const std::vector<int> answers = {1, 1, KindLearner::kNo};
    learner.fit(features, answers);
    const std::string text = describeFit(learner, answers, two, 3.14);
    CHECK(text.starts_with("learned from 3 answers (player 2, no 1) in 3.1 ms; weights moved "));
    CHECK(text.find("% from the model's; biases monster ") != std::string::npos);
    CHECK(text.find(", player +") != std::string::npos); // Answered player twice: its bias went up.
}

TEST_CASE("a box fit says what it learned from and how the user boxes near the image") {
    BoxLearner learner;
    learner.fit({}, 0);
    CHECK(describeBoxFit(learner, 0.0) == "learned from no answered guesses yet: the finder's boxes");

    BoxSamples samples;
    samples.size = 2;
    samples.features = {1.0f, 0.0f, 0.0f, 1.0f};
    samples.nudges = {Nudge{0.1f, -0.05f, std::log(0.8f), std::log(0.75f)},
                      Nudge{0.1f, -0.05f, std::log(0.8f), std::log(0.75f)}};
    samples.images = {3, 3};
    learner.fit(samples, 3);
    CHECK(describeBoxFit(learner, 4.24) ==
          "learned from 2 answered guesses in 4.2 ms; the user's boxes near this image on average: 0.80 of the "
          "guess's width, 0.75 of its height, the middle 10% of it right, 5% up");
}
