#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

namespace observex {

// observex's log (the user asked, 2026-11-13: a minimal logger, the time of every message, saved to a file and into the
// last 10 s, back after a restart, a ring buffer): what observex did, a line each, e.g. observing started, a
// recording started, the last 10 s saved, XP got. The Log tab shows it.

// One line: when, in local time ("2026-11-13 00:06:01.234", logTime), and what.
struct LogLine {
    std::string when;
    std::string text;
};

// "2026-11-13 00:06:01.234".
std::string logTime(int year, int month, int day, int hour, int minute, int second, int millisecond);

// A line as the log file has it: WHEN, two spaces, TEXT, a line end (line ends inside TEXT become spaces), e.g.
//   2026-11-13 00:06:01.234  Observing Odyssey G61SH
void appendLogLine(std::string& out, const LogLine& line);
// One such line back (with or without its line end); false if it is not one (a "#" line, or malformed).
bool readLogLine(std::string_view line, LogLine& out);

// The log: its last lines, kept in a file too. Each line added goes to the file's end at once (flushed, so a crash
// loses none); at start the file is read back. A ring buffer: once it holds kTrimAt lines, the oldest go, down to
// kKeep, and the file is written again with those. UI thread only.
class LogBook {
public:
    static constexpr std::size_t kKeep = 10000;
    static constexpr std::size_t kTrimAt = kKeep + 1000;

    // Reads `file`'s lines (none if it does not exist yet; the last kKeep if it holds kTrimAt or more) and adds to it
    // from now on. An empty path keeps the lines in memory only.
    explicit LogBook(std::filesystem::path file);

    LogBook(const LogBook&) = delete;
    LogBook& operator=(const LogBook&) = delete;

    void add(LogLine line);
    // No lines any more, in the file too (the Log tile's Clear, the user asked, 2026-11-13); added to again from now.
    void clear();
    // Oldest first.
    const std::vector<LogLine>& lines() const { return lines_; }
    // Lines ever added (those read at start too): grows with every add, also once the oldest have gone.
    std::uint64_t added() const { return added_; }
    // Why the file could not be read or written (the lines are kept in memory all the same); empty while it can.
    const std::string& error() const { return error_; }

private:
    // The file again with lines_, then adding to it.
    void rewrite();
    void openForAdding();

    std::filesystem::path file_;
    std::vector<LogLine> lines_;
    std::uint64_t added_ = 0;
    std::ofstream out_;
    std::string error_;
};

} // namespace observex
