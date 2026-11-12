#include "core/ClipGuesses.h"
#include "core/ClipLines.h"
#include "core/LiveLines.h"
#include "core/Recordings.h"

#include <doctest/doctest.h>

#include <string>
#include <vector>

using namespace observex;

TEST_CASE("frame lines read back") {
    std::string text;
    appendFrame(text, 1, 469805, 0.0);
    appendFrame(text, 2, 469807, 16.66);
    CHECK(text == "frame 1 469805 0\nframe 2 469807 16.66\n");

    unsigned frame = 0;
    unsigned long long image = 0;
    double timeMs = 0.0;
    REQUIRE(readFrame("frame 2 469807 16.7", frame, image, timeMs));
    CHECK(frame == 2);
    CHECK(image == 469807);
    CHECK(timeMs == doctest::Approx(16.7));
    CHECK_FALSE(readFrame("frames 2 469807 16.7", frame, image, timeMs));
    CHECK_FALSE(readFrame("frame", frame, image, timeMs));
    CHECK_FALSE(readFrame("look 1 2.0", frame, image, timeMs));
}

TEST_CASE("shown lines hold every followed thing and read back") {
    Tracked monster;
    monster.x = 1241;
    monster.y = 694;
    monster.width = 91;
    monster.height = 175;
    monster.sure = 0.92f;
    monster.kind = 0;
    monster.kindSure = 0.9f;
    monster.secondKind = 2;
    monster.second = 0.3f;
    monster.id = 17;
    monster.speedX = 0.25f;
    monster.speedY = -0.125f;
    Tracked held = monster;
    held.id = 3;
    held.kindSure = -1.0f; // Its kind sureness is its sureness.
    held.sure = 0.61f;
    held.held = true;
    const std::vector<Tracked> things{monster, held};

    std::string text;
    appendShown(text, 12, 104.25, things);
    CHECK(text == "shown 12 104.25 17 0.92 0 0.9 2 0.3 1241 694 91 175 0.25 -0.125 0 3 0.61 0 0.61 2 0.3 1241 694 91 175 "
                  "0.25 -0.125 1\n");

    unsigned long long image = 0;
    double doneMs = 0.0;
    std::vector<Tracked> read(5);
    REQUIRE(readShown(text.substr(0, text.size() - 1), image, doneMs, read));
    CHECK(image == 12);
    CHECK(doneMs == doctest::Approx(104.25));
    REQUIRE(read.size() == 2);
    CHECK(read[0].id == 17);
    CHECK(read[0].sure == doctest::Approx(0.92f));
    CHECK(read[0].kindSure == doctest::Approx(0.9f));
    CHECK(read[0].secondKind == 2);
    CHECK(read[0].second == doctest::Approx(0.3f));
    CHECK(read[0].x == 1241.0f);
    CHECK(read[0].height == 175.0f);
    CHECK(read[0].speedY == doctest::Approx(-0.125f));
    CHECK_FALSE(read[0].held);
    CHECK(read[1].id == 3);
    CHECK(read[1].kindSure == doctest::Approx(0.61f));
    CHECK(read[1].held);

    std::string nothing;
    appendShown(nothing, 13, 120.0, {});
    CHECK(nothing == "shown 13 120\n");
    REQUIRE(readShown("shown 13 120.0", image, doneMs, read));
    CHECK(read.empty());
    CHECK_FALSE(readShown("look 13 120.0", image, doneMs, read));
}

TEST_CASE("note lines stay one line") {
    std::string text;
    appendNote(text, 2500.0, "recording started");
    appendNote(text, 3000.04, "two\nlines");
    CHECK(text == "note 2500.0 recording started\nnote 3000.0 two lines\n");

    double timeMs = 0.0;
    std::string note;
    REQUIRE(readNote("note 2500.0 recording started", timeMs, note));
    CHECK(timeMs == doctest::Approx(2500.0));
    CHECK(note == "recording started");
    REQUIRE(readNote("note 1.5", timeMs, note));
    CHECK(note.empty());
    CHECK_FALSE(readNote("notes 1.5 x", timeMs, note));
}

