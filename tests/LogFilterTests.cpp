#include "core/LogFilter.h"

#include <doctest/doctest.h>

using namespace observex;

namespace {

LogEntry makeEntry(LogLevel level, const char* source, const char* message) {
    LogEntry entry;
    entry.level = level;
    entry.source = source;
    entry.message = message;
    return entry;
}

} // namespace

TEST_CASE("default filter matches everything") {
    const LogFilter filter;
    for (int i = 0; i < kLogLevelCount; ++i) {
        CHECK(filter.matches(makeEntry(static_cast<LogLevel>(i), "net", "hello")));
    }
}

TEST_CASE("switched-off levels are hidden") {
    LogFilter filter;
    filter.levels[static_cast<std::size_t>(LogLevel::Debug)] = false;
    CHECK_FALSE(filter.matches(makeEntry(LogLevel::Debug, "net", "hello")));
    CHECK(filter.matches(makeEntry(LogLevel::Info, "net", "hello")));
}

TEST_CASE("text matches the source or the message, ignoring case") {
    LogFilter filter;
    filter.text = "NET";
    CHECK(filter.matches(makeEntry(LogLevel::Info, "net", "Peer connected")));
    CHECK(filter.matches(makeEntry(LogLevel::Info, "io", "network is down")));
    CHECK_FALSE(filter.matches(makeEntry(LogLevel::Info, "io", "Loaded asset")));
}

TEST_CASE("level and text must both match") {
    LogFilter filter;
    filter.text = "asset";
    filter.levels[static_cast<std::size_t>(LogLevel::Error)] = false;
    CHECK_FALSE(filter.matches(makeEntry(LogLevel::Error, "io", "Failed to open asset")));
    CHECK(filter.matches(makeEntry(LogLevel::Info, "io", "Loaded asset")));
}

TEST_CASE("filters compare by value") {
    LogFilter a;
    LogFilter b;
    CHECK(a == b);
    b.text = "x";
    CHECK_FALSE(a == b);
}
