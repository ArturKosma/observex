#include "core/DemoSource.h"
#include "core/LogStore.h"

#include <doctest/doctest.h>

#include <chrono>
#include <thread>

using namespace observex;

TEST_CASE("demo source writes entries until stopped") {
    LogStore store;
    DemoSource demo(store);
    demo.setRate(DemoSource::kMaxRate);
    CHECK_FALSE(demo.running());

    demo.start();
    CHECK(demo.running());

    // Wait up to two seconds for the first entries, so a slow machine does not fail the test.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (store.size() == 0 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    CHECK(store.size() > 0u);

    demo.stop();
    CHECK_FALSE(demo.running());
    const auto countAfterStop = store.size();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    CHECK(store.size() == countAfterStop);
}

TEST_CASE("demo source rate is clamped") {
    LogStore store;
    DemoSource demo(store);
    demo.setRate(0.0f);
    CHECK(demo.rate() == DemoSource::kMinRate);
    demo.setRate(1e9f);
    CHECK(demo.rate() == DemoSource::kMaxRate);
}
