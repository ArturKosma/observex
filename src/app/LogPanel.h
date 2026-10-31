#pragma once

#include "core/LogEntry.h"
#include "core/LogFilter.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <vector>

namespace observex {

class LogStore;

// Scrolling, filterable table of log entries.
// It keeps its own copy of the entries, so drawing never holds the store's lock, and it only
// re-filters everything when the filter changes; new entries are filtered as they arrive.
class LogPanel {
public:
    explicit LogPanel(LogStore& store);

    void draw(const char* title, bool* open);

private:
    void pullNewEntries();
    void dropOldEntries();
    void refilter();
    void clear();

    void drawToolbar();
    void drawTable();

    LogStore& store_;
    const std::size_t maxEntries_;

    std::deque<LogEntry> entries_;      // Local copy, oldest first.
    std::uint64_t firstIndex_ = 0;      // Running index of entries_.front(); grows as old entries are dropped.
    std::deque<std::uint64_t> visible_; // Running indices of the entries that pass appliedFilter_.
    std::array<std::size_t, kLogLevelCount> levelCounts_{};
    std::uint64_t lastSeq_ = 0;      // Newest store entry already copied.
    std::vector<LogEntry> incoming_; // Reused buffer for each frame's new entries.

    LogFilter filter_;        // Edited by the toolbar.
    LogFilter appliedFilter_; // What visible_ currently reflects.
    bool autoScroll_ = true;
};

} // namespace observex
