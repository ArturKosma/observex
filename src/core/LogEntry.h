#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace observex {

enum class LogLevel : std::uint8_t { Trace, Debug, Info, Warning, Error, Fatal };

inline constexpr int kLogLevelCount = 6;

// Short display name, e.g. "INFO" or "WARN".
std::string_view levelName(LogLevel level);

// Reads a level name in any case ("info", "Warning", "ERROR"...), plus a few common aliases.
// Returns nullopt for text it does not recognise.
std::optional<LogLevel> parseLogLevel(std::string_view text);

using Clock = std::chrono::system_clock;

struct LogEntry {
    std::uint64_t seq = 0; // Set by LogStore when the entry is added. Starts at 1.
    Clock::time_point time{};
    LogLevel level = LogLevel::Info;
    std::string source; // Where the entry came from, e.g. "net" or a file name.
    std::string message;
};

// Local wall-clock time as "HH:MM:SS.mmm".
std::string formatTime(Clock::time_point time);

} // namespace observex
