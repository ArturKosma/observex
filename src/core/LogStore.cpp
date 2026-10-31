#include "core/LogStore.h"

#include <algorithm>
#include <iterator>
#include <utility>

namespace observex {

LogStore::LogStore(std::size_t capacity) : capacity_(std::max<std::size_t>(capacity, 1)) {}

std::uint64_t LogStore::push(LogEntry entry) {
    std::lock_guard lock(mutex_);
    entry.seq = nextSeq_++;
    if (entries_.size() >= capacity_) {
        entries_.pop_front();
    }
    entries_.push_back(std::move(entry));
    return entries_.back().seq;
}

std::size_t LogStore::copySince(std::uint64_t afterSeq, std::vector<LogEntry>& out) const {
    std::lock_guard lock(mutex_);
    if (entries_.empty() || entries_.back().seq <= afterSeq) {
        return 0;
    }
    // Stored sequence numbers are consecutive, so the first new entry is found by offset.
    // A reader that fell behind (its entries were already dropped) gets everything still stored.
    const std::uint64_t firstSeq = entries_.front().seq;
    const std::size_t start = afterSeq < firstSeq ? 0 : static_cast<std::size_t>(afterSeq - firstSeq + 1);
    out.insert(out.end(), std::next(entries_.begin(), static_cast<std::ptrdiff_t>(start)), entries_.end());
    return entries_.size() - start;
}

void LogStore::clear() {
    std::lock_guard lock(mutex_);
    entries_.clear();
}

std::size_t LogStore::size() const {
    std::lock_guard lock(mutex_);
    return entries_.size();
}

std::uint64_t LogStore::lastSeq() const {
    std::lock_guard lock(mutex_);
    return nextSeq_ - 1;
}

} // namespace observex
