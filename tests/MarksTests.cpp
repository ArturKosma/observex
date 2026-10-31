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

TEST_CASE("carryAnswers copies the answer of the most overlapping answered mark") {
    const std::vector<Mark> previous = {
        Mark{Box{100, 100, 40, 40}, Answer::Yes}, Mark{Box{300, 100, 40, 40}, Answer::No},
        Mark{Box{500, 100, 40, 40}, Answer::None, 0.9f}, // Unanswered: gives nothing.
    };
    std::vector<Mark> marks = {
        Mark{Box{104, 102, 40, 40}, Answer::None, 0.6f}, // Moved a little: same monster.
        Mark{Box{302, 100, 40, 40}, Answer::None, 0.4f}, Mark{Box{500, 100, 40, 40}, Answer::None, 0.8f},
        Mark{Box{130, 100, 40, 40}, Answer::None, 0.3f}, // Overlaps the first by only a quarter.
        Mark{Box{100, 100, 40, 40}, Answer::No},         // Already answered: kept.
    };

    CHECK(carryAnswers(previous, marks) == 2);
    CHECK(marks[0].answer == Answer::Yes);
    CHECK(marks[0].carried);
    CHECK(marks[1].answer == Answer::No);
    CHECK(marks[1].carried);
    CHECK(marks[2].answer == Answer::None);
    CHECK(marks[3].answer == Answer::None);
    CHECK_FALSE(marks[4].carried);
    CHECK(marks[4].answer == Answer::No);
}
