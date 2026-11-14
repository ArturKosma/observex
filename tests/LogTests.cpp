#include "core/ClipGuesses.h"
#include "core/ClipLines.h"
#include "core/Log.h"
#include "core/XpWatch.h"

#include <doctest/doctest.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

using namespace observex;

namespace {

// A fresh folder in the temp folder, removed when done.
struct TempFolder {
    std::filesystem::path path;
    TempFolder() {
        path = std::filesystem::temp_directory_path() / "observex-log-tests";
        std::filesystem::remove_all(path);
        std::filesystem::create_directories(path);
    }
    ~TempFolder() {
        std::error_code error;
        std::filesystem::remove_all(path, error);
    }
};

std::string readFile(const std::filesystem::path& file) {
    std::ifstream in(file, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

std::size_t countLines(const std::string& text, char first) {
    std::size_t count = 0;
    std::size_t at = 0;
    while (at < text.size()) {
        count += text[at] == first ? 1 : 0;
        const std::size_t end = text.find('\n', at);
        if (end == std::string::npos) {
            break;
        }
        at = end + 1;
    }
    return count;
}

GameGauge xpGauge(const char* text) {
    GameGauge gauge;
    gauge.seen = true;
    std::snprintf(gauge.text.data(), gauge.text.size(), "%s", text);
    return gauge;
}

} // namespace

TEST_CASE("a log line reads back as written, its line ends as spaces") {
    CHECK(logTime(2026, 11, 3, 0, 6, 1, 34) == "2026-11-03 00:06:01.034");
    std::string text;
    appendLogLine(text, {"2026-11-13 00:06:01.234", "Observing Odyssey G61SH\nnext"});
    CHECK(text == "2026-11-13 00:06:01.234  Observing Odyssey G61SH next\n");
    LogLine line;
    REQUIRE(readLogLine(text, line));
    CHECK(line.when == "2026-11-13 00:06:01.234");
    CHECK(line.text == "Observing Odyssey G61SH next");
    CHECK_FALSE(readLogLine("# observex log", line));
    CHECK_FALSE(readLogLine("2026-11-13 00:06:01  short time", line));
    CHECK_FALSE(readLogLine("2026-11-13 00:06:01.234 one space", line));
}

TEST_CASE("the log is read back after a restart") {
    TempFolder folder;
    const std::filesystem::path file = folder.path / "observex.log";
    {
        LogBook log(file);
        CHECK(log.lines().empty());
        log.add({"2026-11-13 00:00:01.000", "observex started"});
        log.add({"2026-11-13 00:00:02.500", "Observing Odyssey G61SH"});
        CHECK(log.error().empty());
    }
    CHECK(readFile(file).starts_with("# observex log"));
    LogBook again(file);
    REQUIRE(again.lines().size() == 2);
    CHECK(again.lines()[1].when == "2026-11-13 00:00:02.500");
    CHECK(again.lines()[1].text == "Observing Odyssey G61SH");
    again.add({"2026-11-13 00:00:03.000", "Stopped observing"});
    CHECK(LogBook(file).lines().size() == 3);
}

TEST_CASE("the log is a ring: the oldest lines go, in the file too") {
    TempFolder folder;
    const std::filesystem::path file = folder.path / "observex.log";
    {
        LogBook log(file);
        for (std::size_t i = 0; i < LogBook::kTrimAt + 5; ++i) {
            log.add({"2026-11-13 00:00:00.000", "line " + std::to_string(i)});
        }
        // Trimmed once at kTrimAt, then 5 more.
        CHECK(log.lines().size() == LogBook::kKeep + 5);
        CHECK(log.lines().front().text == "line " + std::to_string(LogBook::kTrimAt - LogBook::kKeep));
        CHECK(log.lines().back().text == "line " + std::to_string(LogBook::kTrimAt + 4));
        CHECK(log.added() == LogBook::kTrimAt + 5);
        CHECK(log.error().empty());
    }
    const std::string text = readFile(file);
    CHECK(countLines(text, '#') == 1);
    CHECK(countLines(text, '2') == LogBook::kKeep + 5);
    CHECK_FALSE(std::filesystem::exists(folder.path / "observex.log.tmp"));
    LogBook again(file);
    CHECK(again.lines().size() == LogBook::kKeep + 5);
    CHECK(again.lines().back().text == "line " + std::to_string(LogBook::kTrimAt + 4));
}

TEST_CASE("a cleared log has no lines, in the file too, and is added to again") {
    TempFolder folder;
    const std::filesystem::path file = folder.path / "observex.log";
    LogBook log(file);
    log.add({"2026-11-13 00:00:01.000", "observex started"});
    log.add({"2026-11-13 00:00:02.000", "Observing Odyssey G61SH"});
    log.clear();
    CHECK(log.lines().empty());
    CHECK(log.error().empty());
    CHECK(LogBook(file).lines().empty());
    CHECK(countLines(readFile(file), '#') == 1);
    log.add({"2026-11-13 00:00:03.000", "Target Changed: None"});
    const LogBook again(file);
    REQUIRE(again.lines().size() == 1);
    CHECK(again.lines()[0].text == "Target Changed: None");
}

TEST_CASE("a log file as long as the ring's end is cut at start") {
    TempFolder folder;
    const std::filesystem::path file = folder.path / "observex.log";
    {
        std::ofstream out(file, std::ios::binary);
        for (std::size_t i = 0; i < LogBook::kTrimAt + 20; ++i) {
            out << "2026-11-13 00:00:00.000  line " << i << "\n";
        }
    }
    LogBook log(file);
    CHECK(log.lines().size() == LogBook::kKeep);
    CHECK(log.lines().front().text == "line " + std::to_string(LogBook::kTrimAt + 20 - LogBook::kKeep));
    CHECK(log.error().empty());
    const std::string text = readFile(file);
    CHECK(countLines(text, '#') == 1);
    CHECK(countLines(text, '2') == LogBook::kKeep);
    // Added to from now on.
    log.add({"2026-11-13 00:00:01.000", "next"});
    CHECK(LogBook(file).lines().back().text == "next");
}

TEST_CASE("XP got, lost and a level up, each once it was read twice") {
    XpWatch watch;
    const GameGauge a = xpGauge("26.64%");
    const GameGauge b = xpGauge("26.76%");
    CHECK_FALSE(watch.read(&a, 1.0)); // Starts it.
    CHECK_FALSE(watch.read(&a, 1.1));
    CHECK_FALSE(watch.read(&b, 1.2)); // Once: maybe a misread.
    const auto got = watch.read(&b, 1.3);
    REQUIRE(got);
    CHECK(got->text == "Got 0.12% XP: 26.64% to 26.76%");
    CHECK(got->seconds == doctest::Approx(1.2));
    CHECK_FALSE(watch.read(&b, 1.4));

    // A misread on one reading only says nothing.
    const GameGauge misread = xpGauge("28.76%");
    CHECK_FALSE(watch.read(&misread, 1.5));
    CHECK_FALSE(watch.read(&b, 1.6));
    CHECK_FALSE(watch.read(&b, 1.7));

    const GameGauge lost = xpGauge("25.56%");
    CHECK_FALSE(watch.read(&lost, 2.0));
    const auto lostChange = watch.read(&lost, 2.1);
    REQUIRE(lostChange);
    CHECK(lostChange->text == "Lost 1.20% XP: 26.76% to 25.56%");

    const GameGauge high = xpGauge("99.80%");
    const GameGauge next = xpGauge("0.12%");
    CHECK_FALSE(watch.read(&high, 3.0));
    CHECK(watch.read(&high, 3.1));
    CHECK_FALSE(watch.read(&next, 3.2));
    const auto level = watch.read(&next, 3.3);
    REQUIRE(level);
    CHECK(level->text == "Level up, got 0.32% XP: 99.80% to 0.12%");
}

TEST_CASE("XP not read says nothing, and long unseen starts again") {
    XpWatch watch;
    const GameGauge a = xpGauge("10.00%");
    const GameGauge b = xpGauge("50.00%");
    GameGauge unread = xpGauge("");
    CHECK_FALSE(watch.read(nullptr, 0.0));
    CHECK_FALSE(watch.read(&unread, 0.1));
    CHECK_FALSE(watch.read(&a, 0.2));
    CHECK_FALSE(watch.read(&a, 0.3));
    CHECK_FALSE(watch.read(&unread, 0.4)); // A window over it: no number.
    CHECK_FALSE(watch.read(&a, 0.5));
    // Another character after the select screen: its XP only starts it.
    CHECK_FALSE(watch.read(&b, 0.5 + XpWatch::kForgetSeconds + 1.0));
    CHECK_FALSE(watch.read(&b, 0.6 + XpWatch::kForgetSeconds + 1.0));
}

TEST_CASE("a clip's log lines read back with their times") {
    std::string text;
    appendNote(text, 12.5, "recording started");
    appendLog(text, 812.3, {"2026-11-13 00:06:01.234", "Saved last 10 s as clip 2026-11-13_00-06-01"});
    CHECK(text.ends_with("log 812.3 2026-11-13 00:06:01.234  Saved last 10 s as clip 2026-11-13_00-06-01\n"));
    const ClipText clip = readClipText(text);
    REQUIRE(clip.logs.size() == 1);
    CHECK(clip.logMs[0] == doctest::Approx(812.3));
    CHECK(clip.logs[0].when == "2026-11-13 00:06:01.234");
    CHECK(clip.logs[0].text == "Saved last 10 s as clip 2026-11-13_00-06-01");
    CHECK(clip.notes.size() == 1);
}
