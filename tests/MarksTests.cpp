#include "core/Marks.h"

#include <doctest/doctest.h>

using namespace observex;

TEST_CASE("overlap is 1 for the same box, 0 for apart boxes and the shared share otherwise") {
    const Box box{10, 10, 20, 20};
    CHECK(overlap(box, box) == doctest::Approx(1.0f));
    CHECK(overlap(box, Box{30, 10, 20, 20}) == 0.0f); // Touching edges share nothing.
    CHECK(overlap(box, Box{100, 100, 5, 5}) == 0.0f);
    // Shifted by half its width: shares 10 x 20 = 200 of 400 + 400 - 200 = 600.
    CHECK(overlap(box, Box{20, 10, 20, 20}) == doctest::Approx(1.0f / 3.0f));
    CHECK(overlap(Box{20, 10, 20, 20}, box) == doctest::Approx(1.0f / 3.0f));
}

TEST_CASE("parseMarks reads images, answers and guesses") {
    const ImageMarks marks = parseMarks("# observex answers\r\n"
                                        "image 000001.jpg\r\n"
                                        "yes 1012 540 80 120\r\n"
                                        "no 300 200 50 60\r\n"
                                        "\r\n"
                                        "image 000005.jpg\r\n"
                                        "image 000009.jpg\n"
                                        "0.72 10 20 30 40\n");
    REQUIRE(marks.size() == 3u);

    const std::vector<Mark>& first = marks.at("000001.jpg");
    REQUIRE(first.size() == 2u);
    CHECK(first[0].answer == Answer::Yes);
    CHECK(first[0].box == Box{1012, 540, 80, 120});
    CHECK(first[0].guess < 0.0f);
    CHECK(first[1].answer == Answer::No);
    CHECK(first[1].box == Box{300, 200, 50, 60});

    CHECK(marks.at("000005.jpg").empty()); // Reviewed, nothing on it.

    const std::vector<Mark>& third = marks.at("000009.jpg");
    REQUIRE(third.size() == 1u);
    CHECK(third[0].answer == Answer::None);
    CHECK(third[0].guess == doctest::Approx(0.72f));
}

TEST_CASE("parseMarks skips lines it does not understand") {
    const ImageMarks marks = parseMarks("yes 1 2 3 4\n" // Before any image line.
                                        "image 000001.jpg\n"
                                        "maybe 1 2 3 4\n"
                                        "yes 1 2 3\n"
                                        "yes 1 2 3 4 5\n"
                                        "yes 1 2 0 4\n" // No area.
                                        "1.5 1 2 3 4\n" // A guess above 1.
                                        "yes 1 2 3 4\n");
    REQUIRE(marks.size() == 1u);
    REQUIRE(marks.at("000001.jpg").size() == 1u);
    CHECK(marks.at("000001.jpg")[0].box == Box{1, 2, 3, 4});
}

TEST_CASE("formatMarks writes what parseMarks reads") {
    ImageMarks marks;
    marks["000002.jpg"] = {Mark{Box{1, 2, 3, 4}, Answer::Yes}, Mark{Box{5, 6, 7, 8}, Answer::No, 0.25f}};
    marks["000001.jpg"] = {Mark{Box{9, 9, 9, 9}, Answer::None, 0.5f}, Mark{Box{8, 8, 8, 8}}}; // The second has nothing.
    marks["000003.jpg"] = {};

    const std::string text = formatMarks(marks, "line one\nline two");
    CHECK(text == "# line one\n"
                  "# line two\n"
                  "image 000001.jpg\n"
                  "0.500 9 9 9 9\n"
                  "image 000002.jpg\n"
                  "yes 1 2 3 4\n"
                  "no 5 6 7 8\n"
                  "image 000003.jpg\n");

    const ImageMarks back = parseMarks(text);
    REQUIRE(back.size() == 3u);
    CHECK(back.at("000001.jpg").size() == 1u);
    CHECK(back.at("000002.jpg")[1].answer == Answer::No);
    CHECK(back.at("000003.jpg").empty());
}

namespace {
constexpr std::string_view kKinds[] = {"monster", "player", "npc", "own"};
}

