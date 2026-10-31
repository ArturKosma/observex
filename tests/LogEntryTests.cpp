#include "core/LogEntry.h"

#include <doctest/doctest.h>

#include <string>

using namespace observex;

TEST_CASE("levelName gives short level names") {
    CHECK(levelName(LogLevel::Trace) == "TRACE");
    CHECK(levelName(LogLevel::Info) == "INFO");
    CHECK(levelName(LogLevel::Warning) == "WARN");
    CHECK(levelName(LogLevel::Fatal) == "FATAL");
}

TEST_CASE("parseLogLevel ignores case and accepts aliases") {
    CHECK(parseLogLevel("info") == LogLevel::Info);
    CHECK(parseLogLevel("ERROR") == LogLevel::Error);
    CHECK(parseLogLevel("Warn") == LogLevel::Warning);
    CHECK(parseLogLevel("warning") == LogLevel::Warning);
    CHECK(parseLogLevel("Verbose") == LogLevel::Trace);
    CHECK(parseLogLevel("critical") == LogLevel::Fatal);
    CHECK_FALSE(parseLogLevel("loud").has_value());
    CHECK_FALSE(parseLogLevel("").has_value());
}

TEST_CASE("every level survives a round trip through its name") {
    for (int i = 0; i < kLogLevelCount; ++i) {
        const auto level = static_cast<LogLevel>(i);
        CHECK(parseLogLevel(levelName(level)) == level);
    }
}

TEST_CASE("formatTime prints HH:MM:SS.mmm") {
    const Clock::time_point time =
        Clock::time_point{} + std::chrono::hours(5) + std::chrono::seconds(7) + std::chrono::milliseconds(42);
    const std::string text = formatTime(time);
    REQUIRE(text.size() == 12u);
    CHECK(text[2] == ':');
    CHECK(text[5] == ':');
    // The hour depends on the local time zone; seconds and milliseconds do not.
    CHECK(text.substr(6) == "07.042");
}