TEST_CASE("a clip's look lines are the live log's, so the replay reads them between the other lines") {
    std::string text;
    appendFrame(text, 1, 7, 0.0);
    char look[kLookChars];
    const std::vector<Detection> found{{100, 200, 40, 80, 0.8f}};
    text.append(look, writeLook(look, 7, 0.0, found));
    appendNote(text, 1.0, "mark 1");

    std::vector<Detection> read;
    unsigned long long image = 0;
    double timeMs = 0.0;
    int looks = 0;
    std::size_t start = 0;
    while (start < text.size()) {
        const std::size_t end = text.find('\n', start);
        if (readLook(std::string_view(text).substr(start, end - start), image, timeMs, read)) {
            ++looks;
        }
        start = end + 1;
    }
    CHECK(looks == 1);
    CHECK(image == 7);
    REQUIRE(read.size() == 1);
    CHECK(read[0].x == 100.0f);
}

TEST_CASE("clip info reads back what was written") {
    ClipInfo info;
    info.saved = "2026-11-10 18:22:05";
    info.seconds = 10.25;
    info.frames = 1234;
    info.looks = 2011;
    info.screen = "Odyssey G61SH";
    info.width = 2560;
    info.height = 1440;
    info.model = "C:\\models\\character\\model.onnx (written 2026-11-10 00:18)";
    const ClipInfo read = parseClipInfo(formatClipInfo(info));
    CHECK(read.saved == info.saved);
    CHECK(read.seconds == doctest::Approx(10.25));
    CHECK(read.frames == 1234);
    CHECK(read.looks == 2011);
    CHECK(read.screen == info.screen);
    CHECK(read.width == 2560);
    CHECK(read.height == 1440);
    CHECK(read.model == info.model);

    const ClipInfo odd = parseClipInfo("# a comment\r\nsaved=2026-11-10 01:02:03\r\nsize=bad\nunknown=1\nframes\n");
    CHECK(odd.saved == "2026-11-10 01:02:03");
    CHECK(odd.width == 0);
    CHECK(odd.frames == 0);
}

TEST_CASE("snapped lines read back with their image's time and their lead") {
    Tracked thing;
    thing.id = 4;
    thing.x = 1300.5f;
    thing.y = 700.25f;
    thing.width = 90.0f;
    thing.height = 170.0f;
    thing.sure = 0.8f;
    thing.speedX = 2.5f;
    thing.held = true;
    std::string text;
    appendSnapped(text, 12, 100.125, 103.5, 6.0, std::vector<Tracked>{thing});
    REQUIRE(text.back() == '\n');
    unsigned long long image = 0;
    double timeMs = 0.0;
    double doneMs = 0.0;
    double leadMs = 0.0;
    std::vector<Tracked> read;
    REQUIRE(readSnapped(text.substr(0, text.size() - 1), image, timeMs, doneMs, leadMs, read));
    CHECK(image == 12);
    CHECK(timeMs == 100.125);
    CHECK(doneMs == 103.5);
    CHECK(leadMs == 6.0);
    REQUIRE(read.size() == 1);
    CHECK(read[0].id == 4);
    CHECK(read[0].x == 1300.5f);
    CHECK(read[0].y == 700.25f);
    CHECK(read[0].speedX == 2.5f);
    CHECK(read[0].held);
    CHECK_FALSE(readShown(text, image, doneMs, read)); // Not a shown line,
    CHECK_FALSE(readSnapped("snapped 12 100.1", image, timeMs, doneMs, leadMs, read)); // nor a whole snapped one.
}