TEST_CASE("parseMarks reads kinds, and yes as a kind not checked yet") {
    const ImageMarks marks = parseMarks("image 000001.jpg\n"
                                        "own 1 2 3 4\n"
                                        "monster 5 6 7 8\n"
                                        "yes 9 9 9 9\n"         // From before kinds: the first kind, to check.
                                        "yes player 8 8 8 8\n"  // To check, with a suggested kind.
                                        "no 7 7 7 7\n"
                                        "dragon 1 2 3 4\n"      // Not a kind of this category.
                                        "yes dragon 1 2 3 4\n"
                                        "player 1 2 3\n",
                                        kKinds);
    const std::vector<Mark>& first = marks.at("000001.jpg");
    REQUIRE(first.size() == 5u);
    CHECK(first[0].answer == Answer::Yes);
    CHECK(first[0].kind == 3);
    CHECK_FALSE(first[0].unchecked);
    CHECK(first[0].box == Box{1, 2, 3, 4});
    CHECK(first[1].kind == 0);
    CHECK_FALSE(first[1].unchecked);
    CHECK(first[2].answer == Answer::Yes);
    CHECK(first[2].kind == 0);
    CHECK(first[2].unchecked);
    CHECK(first[2].box == Box{9, 9, 9, 9});
    CHECK(first[3].kind == 1);
    CHECK(first[3].unchecked);
    CHECK(first[3].box == Box{8, 8, 8, 8});
    CHECK(first[4].answer == Answer::No);
    CHECK_FALSE(kindsChecked(first));

    // Without kinds a yes is just a yes, and kind words mean nothing.
    const ImageMarks plain = parseMarks("image 000001.jpg\nyes 1 2 3 4\nmonster 1 2 3 4\n");
    REQUIRE(plain.at("000001.jpg").size() == 1u);
    CHECK_FALSE(plain.at("000001.jpg")[0].unchecked);
}

TEST_CASE("can't tell and dead answers are the characters' last kinds, and read back as written") {
    const std::string text = "image 000001.jpg\nunknown 1 2 3 4\nmonster 5 6 7 8\ndead 9 9 9 9\n";
    const ImageMarks marks = parseMarks(text, kCharacterKindIds);
    const std::vector<Mark>& first = marks.at("000001.jpg");
    REQUIRE(first.size() == 3u);
    CHECK(first[0].answer == Answer::Yes);
    CHECK(first[0].kind == 4);
    CHECK_FALSE(first[0].unchecked);
    CHECK(first[2].answer == Answer::Yes);
    CHECK(first[2].kind == 5);
    CHECK(kindsChecked(first));
    CHECK(formatMarks(marks, {}, kCharacterKindIds) == text);
    // Read with the kinds of a model trained before them (the bench reads with all of them): left out.
    CHECK(parseMarks(text, kKinds).at("000001.jpg").size() == 1u);
}

TEST_CASE("formatMarks writes kinds, and yes with the suggested kind while unchecked") {
    ImageMarks marks;
    Mark checked{Box{1, 2, 3, 4}, Answer::Yes};
    checked.kind = 1;
    Mark unchecked{Box{5, 6, 7, 8}, Answer::Yes};
    unchecked.kind = 3;
    unchecked.unchecked = true;
    marks["000001.jpg"] = {checked, unchecked, Mark{Box{9, 9, 9, 9}, Answer::No}};

    const std::string text = formatMarks(marks, {}, kKinds);
    CHECK(text == "image 000001.jpg\n"
                  "player 1 2 3 4\n"
                  "yes own 5 6 7 8\n"
                  "no 9 9 9 9\n");
    const std::vector<Mark> back = parseMarks(text, kKinds).at("000001.jpg");
    REQUIRE(back.size() == 3u);
    CHECK(back[0].kind == 1);
    CHECK_FALSE(back[0].unchecked);
    CHECK(back[1].kind == 3);
    CHECK(back[1].unchecked);

    // Without kinds every yes is "yes".
    CHECK(formatMarks(marks) == "image 000001.jpg\nyes 1 2 3 4\nyes 5 6 7 8\nno 9 9 9 9\n");
}

