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

TEST_CASE("brain lines read back, and say what the State tile showed at each time") {
    std::string text;
    appendBrain(text, 1200, 0.0, {false, "", -1});
    appendBrain(text, 1203, 50.5, {true, "", 0});
    appendBrain(text, 1207, 83.25, {true, "Combat > Strike", 1});
    appendBrain(text, 1209, 120.0, {false, "", -1});
    CHECK(text == "brain 1200 0 0 -1\nbrain 1203 50.5 1 0\nbrain 1207 83.25 1 1 Combat > Strike\n"
                  "brain 1209 120 0 -1\n");

    unsigned long long image = 0;
    double timeMs = 0.0;
    statetree::BrainShown brain;
    REQUIRE(readBrain("brain 1207 83.25 1 1 Combat > Strike", image, timeMs, brain));
    CHECK(image == 1207);
    CHECK(timeMs == 83.25);
    CHECK(brain == statetree::BrainShown{true, "Combat > Strike", 1});
    REQUIRE(readBrain("brain 1203 50.5 1 -1", image, timeMs, brain)); // Root, its tasks done.
    CHECK(brain == statetree::BrainShown{true, "", -1});
    CHECK_FALSE(readBrain("brains 1203 50.5 1 0", image, timeMs, brain));
    CHECK_FALSE(readBrain("brain 1203 50.5 1", image, timeMs, brain));
    CHECK_FALSE(readBrain("brain 1203 50.5 yes 0", image, timeMs, brain));

    // A state's name with a line end in it stays one line.
    std::string broken;
    appendBrain(broken, 1, 0.0, {true, "Fight\nHard", 0});
    CHECK(broken == "brain 1 0 1 0 Fight Hard\n");

    const ClipText clip = readClipText("frame 1 1200 0\n" + text);
    REQUIRE(clip.brains.size() == 4);
    CHECK(recordedBrain(clip.brains, -5.0) == nullptr); // Before the first.
    CHECK(*recordedBrain(clip.brains, 0.0) == statetree::BrainShown{false, "", -1});
    CHECK(*recordedBrain(clip.brains, 60.0) == statetree::BrainShown{true, "", 0});
    CHECK(*recordedBrain(clip.brains, 100.0) == statetree::BrainShown{true, "Combat > Strike", 1});
    CHECK(*recordedBrain(clip.brains, 500.0) == statetree::BrainShown{false, "", -1});
}

TEST_CASE("camera lines read back exactly, and say what the Camera card showed at each time") {
    std::string text;
    const CameraShown none;
    const CameraShown turned{true, false, false, 0, -37.125f, 0.0f, 0.0f, 0.0f};
    const CameraShown guessed{true, true, true, 1, 359.98f, 21.43f, 0.31f, 0.0f};
    const CameraShown pinned{true, true, true, 3, 0.1f, 81.99987f, 0.0125f, 1.0f / 3.0f};
    appendCamera(text, 1200, 0.0, none);
    appendCamera(text, 1203, 50.5, turned);
    appendCamera(text, 1207, 83.25, guessed);
    appendCamera(text, 1209, 120.0, pinned);
    CHECK(text.starts_with("camera 1200 0 0 0 0 0 0 0 0 0\ncamera 1203 50.5 1 0 -37.125 0 0 0 0 0\n"));

    unsigned long long image = 0;
    double timeMs = 0.0;
    CameraShown camera;
    REQUIRE(readCamera("camera 1207 83.25 1 1 359.98 1 21.43 1 0.31 0", image, timeMs, camera));
    CHECK(image == 1207);
    CHECK(timeMs == 83.25);
    CHECK(camera == guessed);
    CHECK_FALSE(readCamera("cameras 1207 83.25 1 1 359.98 1 21.43 1 0.31 0", image, timeMs, camera));
    CHECK_FALSE(readCamera("camera 1207 83.25 1 1 359.98 1 21.43 1 0.31", image, timeMs, camera));
    CHECK_FALSE(readCamera("camera 1207 83.25 1 yes 359.98 1 21.43 1 0.31 0", image, timeMs, camera));

    const ClipText clip = readClipText("frame 1 1200 0\n" + text);
    REQUIRE(clip.cameras.size() == 4);
    CHECK(clip.cameras[3].camera == pinned); // Every number exactly (a third is no short decimal).
    CHECK(recordedCamera(clip.cameras, -5.0) == nullptr); // Before the first.
    CHECK(*recordedCamera(clip.cameras, 0.0) == none);
    CHECK(*recordedCamera(clip.cameras, 60.0) == turned);
    CHECK(*recordedCamera(clip.cameras, 100.0) == guessed);
    CHECK(*recordedCamera(clip.cameras, 500.0) == pinned);
}

