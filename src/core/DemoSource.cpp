#include "core/DemoSource.h"

#include "core/LogStore.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <mutex>
#include <random>

namespace observex {

namespace {

constexpr auto kTick = std::chrono::milliseconds(10);

struct MessageTemplate {
    int weight; // How often this message is picked, relative to the others.
    LogLevel level;
    const char* source;
    const char* format; // printf format with one %d.
};

constexpr std::array<MessageTemplate, 9> kTemplates = {{
    {20, LogLevel::Trace, "physics", "Step finished, %d active bodies"},
    {15, LogLevel::Debug, "render", "Frame submitted, %d draw calls"},
    {10, LogLevel::Debug, "net", "Received packet of %d bytes"},
    {20, LogLevel::Info, "net", "Peer connected, latency %d ms"},
    {15, LogLevel::Info, "io", "Loaded asset #%d"},
    {10, LogLevel::Warning, "audio", "Buffer underrun, %d samples dropped"},
    {5, LogLevel::Warning, "render", "Frame took %d ms, over budget"},
    {4, LogLevel::Error, "io", "Failed to open asset #%d: file not found"},
    {1, LogLevel::Fatal, "core", "Out of memory allocating %d bytes"},
}};

class EntryGenerator {
public:
    EntryGenerator() : rng_(std::random_device{}()) {
        std::array<int, kTemplates.size()> weights{};
        std::transform(kTemplates.begin(), kTemplates.end(), weights.begin(),
                       [](const MessageTemplate& t) { return t.weight; });
        pick_ = std::discrete_distribution<std::size_t>(weights.begin(), weights.end());
    }

    LogEntry next() {
        const MessageTemplate& t = kTemplates[pick_(rng_)];
        char text[128];
        std::snprintf(text, sizeof(text), t.format, number_(rng_));

        LogEntry entry;
        entry.time = Clock::now();
        entry.level = t.level;
        entry.source = t.source;
        entry.message = text;
        return entry;
    }

private:
    std::mt19937 rng_;
    std::discrete_distribution<std::size_t> pick_;
    std::uniform_int_distribution<int> number_{1, 5000};
};

} // namespace

DemoSource::DemoSource(LogStore& store) : store_(store) {}

DemoSource::~DemoSource() {
    stop();
}

void DemoSource::start() {
    if (!running()) {
        thread_ = std::jthread([this](std::stop_token stop) { run(stop); });
    }
}

void DemoSource::stop() {
    if (running()) {
        thread_.request_stop();
        thread_.join();
    }
}

void DemoSource::setRate(float entriesPerSecond) {
    rate_ = std::clamp(entriesPerSecond, kMinRate, kMaxRate);
}

void DemoSource::run(std::stop_token stop) {
    std::mutex mutex;
    std::condition_variable_any wake;
    EntryGenerator generator;

    auto last = std::chrono::steady_clock::now();
    double pending = 0.0; // Entries owed so far, including the fraction carried between ticks.

    while (!stop.stop_requested()) {
        {
            // Sleeps for one tick, or returns at once when stop is requested.
            std::unique_lock lock(mutex);
            (void)wake.wait_for(lock, stop, kTick, [] { return false; });
        }

        const auto now = std::chrono::steady_clock::now();
        const double rate = rate_.load();
        pending += std::chrono::duration<double>(now - last).count() * rate;
        last = now;

        // After a long stall (e.g. a debugger break), write at most a quarter second's worth at once.
        pending = std::min(pending, std::max(1.0, rate * 0.25));
        for (; pending >= 1.0 && !stop.stop_requested(); pending -= 1.0) {
            store_.push(generator.next());
        }
    }
}

} // namespace observex
