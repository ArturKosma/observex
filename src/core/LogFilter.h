#pragma once

#include "core/LogEntry.h"

#include <array>
#include <string>

namespace observex {

// Decides which entries a view shows: the levels that are switched on, plus an optional text search.
struct LogFilter {
    static_assert(kLogLevelCount == 6, "Update the default list of enabled levels");
    std::array<bool, kLogLevelCount> levels = {true, true, true, true, true, true};

    // Matches when it appears in the entry's source or message, ignoring case. Empty matches everything.
    std::string text;

    bool levelEnabled(LogLevel level) const;
    bool matches(const LogEntry& entry) const;

    bool operator==(const LogFilter&) const = default;
};

} // namespace observex