TEST_CASE("a reviewed image with a guess added since is not fully answered, and keeps the guess when saved") {
    // As the sweep for characters nobody marked writes it: the image's answers, then the model's guess.
    const std::string text = "image 000001.jpg\n"
                             "monster 1 2 3 4\n"
                             "0.820 player 0.700 monster 0.400 50 60 20 40\n";
    const ImageMarks marks = parseMarks(text, kKinds);
    const std::vector<Mark>& image = marks.at("000001.jpg");
    REQUIRE(image.size() == 2u);
    CHECK(image[1].answer == Answer::None);
    CHECK(kindsChecked(image));
    CHECK_FALSE(fullyAnswered(image));
    CHECK(formatMarks(marks, {}, kKinds) == text);

    std::vector<Mark> answered = image;
    answered[1].answer = Answer::No;
    CHECK(fullyAnswered(answered));
    // A yes whose kind waits for the user is not done either.
    answered[0].unchecked = true;
    CHECK_FALSE(fullyAnswered(answered));
}

TEST_CASE("carryKinds suggests the kinds checked on the image before, and leaves them to check") {
    Mark player{Box{100, 100, 40, 40}, Answer::Yes};
    player.kind = 1;
    Mark notChecked{Box{300, 100, 40, 40}, Answer::Yes};
    notChecked.kind = 2;
    notChecked.unchecked = true;
    const std::vector<Mark> previous = {player, notChecked, Mark{Box{500, 100, 40, 40}, Answer::No}};

    Mark near{Box{104, 102, 40, 40}, Answer::Yes}; // The player, moved a little.
    near.unchecked = true;
    Mark nextToUnchecked{Box{302, 100, 40, 40}, Answer::Yes}; // Only an unchecked one there: nothing to copy.
    nextToUnchecked.unchecked = true;
    Mark onNo{Box{500, 100, 40, 40}, Answer::Yes}; // Only a no there.
    onNo.unchecked = true;
    Mark checkedHere{Box{100, 100, 40, 40}, Answer::Yes}; // Already checked: kept.
    std::vector<Mark> marks = {near, nextToUnchecked, onNo, checkedHere};

    CHECK(carryKinds(previous, marks) == 1);
    CHECK(marks[0].kind == 1);
    CHECK(marks[0].carried);
    CHECK(marks[0].unchecked); // Still the user's to check.
    CHECK(marks[1].kind == 0);
    CHECK_FALSE(marks[1].carried);
    CHECK(marks[2].kind == 0);
    CHECK(marks[3].kind == 0);
    CHECK_FALSE(marks[3].carried);
}

TEST_CASE("carryAnswers copies the answer of the most overlapping answered mark") {
    Mark yes{Box{100, 100, 40, 40}, Answer::Yes};
    yes.kind = 2; // Its kind comes along.
    const std::vector<Mark> previous = {
        yes, Mark{Box{300, 100, 40, 40}, Answer::No},
        Mark{Box{500, 100, 40, 40}, Answer::None, 0.9f}, // Unanswered: gives nothing.
    };
    std::vector<Mark> marks = {
        Mark{Box{104, 102, 40, 40}, Answer::None, 0.6f}, // Moved a little: same character.
        Mark{Box{302, 100, 40, 40}, Answer::None, 0.4f}, Mark{Box{500, 100, 40, 40}, Answer::None, 0.8f},
        Mark{Box{130, 100, 40, 40}, Answer::None, 0.3f}, // Overlaps the first by only a quarter.
        Mark{Box{100, 100, 40, 40}, Answer::No},         // Already answered: kept.
    };

    CHECK(carryAnswers(previous, marks) == 2);
    CHECK(marks[0].answer == Answer::Yes);
    CHECK(marks[0].kind == 2);
    CHECK(marks[0].carried);
    CHECK(marks[1].answer == Answer::No);
    CHECK(marks[1].carried);
    CHECK(marks[2].answer == Answer::None);
    CHECK(marks[3].answer == Answer::None);
    CHECK_FALSE(marks[4].carried);
    CHECK(marks[4].answer == Answer::No);
}

