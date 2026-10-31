#include "core/LogStore.h"

#include <doctest/doctest.h>

#include <string>
#include <thread>
#include <utility>
#include <vector>

using namespace observex;

namespace {

LogEntry makeEntry(std::string message) {
    LogEntry entry;
    entry.source = "test";
    entry.message = std::move(message);
    return entry;
}

} // namespace

TEST_CASE("push gives increasing sequence numbers") {
    LogStore store(10);
    CHECK(store.lastSeq() == 0u);
    CHECK(store.push(makeEntry("a")) == 1u);
    CHECK(store.push(makeEntry("b")) == 2u);
    CHECK(store.size() == 2u);
    CHECK(store.lastSeq() == 2u);
}

TEST_CASE("copySince returns only newer entries, oldest first") {
    LogStore store(10);
    store.push(makeEntry("a"));
    store.push(makeEntry("b"));
    store.push(makeEntry("c"));

    std::vector<LogEntry> out;
    CHECK(store.copySince(1, out) == 2u);
    REQUIRE(out.size() == 2u);
    CHECK(out[0].message == "b");
    CHECK(out[1].message == "c");

    out.clear();
    CHECK(store.copySince(3, out) == 0u);
    CHECK(out.empty());
}

TEST_CASE("copySince appends to what is already in the output") {
    LogStore store(10);
    store.push(makeEntry("a"));
    std::vector<LogEntry> out{makeEntry("existing")};
    CHECK(store.copySince(0, out) == 1u);
    REQUIRE(out.size() == 2u);
    CHECK(out[0].message == "existing");
    CHECK(out[1].message == "a");
}

TEST_CASE("a full store drops its oldest entries") {
    LogStore store(3);
    for (int i = 1; i <= 5; ++i) {
        store.push(makeEntry(std::to_string(i)));
    }
    CHECK(store.size() == 3u);

    std::vector<LogEntry> out;
    store.copySince(0, out);
    REQUIRE(out.size() == 3u);
    CHECK(out.front().seq == 3u);
    CHECK(out.front().message == "3");
    CHECK(out.back().seq == 5u);

    // A reader that fell behind gets everything still stored.
    out.clear();
    CHECK(store.copySince(1, out) == 3u);
}

TEST_CASE("clear keeps sequence numbers counting") {
    LogStore store(10);
    store.push(makeEntry("a"));
    store.push(makeEntry("b"));
    store.clear();
    CHECK(store.size() == 0u);
    CHECK(store.lastSeq() == 2u);

    CHECK(store.push(makeEntry("after")) == 3u);
    std::vector<LogEntry> out;
    CHECK(store.copySince(2, out) == 1u);
    REQUIRE(out.size() == 1u);
    CHECK(out[0].message == "after");
}

TEST_CASE("capacity is at least one") {
    LogStore store(0);
    CHECK(store.capacity() == 1u);
    store.push(makeEntry("a"));
    store.push(makeEntry("b"));
    CHECK(store.size() == 1u);
}

TEST_CASE("concurrent writers lose no entries") {
    constexpr std::size_t kThreads = 4;
    constexpr std::size_t kPerThread = 5000;
    LogStore store(kThreads * kPerThread);
    {
        std::vector<std::jthread> writers;
        for (std::size_t t = 0; t < kThreads; ++t) {
            writers.emplace_back([&store] {
                for (std::size_t i = 0; i < kPerThread; ++i) {
                    store.push(makeEntry("x"));
                }
            });
        }
    } // jthreads join here
    CHECK(store.size() == kThreads * kPerThread);
    CHECK(store.lastSeq() == kThreads * kPerThread);

    std::vector<LogEntry> out;
    store.copySince(0, out);
    REQUIRE(out.size() == kThreads * kPerThread);
    for (std::size_t i = 0; i < out.size(); ++i) {
        REQUIRE(out[i].seq == i + 1);
    }
}
