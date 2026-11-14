#include "core/Log.h"

#include <cstdio>
#include <iterator>
#include <system_error>

namespace observex {

namespace {

// The file's first line, written whenever it is made or written again.
std::string header() {
    char text[160];
    std::snprintf(text, sizeof(text),
                  "# observex log: what observex did, oldest first (core/Log); the last %zu,%03zu lines are kept.\n",
                  LogBook::kKeep / 1000, LogBook::kKeep % 1000);
    return text;
}

bool isDigit(char c) { return c >= '0' && c <= '9'; }

} // namespace

std::string logTime(int year, int month, int day, int hour, int minute, int second, int millisecond) {
    char text[32];
    std::snprintf(text, sizeof(text), "%04d-%02d-%02d %02d:%02d:%02d.%03d", year, month, day, hour, minute, second,
                  millisecond);
    return text;
}

void appendLogLine(std::string& out, const LogLine& line) {
    out += line.when;
    out += "  ";
    for (const char c : line.text) {
        out += c == '\n' || c == '\r' ? ' ' : c;
    }
    out += '\n';
}

bool readLogLine(std::string_view line, LogLine& out) {
    while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) {
        line.remove_suffix(1);
    }
    // "2026-11-13 00:06:01.234", then two spaces.
    constexpr std::string_view kShape = "0000-00-00 00:00:00.000";
    if (line.size() < kShape.size() + 2 || line.substr(kShape.size(), 2) != "  ") {
        return false;
    }
    for (std::size_t i = 0; i < kShape.size(); ++i) {
        if (kShape[i] == '0' ? !isDigit(line[i]) : line[i] != kShape[i]) {
            return false;
        }
    }
    out.when = std::string(line.substr(0, kShape.size()));
    out.text = std::string(line.substr(kShape.size() + 2));
    return true;
}

LogBook::LogBook(std::filesystem::path file) : file_(std::move(file)) {
    if (file_.empty()) {
        return;
    }
    std::error_code error;
    if (std::filesystem::exists(file_, error)) {
        std::string text;
        { // Closed before it may be written again.
            std::ifstream in(file_, std::ios::binary);
            text.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
            if (!in.is_open()) {
                error_ = "Could not read the log file";
            }
        }
        LogLine line;
        for (std::size_t at = 0; at < text.size();) {
            std::size_t end = text.find('\n', at);
            end = end == std::string::npos ? text.size() : end;
            if (readLogLine(std::string_view(text).substr(at, end - at), line)) {
                lines_.push_back(std::move(line));
            }
            at = end + 1;
        }
        added_ = lines_.size();
        if (lines_.size() >= kTrimAt) { // As add() would have.
            lines_.erase(lines_.begin(), lines_.end() - static_cast<std::ptrdiff_t>(kKeep));
            rewrite();
            return;
        }
    }
    openForAdding();
}

void LogBook::add(LogLine line) {
    if (out_.is_open()) {
        std::string text;
        appendLogLine(text, line);
        out_.write(text.data(), static_cast<std::streamsize>(text.size()));
        out_.flush();
        if (!out_) {
            error_ = "Could not write the log file";
            out_.clear(); // Tried again with the next line.
        }
    }
    lines_.push_back(std::move(line));
    ++added_;
    if (lines_.size() >= kTrimAt) {
        lines_.erase(lines_.begin(), lines_.end() - static_cast<std::ptrdiff_t>(kKeep));
        rewrite();
    }
}

void LogBook::clear() {
    lines_.clear();
    rewrite();
}

void LogBook::rewrite() {
    if (file_.empty()) {
        return;
    }
    out_.close();
    // Into a new file first, then over the old one: a crash meanwhile keeps the old.
    std::filesystem::path temporary = file_;
    temporary += L".tmp";
    {
        std::string text = header();
        for (const LogLine& line : lines_) {
            appendLogLine(text, line);
        }
        std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
        out.write(text.data(), static_cast<std::streamsize>(text.size()));
        if (!out) {
            error_ = "Could not write the log file";
        }
    }
    std::error_code error;
    std::filesystem::rename(temporary, file_, error);
    if (error) {
        error_ = "Could not write the log file: " + error.message();
    }
    openForAdding();
}

void LogBook::openForAdding() {
    std::error_code error;
    const bool fresh = !std::filesystem::exists(file_, error) || std::filesystem::file_size(file_, error) == 0;
    out_.open(file_, std::ios::binary | std::ios::app);
    if (!out_.is_open()) {
        error_ = "Could not open the log file";
        return;
    }
    if (fresh) {
        const std::string text = header();
        out_.write(text.data(), static_cast<std::streamsize>(text.size()));
        out_.flush();
    }
}

} // namespace observex
