#include "core/TripleBuffer.h"

#include <doctest/doctest.h>

#include <cstdint>
#include <thread>

using namespace observex;

TEST_CASE("the reader sees nothing until the writer publishes") {
    TripleBuffer<int> buffer;
    CHECK_FALSE(buffer.hasNew());
    CHECK_FALSE(buffer.fetch());
    CHECK(buffer.readSlot() == 0);

    buffer.writeSlot() = 5;
    buffer.publish();
    CHECK(buffer.hasNew());
    CHECK(buffer.fetch());
    CHECK(buffer.readSlot() == 5);
    CHECK_FALSE(buffer.fetch());
    CHECK(buffer.readSlot() == 5);
}

TEST_CASE("the reader gets only the newest value") {
    TripleBuffer<int> buffer;
    buffer.writeSlot() = 1;
    CHECK_FALSE(buffer.publish()); // The slot taken back was never published.
    buffer.writeSlot() = 2;
    CHECK(buffer.publish()); // The slot taken back still holds 1, never read.
    CHECK(buffer.fetch());
    CHECK(buffer.readSlot() == 2);
}

TEST_CASE("publish reports a slot the reader handed back as read") {
    TripleBuffer<int> buffer;
    buffer.writeSlot() = 1;
    buffer.publish();
    CHECK(buffer.fetch());
    buffer.writeSlot() = 2;
    CHECK_FALSE(buffer.publish());
}

TEST_CASE("writer and reader never share a slot") {
    TripleBuffer<int> buffer;
    for (int i = 0; i < 30; ++i) {
        buffer.writeSlot() = i;
        buffer.publish();
        if (i % 3 == 0) {
            buffer.fetch();
        }
        CHECK(buffer.writeIndex() != buffer.readIndex());
    }
}

TEST_CASE("a concurrent reader never sees a torn or older value") {
    struct Pair {
        std::uint64_t a = 0;
        std::uint64_t b = 0;
    };
    constexpr std::uint64_t kCount = 200'000;
    TripleBuffer<Pair> buffer;

    std::jthread writer([&buffer] {
        for (std::uint64_t i = 1; i <= kCount; ++i) {
            Pair& slot = buffer.writeSlot();
            slot.a = i;
            slot.b = i;
            buffer.publish();
        }
    });

    std::uint64_t last = 0;
    bool consistent = true;
    while (last < kCount) {
        if (buffer.fetch()) {
            const Pair& value = buffer.readSlot();
            consistent = consistent && value.a == value.b && value.a > last;
            last = value.a;
        }
    }
    CHECK(consistent);
}
