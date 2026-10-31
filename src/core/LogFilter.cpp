#include "core/LogFilter.h"

#include "core/Text.h"

namespace observex {

bool LogFilter::levelEnabled(LogLevel level) const {
    const auto index = static_cast<std::size_t>(level);
    return index < levels.size() && levels[index];
}

bool LogFilter::matches(const LogEntry& entry) const {
    if (!levelEnabled(entry.level)) {
        return false;
    }
    return containsIgnoreCase(entry.message, text) || containsIgnoreCase(entry.source, text);
}

} // namespace observex
