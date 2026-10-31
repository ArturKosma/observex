#pragma once

#include <atomic>
#include <stop_token>
#include <thread>

namespace observex {

class LogStore;

// Background thread that writes made-up log entries, so the UI has live data before real sources exist.
class DemoSource {
public:
    static constexpr float kMinRate = 1.0f;
    static constexpr float kMaxRate = 5000.0f;

    explicit DemoSource(LogStore& store);
    ~DemoSource();

    DemoSource(const DemoSource&) = delete;
    DemoSource& operator=(const DemoSource&) = delete;

    // start() and stop() are meant to be called from one thread (the UI thread).
    void start();
    void stop();
    bool running() const { return thread_.joinable(); }

    // Entries per second, clamped to [kMinRate, kMaxRate]. Takes effect right away.
    void setRate(float entriesPerSecond);
    float rate() const { return rate_.load(); }

private:
    void run(std::stop_token stop);

    LogStore& store_;
    std::atomic<float> rate_{20.0f};
    std::jthread thread_;
};

} // namespace observex
