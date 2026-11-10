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
    CHECK(text == "frame 1 469805 0.0\nframe 2 469807 16.7\n");

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
    CHECK(text == "shown 12 104.2 17 0.92 0 0.90 2 0.30 1241 694 91 175 0.250 -0.125 0 3 0.61 0 0.61 2 0.30 1241 694 91 "
                  "175 0.250 -0.125 1\n");

    unsigned long long image = 0;
    double doneMs = 0.0;
    std::vector<Tracked> read(5);
    REQUIRE(readShown(text.substr(0, text.size() - 1), image, doneMs, read));
    CHECK(image == 12);
    CHECK(doneMs == doctest::Approx(104.2));
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
    CHECK(nothing == "shown 13 120.0\n");
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