TEST_CASE("carryAnswers copies no answer onto a guess the review is sure is something else, and leaves it to the "
          "user") {
    Mark player{Box{300, 100, 40, 40}, Answer::Yes};
    player.kind = 1;
    Mark monster{Box{400, 100, 40, 40}, Answer::Yes};
    monster.kind = 0;
    Mark npc{Box{500, 100, 40, 40}, Answer::Yes};
    npc.kind = 2;
    const std::vector<Mark> previous = {Mark{Box{100, 100, 40, 40}, Answer::No},
                                        Mark{Box{200, 100, 40, 40}, Answer::No}, player, monster, npc};
    std::vector<Mark> marks = {
        Mark{Box{102, 100, 40, 40}, Answer::None, 0.9f}, // A lantern was there; the camera moved, now a player.
        Mark{Box{202, 100, 40, 40}, Answer::None, 0.4f}, // Unsure: the no carries.
        Mark{Box{302, 100, 40, 40}, Answer::None, 0.9f}, // Sure of the kind answered before: carried.
        Mark{Box{402, 100, 40, 40}, Answer::None, 0.9f}, // A monster before, sure it is the user's character now.
        Mark{Box{502, 100, 40, 40}, Answer::None, 0.5f}, // Not sure of any kind: the kind before carries.
    };
    int presumed[] = {1, -1, 1, 3, -1};

    CHECK(carryAnswers(previous, marks, presumed) == 3);
    CHECK(marks[0].answer == Answer::None);
    CHECK_FALSE(marks[0].carried);
    CHECK(presumed[0] == -1); // So it is not pre-filled either.
    CHECK(marks[1].answer == Answer::No);
    CHECK(marks[1].carried);
    CHECK(marks[2].answer == Answer::Yes);
    CHECK(marks[2].kind == 1);
    CHECK(marks[2].carried);
    CHECK(marks[3].answer == Answer::None); // Neither the monster before nor the sure kind: asked.
    CHECK_FALSE(marks[3].carried);
    CHECK(presumed[3] == -1);
    CHECK(marks[4].answer == Answer::Yes);
    CHECK(marks[4].kind == 2);

    CHECK(presumeKinds(marks, presumed) == 0);
    CHECK(marks[0].answer == Answer::None); // Asked.
    CHECK(marks[3].answer == Answer::None);
}

TEST_CASE("presumedKind of a guess alone: its kind when the finder is sure enough") {
    const auto guess = [](float sure) { return Mark{Box{100, 100, 40, 40}, Answer::None, sure}; };
    CHECK(presumedKind(guess(0.92f)) == 0);       // Sure (a one-category model: of a character).
    CHECK(presumedKind(guess(kPresumeYes)) == 0); // Exactly sure enough.
    CHECK(presumedKind(guess(0.6f)) == -1);       // Unsure: left to the user.
    // Drawn by the user: no guess.
    CHECK(presumedKind(Mark{Box{500, 100, 40, 40}, Answer::Yes}) == -1);
}

TEST_CASE("guesses of a model with kinds keep the kind it was surest of and the next") {
    const ImageMarks marks = parseMarks("image 000001.jpg\n"
                                        "0.820 player 0.700 own 0.400 1 2 3 4\n"
                                        "0.900 monster 0.900 5 6 7 8\n"             // No second kind.
                                        "0.820 dragon 0.800 player 0.100 1 1 1 1\n" // An unknown kind: the first.
                                        "0.600 1 1 1 1\n",                          // Without kinds.
                                        kKinds);
    const std::vector<Mark>& first = marks.at("000001.jpg");
    REQUIRE(first.size() == 4u);
    CHECK(first[0].answer == Answer::None);
    CHECK(first[0].guess == doctest::Approx(0.82f));
    CHECK(first[0].kind == 1);
    CHECK(first[0].kindGuess == doctest::Approx(0.7f));
    CHECK(first[0].secondKind == 3);
    CHECK(first[0].secondGuess == doctest::Approx(0.4f));
    CHECK(first[0].box == Box{1, 2, 3, 4});
    CHECK(first[1].kind == 0);
    CHECK(first[1].secondGuess == -1.0f);
    CHECK(first[1].box == Box{5, 6, 7, 8});
    CHECK(first[2].kind == 0);
    CHECK(first[2].kindGuess == doctest::Approx(0.8f));
    CHECK(first[2].secondKind == 1);
    CHECK(first[3].guess == doctest::Approx(0.6f));
    CHECK(first[3].secondGuess == -1.0f);
    CHECK(kindGuessOf(first[3]) == doctest::Approx(0.6f)); // Without kinds, how sure of its kind is `guess`.

    // Written back the same way; without kinds only how sure.
    ImageMarks guesses;
    guesses["000001.jpg"] = {first[0], first[1]};
    CHECK(formatMarks(guesses, {}, kKinds) == "image 000001.jpg\n"
                                               "0.820 player 0.700 own 0.400 1 2 3 4\n"
                                               "0.900 monster 0.900 5 6 7 8\n");
    CHECK(formatMarks(guesses) == "image 000001.jpg\n0.820 1 2 3 4\n0.900 5 6 7 8\n");
    // Read without kinds, the kind words are passed over.
    const ImageMarks plain = parseMarks("image 000001.jpg\n0.820 player 0.700 own 0.400 1 2 3 4\n");
    REQUIRE(plain.at("000001.jpg").size() == 1u);
    CHECK(plain.at("000001.jpg")[0].box == Box{1, 2, 3, 4});
}

