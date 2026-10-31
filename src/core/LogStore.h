#pragma once

#include "core/LogEntry.h"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <vector>

namespace observex {

// Thread-safe, bounded store of log entries. Any thread can add entries. Readers ask for what
// is new since the last sequence number they saw, so each frame only copies new data.
class LogStore {
public:
    static constexpr std::size_t kDefaultCapacity = 200'000;

    explicit LogStore(std::size_t capacity = kDefaultCapacity);

    // Adds an entry and returns the sequence number it was given. Drops the oldest entry when full.
    std::uint64_t push(LogEntry entry);

    // Appends every stored entry with seq > afterSeq to `out`, oldest first. Returns how many it appended.
    std::size_t copySince(std::uint64_t afterSeq, std::vector<LogEntry>& out) const;

    // Removes all entries. Sequence numbers keep counting up, so a number is never reused.
    void clear();

    std::size_t size() const;
    std::size_t capacity() const { return capacity_; }

    // Sequence number of the newest entry ever added, or 0 if nothing was added yet.
    std::uint64_t lastSeq() const;

private:
    mutable std::mutex mutex_;
    std::deque<LogEntry> entries_; // Oldest first. Sequence numbers are consecutive.
    const std::size_t capacity_;
    std::uint64_t nextSeq_ = 1;
};

} // namespace observex
