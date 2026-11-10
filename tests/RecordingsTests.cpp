#include "core/Recordings.h"

#include <doctest/doctest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

using namespace observex;

TEST_CASE("a recording's info survives writing and reading") {
    RecordingInfo info;
    info.started = "2026-11-01 12:56:11";
    info.seconds = 34;
    info.screen = "Odyssey G61SH";
    info.width = 2560;
    info.height = 1440;
    info.every = 3;
    info.kind = RecordKind::Unsure;
    info.images = 12;
    const RecordingInfo read = parseRecordingInfo(formatRecordingInfo(info));
    CHECK(read.started == info.started);
    CHECK(read.seconds == 34);
    CHECK(read.screen == "Odyssey G61SH");
    CHECK(read.width == 2560);
    CHECK(read.height == 1440);
    CHECK(read.every == 3);
    CHECK(read.kind == RecordKind::Unsure);
    CHECK(read.images == 12);
}

TEST_CASE("reading a recording's info skips what it does not know") {
    const RecordingInfo read =
        parseRecordingInfo("# a comment\r\nstarted=2026-10-31 23:59:49\r\nnew-key=1\r\nnot a line\r\nimages=60\r\n");
    CHECK(read.started == "2026-10-31 23:59:49");
    CHECK(read.images == 60);
    CHECK(read.seconds == 0);
    CHECK(read.screen.empty());
    CHECK(read.kind == RecordKind::Every); // Recordings before Record asked were every 3 s.
    CHECK(parseRecordingInfo("unsure=1\n").kind == RecordKind::Unsure);  // The first "Only unsure" ones.
    CHECK(parseRecordingInfo("kind=auto\n").kind == RecordKind::Unsure); // Auto went on to only unsure.
}

TEST_CASE("record kinds go to their words and back") {
    for (const RecordKind kind : {RecordKind::Every, RecordKind::Unsure}) {
        CHECK(parseRecordKind(recordKindId(kind)) == kind);
    }
    CHECK(parseRecordKind("") == RecordKind::Every);
    CHECK(parseRecordKind("later") == RecordKind::Every);
}

TEST_CASE("a recording is done after its images, and says how long is left") {
    CHECK_FALSE(recordingDone(kRecordImages - 1));
    CHECK(recordingDone(kRecordImages));
    // Every 3 s: exactly the images left, whatever the time.
    CHECK(recordingSecondsLeft(RecordKind::Every, 3, 0, 0.0) == doctest::Approx(kRecordImages * 3.0));
    CHECK(recordingSecondsLeft(RecordKind::Every, 3, kRecordImages - 10, 999.0) == doctest::Approx(30.0));
    // Only unsure: a new place's pace until it has a few, then its own (here one image every 6 s).
    CHECK(recordingSecondsLeft(RecordKind::Unsure, 3, 0, 0.0) == doctest::Approx(kRecordImages * 4.5));
    CHECK(recordingSecondsLeft(RecordKind::Unsure, 3, 10, 60.0) == doctest::Approx((kRecordImages - 10) * 6.0));
    CHECK(recordingSecondsLeft(RecordKind::Unsure, 3, kRecordImages + 1, 60.0) == 0.0);
}

TEST_CASE("recordings are listed newest first, only folders with a recording file") {
    const std::filesystem::path root =
        std::filesystem::temp_directory_path() /
        ("observex-tests-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    for (const char* name : {"2026-11-01_09-00-00", "2026-11-01_12-00-00", "2026-10-31_23-00-00", "no-file"}) {
        std::filesystem::create_directories(root / name);
    }
    for (const auto& [name, started] : {std::pair{"2026-11-01_09-00-00", "2026-11-01 09:00:00"},
                                        std::pair{"2026-11-01_12-00-00", "2026-11-01 12:00:00"},
                                        std::pair{"2026-10-31_23-00-00", "2026-10-31 23:00:00"}}) {
        RecordingInfo info;
        info.started = started;
        REQUIRE(writeRecordingInfo(root / name, info));
    }
    std::ofstream(root / "stray.observex") << "started=2026-12-01 00:00:00\n"; // A file, not a folder: skipped.

    const std::vector<RecordingEntry> recordings = listRecordings(root);
    REQUIRE(recordings.size() == 3);
    CHECK(recordings[0].info.started == "2026-11-01 12:00:00");
    CHECK(recordings[1].info.started == "2026-11-01 09:00:00");
    CHECK(recordings[2].folder.filename() == "2026-10-31_23-00-00");
    std::filesystem::remove_all(root);
}