TEST_CASE("finds lines read back every search exactly, with the look taken in") {
    SnapFind found;
    found.finder = 12345;
    found.id = 7;
    found.learnedImage = 4000000001ull;
    found.learnedX = 100.125f;
    found.learnedY = 50.0625f;
    found.learnedWidth = 30.0f;
    found.learnedHeight = 28.5f;
    found.guessX = 131.3f;
    found.guessY = 50.1f;
    found.reach = 26.7f;
    found.found = true;
    found.x = 133.0390625f;
    found.y = 49.9f;
    found.match = 0.93f;
    SnapFind lost = found;
    lost.id = 9;
    lost.learnedImage = 0; // Nothing learned of it yet.
    lost.found = false;
    lost.x = 0.0f;
    lost.y = 0.0f;
    lost.match = -2.0f;
    std::string text;
    appendFinds(text, 4000000003ull, 4000000001ull, std::vector<SnapFind>{found, lost});
    REQUIRE(text.back() == '\n');
    unsigned long long image = 0;
    unsigned long long lookImage = 0;
    std::vector<SnapFind> read;
    REQUIRE(readFinds(text.substr(0, text.size() - 1), image, lookImage, read));
    CHECK(image == 4000000003ull);
    CHECK(lookImage == 4000000001ull);
    REQUIRE(read.size() == 2);
    for (std::size_t i = 0; i < 2; ++i) {
        const SnapFind& was = i == 0 ? found : lost;
        CHECK(read[i].finder == was.finder);
        CHECK(read[i].id == was.id);
        CHECK(read[i].learnedImage == was.learnedImage);
        CHECK(read[i].learnedX == was.learnedX);
        CHECK(read[i].learnedHeight == was.learnedHeight);
        CHECK(read[i].guessX == was.guessX);
        CHECK(read[i].reach == was.reach);
        CHECK(read[i].found == was.found);
        CHECK(read[i].x == was.x);
        CHECK(read[i].y == was.y);
        CHECK(read[i].match == was.match);
    }
    // No searches (nothing followed) still says which look was taken in.
    text.clear();
    appendFinds(text, 5, 0, {});
    REQUIRE(readFinds(text, image, lookImage, read));
    CHECK(image == 5);
    CHECK(lookImage == 0);
    CHECK(read.empty());
    CHECK_FALSE(readFinds("snapped 5 1 2 3", image, lookImage, read));
}

TEST_CASE("ground lines read back the grid exactly as drawn, and say what was seen at each time") {
    GroundCamera drawn;
    drawn.known = true;
    drawn.pitchMeasured = true;
    drawn.pitch = 0.3712345f;
    drawn.distance = 10.551f;
    drawn.focal = 1700.0f;
    drawn.middleX = 1280.0f;
    drawn.middleY = 720.0f;
    drawn.yaw = -2.0943951f;
    drawn.feetX = 0.125f;
    drawn.feetY = -3.7512f;
    GroundCamera later = drawn; // A newer camera drawn over the same image.
    later.yaw = 1.0000001f;
    later.pitchMeasured = false;
    std::string text;
    appendFrame(text, 1, 100, 0.0);
    appendGround(text, 99, -8.25, 97, drawn); // Drawn before the first frame: seen on it.
    appendGround(text, 100, 0.0, 98, drawn);
    appendGround(text, 100, 0.0, 99, later);
    appendFrame(text, 2, 103, 25.0);
    appendGround(text, 103, 25.0, 0, GroundCamera{}); // The flag went off.
    CHECK(text.find("ground 100 0 98 1 1 0.3712345 10.551 1700 1280 720 ") != std::string::npos);
    CHECK(text.find("ground 103 25 0 0 0 0 0 0 0 0 0 0 0\n") != std::string::npos);

    const ClipText clip = readClipText(text);
    REQUIRE(clip.grounds.size() == 4);
    const GroundCamera& read = clip.grounds[1].camera;
    CHECK(clip.grounds[1].image == 100);
    CHECK(clip.grounds[1].followed == 98);
    CHECK(read.known);
    CHECK(read.pitchMeasured);
    CHECK(read.pitch == drawn.pitch); // Exactly.
    CHECK(read.distance == drawn.distance);
    CHECK(read.yaw == drawn.yaw);
    CHECK(read.feetY == drawn.feetY);
    CHECK(read.middleX == drawn.middleX);

    CHECK_FALSE(recordedGround(clip.grounds, -9.0).known); // Before the first line.
    CHECK(recordedGround(clip.grounds, -1.0).yaw == drawn.yaw);
    CHECK(recordedGround(clip.grounds, 0.0).yaw == later.yaw); // The last drawn over that image.
    CHECK_FALSE(recordedGround(clip.grounds, 0.0).pitchMeasured);
    CHECK(recordedGround(clip.grounds, 24.9).yaw == later.yaw);
    CHECK_FALSE(recordedGround(clip.grounds, 25.0).known);
    CHECK(followedImages(clip) == std::vector<unsigned long long>{97, 98, 99});

    unsigned long long image = 0;
    double timeMs = 0.0;
    unsigned long long followed = 0;
    GroundCamera camera;
    CHECK_FALSE(readGround("ground 100 0 98 1", image, timeMs, followed, camera));
    CHECK_FALSE(readGround("grounds 100 0 98 1 1 0 0 0 0 0 0 0 0", image, timeMs, followed, camera));
}