TEST_CASE("presumedKind of a guess alone leaves it to the user when the finder was unsure which kind it is") {
    Mark close{Box{100, 100, 40, 40}, Answer::None, 0.85f}; // Player 85%, You 70%: unsure which kind.
    close.kind = 1;
    close.secondKind = 3;
    close.secondGuess = 0.7f;
    Mark apart = close; // Player 85%, You 10%: sure.
    apart.secondGuess = 0.1f;

    CHECK(presumedKind(close) == -1);
    CHECK(presumedKind(apart) == 1); // Yes of the kind it guessed.

    // Sure it is a character (93%) but not which kind (Player 85%, You 70%): asked too.
    Mark character = close;
    character.guess = 0.93f;
    character.kindGuess = 0.85f;
    CHECK(presumedKind(character) == -1);
}

TEST_CASE("presumedKind of a guess alone goes by how sure the finder was of the kind, not of a character") {
    Mark unsureKind{Box{100, 100, 40, 40}, Answer::None, 0.93f}; // A character 93%, but Monster only 70%.
    unsureKind.kind = 0;
    unsureKind.kindGuess = 0.7f;
    unsureKind.secondKind = 2;
    unsureKind.secondGuess = 0.3f;
    Mark sure = unsureKind; // Monster 90%.
    sure.kindGuess = 0.9f;

    CHECK(presumedKind(unsureKind) == -1);
    CHECK(presumedKind(sure) == 0);
}

TEST_CASE("sureKind is the surest kind when sure enough and clearly ahead of the next") {
    const float sure[] = {0.9f, 0.3f, 0.1f, 0.05f};
    CHECK(sureKind(sure, kPresumeYes) == 0);
    CHECK(sureKind(sure, 0.95f) == -1); // Not sure enough.
    const float close[] = {0.2f, 0.85f, 0.7f, 0.1f}; // Player 85%, NPC 70%: unsure which.
    CHECK(sureKind(close, kPresumeYes) == -1);
    const float apart[] = {0.2f, 0.85f, 0.65f, 0.1f}; // Exactly far enough apart.
    CHECK(sureKind(apart, kPresumeYes) == 1);
    const float one[] = {0.8f}; // A model of one category.
    CHECK(sureKind(one, kPresumeYes) == 0);
    CHECK(sureKind({}, kPresumeYes) == -1);
}

TEST_CASE("presumedKind: the model and the re-fit agreeing, or the re-fit very sure") {
    const float monster[] = {0.9f, 0.3f, 0.1f, 0.05f};
    const float player[] = {0.1f, 0.82f, 0.2f, 0.05f};     // Sure enough to agree on, not alone.
    const float surePlayer[] = {0.1f, 0.92f, 0.2f, 0.05f}; // Sure enough alone (kPresumeLearned).
    const float unsure[] = {0.6f, 0.3f, 0.1f, 0.05f};
    const float closeKinds[] = {0.2f, 0.97f, 0.9f, 0.05f};

    CHECK(presumedKind(monster, monster) == 0);    // Nothing learned yet: the model's own rule.
    CHECK(presumedKind(unsure, unsure) == -1);
    CHECK(presumedKind(monster, player) == -1);     // They disagree: the user answers.
    CHECK(presumedKind(monster, surePlayer) == 1);  // The re-fit learned it is a player: very sure.
    CHECK(presumedKind(unsure, surePlayer) == 1);
    CHECK(presumedKind(player, player) == 1);
    CHECK(presumedKind(monster, unsure) == -1);     // The answers made it unsure.
    CHECK(presumedKind(monster, closeKinds) == -1); // Very sure, but of two kinds.
    CHECK(presumedKind(closeKinds, closeKinds) == -1);
}

