#include "core/Question.h"

#include <doctest/doctest.h>

using namespace observex;

TEST_CASE("Question goes from asked to answered to taken") {
    Question question;
    CHECK(question.take() == Question::State::None);
    CHECK_FALSE(question.asked());

    question.ask();
    CHECK(question.asked());
    CHECK(question.take() == Question::State::Asked); // Still waiting: nothing taken.

    question.answer(true);
    CHECK_FALSE(question.asked());
    CHECK(question.take() == Question::State::Yes);
    CHECK(question.take() == Question::State::None); // Taken once.

    question.ask();
    question.answer(false);
    CHECK(question.take() == Question::State::No);
}

TEST_CASE("Question ignores answers to a dropped question, and to none") {
    Question question;
    question.answer(true);
    CHECK(question.take() == Question::State::None);

    question.ask();
    question.drop();
    question.answer(true);
    CHECK(question.take() == Question::State::None);

    question.ask();
    question.answer(true);
    question.ask(); // Asked again before the answer was taken: the old answer goes.
    CHECK(question.take() == Question::State::Asked);
}
