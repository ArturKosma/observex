#pragma once

#include <cstdio>
#include <filesystem>
#include <string>
#include <string_view>

namespace observex {

// The review log: everything the user does in a recording's reviews, with what the review showed and why (the user
// asked, 2026-11-08: to read back later, with Claude, what they mostly fixed and why, and what to change in the model
// or the review). Appended to <category>-review-log.txt in the recording's folder, across reviews; core/ReviewLines
// words the lines, the header explains them. Each line reaches the disk at once, so closing or a crash loses nothing.
// If the file cannot be written, the review goes on without it.
class ReviewLog {
public:
    explicit ReviewLog(const std::filesystem::path& file);
    ~ReviewLog();

    ReviewLog(const ReviewLog&) = delete;
    ReviewLog& operator=(const ReviewLog&) = delete;

    // Writes `text` with the date and time in front; its further lines (after "\n") go below it, indented.
    void write(std::string_view text);

    // When `file` was last written, as "2026-11-08 02:03" (local time); "unknown" if it cannot be read.
    static std::string fileTime(const std::filesystem::path& file);

private:
    std::FILE* file_ = nullptr;
};

} // namespace observex