TEST_CASE("showKinds makes a guess show the kinds given, and presumeKinds answers it yes of one") {
    Mark guess{Box{100, 100, 40, 40}, Answer::None, 0.93f}; // Monster 90%, NPC 30% by the model.
    guess.kind = 0;
    guess.kindGuess = 0.9f;
    guess.secondKind = 2;
    guess.secondGuess = 0.3f;
    const float learned[] = {0.25f, 0.6f, 0.1f, 0.05f}; // Player 60%, Monster 25% after the user's answers.
    showKinds(guess, learned);
    CHECK(guess.kind == 1);
    CHECK(guess.kindGuess == doctest::Approx(0.6f));
    CHECK(guess.secondKind == 0);
    CHECK(guess.secondGuess == doctest::Approx(0.25f));
    CHECK(guess.answer == Answer::None);
    CHECK(guess.guess == doctest::Approx(0.93f)); // How sure of a character stays the finder's.
    const float one[] = {0.7f};
    Mark single = guess;
    showKinds(single, one);
    CHECK(single.kind == 0);
    CHECK(single.secondGuess == -1.0f);

    Mark carried = guess; // Answered no on the image before.
    carried.answer = Answer::No;
    carried.carried = true;
    std::vector<Mark> marks = {guess, guess, carried, guess};
    const int kinds[] = {1, -1, 0}; // The last has none.
    CHECK(presumeKinds(marks, kinds) == 1);
    CHECK(marks[0].answer == Answer::Yes);
    CHECK(marks[0].kind == 1);
    CHECK(marks[0].presumed);
    CHECK(marks[1].answer == Answer::None);
    CHECK(marks[2].answer == Answer::No); // An answer from the image before wins.
    CHECK_FALSE(marks[2].presumed);
    CHECK(marks[3].answer == Answer::None);
}

TEST_CASE("matchGuesses gives each guess the yes answer it matches, surest guess first, each answer once") {
    const auto guess = [](float sure, Box box) {
        Mark mark;
        mark.guess = sure;
        mark.box = box;
        return mark;
    };
    const auto answer = [](Answer what, std::uint8_t kind, Box box) {
        Mark mark;
        mark.answer = what;
        mark.kind = kind;
        mark.box = box;
        return mark;
    };
    const Box monster{100, 100, 40, 80};
    const Box player{300, 100, 40, 80};
    const std::vector<Mark> answers = {
        answer(Answer::Yes, 0, monster),
        answer(Answer::Yes, 1, player),
        answer(Answer::No, 0, Box{500, 100, 40, 80}),
        answer(Answer::Yes, 2, Box{700, 100, 40, 80}), // Drawn: no guess on it.
    };
    const std::vector<Mark> guesses = {
        guess(0.5f, Box{104, 100, 40, 80}), // Also on the monster, less sure: the surer one took it.
        guess(0.9f, monster),
        guess(0.6f, Box{310, 100, 40, 80}), // On the player, shifted a quarter: overlap 0.6.
        guess(0.7f, Box{500, 100, 40, 80}), // On the no.
        guess(0.4f, Box{330, 100, 40, 80}), // Too far off the player (overlap 0.14), which is taken anyway.
    };
    CHECK(matchGuesses(guesses, answers) == std::vector<int>{-1, 0, 1, -1, -1});
    CHECK(matchGuesses(guesses, {}) == std::vector<int>(5, -1));
    CHECK(matchGuesses({}, answers).empty());
    // A stricter overlap: the shifted one no longer counts.
    CHECK(matchGuesses(guesses, answers, 0.7f) == std::vector<int>{-1, 0, -1, -1, -1});

    // Two answers under one guess: it takes the one it overlaps most; the next guess the other.
    const std::vector<Mark> close = {answer(Answer::Yes, 1, Box{0, 0, 100, 100}),
                                     answer(Answer::Yes, 3, Box{10, 0, 100, 100})};
    CHECK(matchGuesses({guess(0.8f, Box{10, 0, 100, 100}), guess(0.3f, Box{0, 0, 100, 100})}, close) ==
          std::vector<int>{3, 1});
}

