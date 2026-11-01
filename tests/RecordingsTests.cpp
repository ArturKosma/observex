#include "core/Recordings.h"

#include <doctest/doctest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
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
    info.images = 12;
    const RecordingInfo read = parseRecordingInfo(formatRecordingInfo(info));
    CHECK(read.started == info.started);
    CHECK(read.seconds == 34);
    CHECK(read.screen == "Odyssey G61SH");
    CHECK(read.width == 2560);
    CHECK(read.height == 1440);
    CHECK(read.every == 3);
    CHECK(read.images == 12);
}

TEST_CASE("reading a recording's info skips what it does not know") {
    const RecordingInfo read =
        parseRecordingInfo("# a comment\r\nstarted=2026-10-31 23:59:49\r\nnew-key=1\r\nnot a line\r\nimages=60\r\n");
    CHECK(read.started == "2026-10-31 23:59:49");
    CHECK(read.images == 60);
    CHECK(read.seconds == 0);
    CHECK(read.screen.empty());
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
