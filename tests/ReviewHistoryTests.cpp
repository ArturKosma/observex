#include "core/ReviewHistory.h"

#include <doctest/doctest.h>

using namespace observex;

namespace {

Mark guess(int x, float sure) {
    Mark mark;
    mark.box = Box{x, 10, 20, 30};
    mark.guess = sure;
    return mark;
}

} // namespace

TEST_CASE("undoing with no steps does nothing") {
    ReviewHistory history;
    ImageMarks answers;
    ImageMarks drafts;
    CHECK_FALSE(history.undo(answers, drafts));
    CHECK(history.size() == 0);
}

TEST_CASE("undoing an answer on an image not reviewed yet puts its draft back") {
    ReviewHistory history;
    ImageMarks answers;
    ImageMarks drafts;
    drafts["000002.jpg"] = {guess(0, 0.4f), guess(50, 0.5f)};
    const std::vector<Mark> before = drafts["000002.jpg"];

    history.push(1, "000002.jpg", 1, answers, drafts);
    drafts["000002.jpg"][1].answer = Answer::No;

    const auto undone = history.undo(answers, drafts);
    REQUIRE(undone);
    CHECK(undone->index == 1);
    CHECK(undone->selected == 1); // The mark it answered.
    CHECK_FALSE(undone->answersChanged);
    CHECK(drafts.at("000002.jpg") == before);
    CHECK(answers.empty());
    CHECK(history.size() == 0);
}

TEST_CASE("undoing the first answer on an image leaves no draft, so it shows its guesses again") {
    ReviewHistory history;
    ImageMarks answers;
    ImageMarks drafts;
    history.push(0, "000001.jpg", 0, answers, drafts);
    drafts["000001.jpg"] = {guess(0, 0.4f)};
    drafts["000001.jpg"][0].answer = Answer::Yes;

    REQUIRE(history.undo(answers, drafts));
    CHECK_FALSE(drafts.contains("000001.jpg"));
}

TEST_CASE("undoing an edit of a reviewed image puts its answers back and says to save them") {
    ReviewHistory history;
    ImageMarks answers;
    ImageMarks drafts;
    Mark yes = guess(0, 0.9f);
    yes.answer = Answer::Yes;
    answers["000001.jpg"] = {yes};

    // A box drawn, then removed again: two steps.
    history.push(0, "000001.jpg", -1, answers, drafts);
    answers["000001.jpg"].push_back(Mark{Box{100, 100, 40, 40}, Answer::Yes});
    history.push(0, "000001.jpg", 1, answers, drafts);
    answers["000001.jpg"].erase(answers["000001.jpg"].begin() + 1);

    auto undone = history.undo(answers, drafts); // The removal: the drawn box is back, and selected.
    REQUIRE(undone);
    CHECK(undone->answersChanged);
    CHECK(undone->selected == 1);
    REQUIRE(answers.at("000001.jpg").size() == 2);
    CHECK(answers.at("000001.jpg")[1].box == Box{100, 100, 40, 40});

    undone = history.undo(answers, drafts); // The drawing: only the first mark again.
    REQUIRE(undone);
    CHECK(undone->answersChanged);
    CHECK(undone->selected == -1);
    CHECK(answers.at("000001.jpg") == std::vector<Mark>{yes});
}

TEST_CASE("undoing a move that made an image reviewed takes it out of the answers and gives its draft back") {
    ReviewHistory history;
    ImageMarks answers;
    ImageMarks drafts;
    Mark yes = guess(0, 0.9f);
    yes.answer = Answer::Yes;
    yes.presumed = true;
    drafts["000003.jpg"] = {yes};

    // Right: the image becomes reviewed (its draft becomes its answers) and the next one shows.
    history.push(2, "000003.jpg", -1, answers, drafts);
    answers["000003.jpg"] = drafts["000003.jpg"];
    answers["000003.jpg"][0].presumed = false;
    drafts.erase("000003.jpg");

    const auto undone = history.undo(answers, drafts);
    REQUIRE(undone);
    CHECK(undone->index == 2); // Back on the image it came from.
    CHECK(undone->answersChanged);
    CHECK_FALSE(answers.contains("000003.jpg"));
    CHECK(drafts.at("000003.jpg") == std::vector<Mark>{yes});
}

TEST_CASE("undoing walks back through edits and moves in order") {
    ReviewHistory history;
    ImageMarks answers;
    ImageMarks drafts;
    answers["000001.jpg"] = {guess(0, 0.9f)};
    answers["000001.jpg"][0].answer = Answer::Yes;
    const ImageMarks reviewed = answers;

    history.push(0, "000001.jpg", 0, answers, drafts); // A kind changed on image 1,
    answers["000001.jpg"][0].kind = 2;
    history.push(0, "000001.jpg", 0, answers, drafts);  // Right, nothing else to change,
    history.push(1, "000002.jpg", -1, answers, drafts); // an answer on image 2,
    drafts["000002.jpg"] = {guess(5, 0.5f)};
    drafts["000002.jpg"][0].answer = Answer::No;
    history.push(1, "000002.jpg", -1, answers, drafts); // and Left.
    CHECK(history.size() == 4);

    auto undone = history.undo(answers, drafts); // Left: back on image 2, nothing else changes.
    CHECK(undone->index == 1);
    CHECK_FALSE(undone->answersChanged);
    CHECK(drafts.contains("000002.jpg"));
    undone = history.undo(answers, drafts); // The answer on image 2.
    CHECK(undone->index == 1);
    CHECK_FALSE(drafts.contains("000002.jpg"));
    undone = history.undo(answers, drafts); // Right: back on image 1, its answers as they were.
    CHECK(undone->index == 0);
    CHECK_FALSE(undone->answersChanged);
    CHECK(answers.at("000001.jpg")[0].kind == 2);
    undone = history.undo(answers, drafts); // The kind.
    CHECK(undone->index == 0);
    CHECK(undone->selected == 0);
    CHECK(undone->answersChanged);
    CHECK(answers == reviewed);
    CHECK_FALSE(history.undo(answers, drafts));
}