TEST_CASE("matchAnswers also takes the user's tighter box around the same thing, but not inside a group's box") {
    const auto guess = [](float sure, Box box) {
        Mark mark;
        mark.guess = sure;
        mark.box = box;
        return mark;
    };
    const auto yes = [](std::uint8_t kind, Box box) {
        Mark mark;
        mark.answer = Answer::Yes;
        mark.kind = kind;
        mark.box = box;
        return mark;
    };
    const Box loose{100, 100, 60, 100};
    const std::vector<Mark> answers = {
        Mark{loose, Answer::No},
        yes(1, Box{112, 115, 36, 70}),      // Tighter, all inside: overlap 0.42.
        Mark{Box{300, 100, 100, 100}, Answer::No},
        yes(0, Box{305, 110, 40, 80}),      // Two inside one guess: a group, split by the user.
        yes(0, Box{355, 110, 40, 80}),
        yes(2, Box{500, 100, 20, 20}),      // Inside the next guess, but too small: overlap 0.04.
        yes(3, Box{720, 100, 40, 100}),     // Overlapping the next guess a third, half of it outside.
    };
    const std::vector<Mark> guesses = {guess(0.9f, loose), guess(0.8f, Box{300, 100, 100, 100}),
                                       guess(0.7f, Box{500, 100, 100, 100}), guess(0.6f, Box{700, 100, 40, 100})};
    CHECK(matchAnswers(guesses, answers) == std::vector<int>{1, -1, -1, -1});
    CHECK(matchGuesses(guesses, answers) == std::vector<int>{1, -1, -1, -1});
    // The same box still matches by overlap alone.
    CHECK(matchAnswers({guess(0.9f, Box{305, 110, 40, 80})}, answers) == std::vector<int>{3});
    CHECK(shareInside(Box{112, 115, 36, 70}, loose) == doctest::Approx(1.0f));
    CHECK(shareInside(loose, Box{112, 115, 36, 70}) == doctest::Approx(0.42f));
    CHECK(shareInside(loose, Box{0, 0, 10, 10}) == 0.0f);
}

TEST_CASE("carryAnswers leaves a no the user drew again tighter, and carries their tighter box") {
    Mark tight{Box{112, 115, 36, 70}, Answer::Yes};
    tight.kind = 1;
    Mark left{Box{305, 110, 40, 80}, Answer::Yes};
    Mark right{Box{355, 110, 40, 80}, Answer::Yes};
    const std::vector<Mark> previous = {Mark{Box{100, 100, 60, 100}, Answer::No}, tight,
                                        Mark{Box{300, 100, 100, 100}, Answer::No}, left, right,
                                        Mark{Box{600, 100, 40, 40}, Answer::No}};
    CHECK(boxedAgain(previous[0], previous));
    CHECK_FALSE(boxedAgain(previous[2], previous)); // Split into two: a group's no.
    CHECK_FALSE(boxedAgain(previous[5], previous)); // A plain no.
    CHECK_FALSE(boxedAgain(previous[1], previous)); // A yes.

    std::vector<Mark> marks = {
        Mark{Box{101, 100, 60, 100}, Answer::None, 0.6f}, // The same loose box: the no is not carried, it is asked.
        Mark{Box{113, 116, 36, 70}, Answer::None, 0.6f},  // Nudged to the user's box: its kind is carried.
        Mark{Box{301, 100, 100, 100}, Answer::None, 0.6f}, // The group again: no.
        Mark{Box{601, 100, 40, 40}, Answer::None, 0.6f},   // No.
    };
    CHECK(carryAnswers(previous, marks) == 3);
    CHECK(marks[0].answer == Answer::None);
    CHECK(marks[1].answer == Answer::Yes);
    CHECK(marks[1].kind == 1);
    CHECK(marks[2].answer == Answer::No);
    CHECK(marks[3].answer == Answer::No);
}