TEST_CASE("when a review last changed answers survives writing and reading, per category") {
    RecordingInfo info;
    info.started = "2026-11-06 22:27:00";
    info.reviewed["character"] = "2026-11-07 10:48:12";
    const RecordingInfo read = parseRecordingInfo(formatRecordingInfo(info));
    REQUIRE(read.reviewed.size() == 1);
    CHECK(read.reviewed.at("character") == "2026-11-07 10:48:12");
    CHECK(parseRecordingInfo("started=2026-11-01 12:56:11\nimages=12\n").reviewed.empty()); // Older files.
    CHECK(parseRecordingInfo("reviewed-=2026-11-07 10:48:12\n").reviewed.empty());          // No category.
}

TEST_CASE("setting one value of a recording file keeps every other line as it is") {
    const std::string old = "# observex recording\nstarted=2026-11-01 12:56:11\nevery=3\nimages=12\n"; // No kind.
    const std::string added = setRecordingValue(old, "reviewed-character", "2026-11-07 10:48:12");
    CHECK(added == old + "reviewed-character=2026-11-07 10:48:12\n");
    // Set again: the line is replaced where it is.
    const std::string later =
        setRecordingValue(added + "screen=Odyssey G61SH\n", "reviewed-character", "2026-11-07 11:02:40");
    CHECK(later == old + "reviewed-character=2026-11-07 11:02:40\nscreen=Odyssey G61SH\n");
    // A last line without its line end, and a key that only starts the same.
    CHECK(setRecordingValue("images=12", "reviewed-character", "x") == "images=12\nreviewed-character=x\n");
    CHECK(setRecordingValue("reviewed-characters=y\n", "reviewed-character", "x") ==
          "reviewed-characters=y\nreviewed-character=x\n");
    CHECK(setRecordingValue("", "reviewed-character", "x") == "reviewed-character=x\n");
}

TEST_CASE("a review's time is saved into the recording's file, which still reads as before") {
    const std::filesystem::path folder =
        std::filesystem::temp_directory_path() /
        ("observex-tests-reviewed-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(folder);
    CHECK_FALSE(writeReviewed(folder, "character", "2026-11-07 10:48:12")); // Not a recording: no file.
    CHECK_FALSE(std::filesystem::exists(folder / kRecordingFile));

    const std::string old = "# observex recording\nstarted=2026-11-01 12:56:11\nseconds=34\nunsure=1\nimages=12\n";
    std::ofstream(folder / kRecordingFile, std::ios::binary) << old;
    REQUIRE(writeReviewed(folder, "character", "2026-11-07 10:48:12"));
    const RecordingInfo read = readRecordingInfo(folder);
    CHECK(read.started == "2026-11-01 12:56:11");
    CHECK(read.seconds == 34);
    CHECK(read.kind == RecordKind::Unsure);
    CHECK(read.images == 12);
    CHECK(read.reviewed.at("character") == "2026-11-07 10:48:12");
    {
        std::ifstream in(folder / kRecordingFile, std::ios::binary);
        const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        CHECK(text == old + "reviewed-character=2026-11-07 10:48:12\n");
    }
    CHECK(readRecordingInfo(folder / "missing").started.empty());
    std::filesystem::remove_all(folder);
}

TEST_CASE("a recording's images are its jpg, jpeg and png files in name order") {
    const std::filesystem::path folder =
        std::filesystem::temp_directory_path() /
        ("observex-tests-images-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(folder);
    for (const char* name : {"000002.jpg", "000001.JPG", "000003.jpeg", "000004.png", "notes.txt", "000005.jpg.tmp"}) {
        std::ofstream(folder / name) << "x";
    }
    CHECK(listImages(folder) == std::vector<std::string>{"000001.JPG", "000002.jpg", "000003.jpeg", "000004.png"});
    CHECK(listImages(folder / "missing").empty());
    std::filesystem::remove_all(folder);
}