TEST_CASE("mouse lines read back, counts, buttons and wheel exactly") {
    std::string text;
    appendMouse(text, {-12.5, 3, -7, 2, 0x0004, 0, 1280, 720});   // The right button pressed, moving.
    appendMouse(text, {0.25, -120, 45, 2, 0, 0, 1280, 720});       // Turning the camera fast.
    appendMouse(text, {7.0, 0, 0, 0, 0x0400, -240, -5, 2000});     // Two notches of the wheel towards the user.
    CHECK(text == "mouse -12.5 3 -7 2 4 0 1280 720\nmouse 0.25 -120 45 2 0 0 1280 720\n"
                  "mouse 7 0 0 0 1024 -240 -5 2000\n");

    ClipMouse mouse;
    REQUIRE(readMouse("mouse 0.25 -120 45 2 0 0 1280 720", mouse));
    CHECK(mouse == ClipMouse{0.25, -120, 45, 2, 0, 0, 1280, 720});
    CHECK_FALSE(readMouse("mice 0.25 -120 45 2 0 0 1280 720", mouse));
    CHECK_FALSE(readMouse("mouse 0.25 -120 45 2 0 0 1280", mouse));
    CHECK_FALSE(readMouse("mouse 0.25 left 45 2 0 0 1280 720", mouse));

    const ClipText clip = readClipText("frame 1 1200 0\n" + text);
    REQUIRE(clip.mice.size() == 3);
    CHECK(clip.mice[0] == ClipMouse{-12.5, 3, -7, 2, 4, 0, 1280, 720});
    CHECK(clip.mice[2].wheel == -240);
}

TEST_CASE("drops lines read back, and Recorded's drops are the last line at or before a time") {
    std::string text;
    Detection adena;
    adena.x = 1203.5f;
    adena.y = 801.25f;
    adena.width = 19.0f;
    adena.height = 9.0f;
    adena.sure = 0.8125f;
    Detection ring = adena;
    ring.x = 1250.0f;
    ring.sure = 0.5f;
    const std::vector<Detection> drops{adena, ring};
    appendDrops(text, 1200, 33.5, true, LootSquare{1000, 600, 400}, drops);
    appendDrops(text, 1210, 83.25, false, LootSquare{1100, 650, 300}, std::span<const Detection>(drops).first(1));
    appendDrops(text, 0, 150.0, false, LootSquare{}, {});
    CHECK(text.starts_with("drops 1200 33.5 1 1000 600 400 0.8125 1203.5 801.25 19 9 0.5 1250 801.25 19 9\n"));

    unsigned long long image = 0;
    double doneMs = 0.0;
    bool kill = false;
    LootSquare square;
    std::vector<Detection> read;
    REQUIRE(readDrops("drops 1210 83.25 0 1100 650 300 0.8125 1203.5 801.25 19 9", image, doneMs, kill, square,
                      read));
    CHECK(image == 1210);
    CHECK(doneMs == 83.25);
    CHECK_FALSE(kill);
    CHECK(square == LootSquare{1100, 650, 300});
    REQUIRE(read.size() == 1);
    CHECK(read[0].x == 1203.5f);
    CHECK(read[0].height == 9.0f);
    CHECK(read[0].sure == 0.8125f);
    CHECK_FALSE(readDrops("drop 1210 83.25 0 1100 650 300", image, doneMs, kill, square, read));
    CHECK_FALSE(readDrops("drops 1210 83.25", image, doneMs, kill, square, read));

    const ClipText clip = readClipText(text);
    REQUIRE(clip.drops.size() == 3);
    CHECK(recordedDrops(clip.drops, 10.0) == nullptr); // Before the first.
    const ClipDropsLine* first = recordedDrops(clip.drops, 50.0);
    REQUIRE(first != nullptr);
    CHECK(first->kill);
    CHECK(first->drops.size() == 2);
    REQUIRE(recordedDrops(clip.drops, 83.25) != nullptr);
    CHECK(recordedDrops(clip.drops, 83.25)->image == 1210);
    CHECK(recordedDrops(clip.drops, 200.0) == nullptr); // Nothing drawn from then on.
}
