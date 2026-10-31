#include "core/LogEntry.h"

#include "core/Text.h"

#include <array>
#include <cstdio>
#include <ctime>

namespace observex {

namespace {

constexpr std::array<std::string_view, kLogLevelCount> kLevelNames = {"TRACE", "DEBUG", "INFO",
                                                                      "WARN",  "ERROR", "FATAL"};

struct LevelAlias {
    std::string_view name;
    LogLevel level;
};

constexpr std::array<LevelAlias, 4> kLevelAliases = {{
    {"verbose", LogLevel::Trace},
    {"warning", LogLevel::Warning},
    {"err", LogLevel::Error},
    {"critical", LogLevel::Fatal},
}};

} // namespace

std::string_view levelName(LogLevel level) {
    const auto index = static_cast<std::size_t>(level);
    return index < kLevelNames.size() ? kLevelNames[index] : "?";
}

std::optional<LogLevel> parseLogLevel(std::string_view text) {
    for (std::size_t i = 0; i < kLevelNames.size(); ++i) {
        if (equalsIgnoreCase(text, kLevelNames[i])) {
            return static_cast<LogLevel>(i);
        }
    }
    for (const LevelAlias& alias : kLevelAliases) {
        if (equalsIgnoreCase(text, alias.name)) {
            return alias.level;
        }
    }
    return std::nullopt;
}

std::string formatTime(Clock::time_point time) {
    const auto sinceEpoch = std::chrono::duration_cast<std::chrono::milliseconds>(time.time_since_epoch());
    const int millis = static_cast<int>(sinceEpoch.count() % 1000);
    const std::time_t seconds = Clock::to_time_t(time);

    std::tm local{};
#if defined(_WIN32)
    localtime_s(&local, &seconds);
#else
    localtime_r(&seconds, &local);
#endif

    char buffer[16];
    std::snprintf(buffer, sizeof(buffer), "%02d:%02d:%02d.%03d", local.tm_hour, local.tm_min, local.tm_sec, millis);
    return buffer;
}

} // namespace observex
