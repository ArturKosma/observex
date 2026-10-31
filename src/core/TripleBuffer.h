#pragma once

#include <array>
#include <atomic>
#include <cstdint>

namespace observex {

// Hands the newest value from one writer thread to one reader thread. Neither side ever waits or locks:
// each side owns one of three slots, and the third (the hand-over slot) is swapped with one atomic exchange.
// The reader always gets the newest published value; values it was too slow to fetch are skipped.
template <typename T> class TripleBuffer {
public:
    // ---- Writer thread ----

    // The slot to fill before publish().
    T& writeSlot() { return slots_[writeIndex_]; }
    int writeIndex() const { return writeIndex_; }

    // Makes the write slot the newest value and takes back a slot to write next.
    // Returns true when the slot taken back holds a value the reader never fetched.
    bool publish() {
        const std::uint8_t previous =
            handover_.exchange(static_cast<std::uint8_t>(writeIndex_ | kFresh), std::memory_order_acq_rel);
        writeIndex_ = static_cast<std::uint8_t>(previous & kIndexMask);
        return (previous & kFresh) != 0;
    }

    // ---- Reader thread ----

    // True when a value newer than the last fetched one is waiting.
    bool hasNew() const { return (handover_.load(std::memory_order_acquire) & kFresh) != 0; }

    // Takes the newest value if there is one. Returns false, keeping the current read slot, if not.
    bool fetch() {
        if (!hasNew()) {
            return false;
        }
        const std::uint8_t previous = handover_.exchange(readIndex_, std::memory_order_acq_rel);
        readIndex_ = static_cast<std::uint8_t>(previous & kIndexMask);
        return true;
    }

    // The last fetched value (a default-constructed T before the first fetch).
    const T& readSlot() const { return slots_[readIndex_]; }
    int readIndex() const { return readIndex_; }

private:
    static constexpr std::uint8_t kIndexMask = 0b011;
    static constexpr std::uint8_t kFresh = 0b100;

    std::array<T, 3> slots_{};
    std::atomic<std::uint8_t> handover_{1};
    std::uint8_t writeIndex_ = 0;
    std::uint8_t readIndex_ = 2;
};

} // namespace observex
