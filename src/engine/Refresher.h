#pragma once

#include <atomic>
#include <filesystem>
#include <stop_token>
#include <string>
#include <thread>
#include <vector>

namespace observex {

// Makes the guesses and features of the reviewed recordings fresh, from the current model, so the kind update
// (KindUpdater) can learn from them again after a new model made them stale (KindUpdater::stale). One recording at a
// time, each with a Finder, as opening its review would do (so re-making a reviewed recording's guesses is harmless).
// The app runs it while a review is open, once that review's own finder is done: live recognition pauses then, so
// the GPU is free. On its own thread, which mostly waits for the Finder; talks through atomics.
class Refresher {
public:
    // Starts on the stale reviewed recordings in `recordings` for `category` (`kinds`: its kinds' ids, in order), with
    // the model in `modelFolder`; all but `skip` (the open review's, whose own finder looks after it). Returns at once.
    Refresher(std::filesystem::path recordings, std::string category, std::vector<std::string> kinds,
              std::filesystem::path modelFolder, std::filesystem::path skip = {});
    // Stops it, and the Finder at work, which then writes nothing.
    ~Refresher();

    Refresher(const Refresher&) = delete;
    Refresher& operator=(const Refresher&) = delete;

    bool done() const { return done_.load(std::memory_order_acquire); }
    // How many recordings were stale (-1 until it has looked), and how many of them it made fresh so far.
    int stale() const { return stale_.load(std::memory_order_relaxed); }
    int refreshed() const { return refreshed_.load(std::memory_order_relaxed); }
    // Why it stopped before the end; empty if it did not. Valid once done().
    const std::string& error() const { return error_; }

private:
    void run(std::stop_token stop);

    std::filesystem::path recordings_;
    std::string category_;
    std::vector<std::string> kinds_;
    std::filesystem::path modelFolder_;
    std::filesystem::path skip_;
    std::atomic<bool> done_{false};
    std::atomic<int> stale_{-1};
    std::atomic<int> refreshed_{0};
    std::string error_;
    std::jthread thread_; // Last, so it stops before the members it uses are destroyed.
};

} // namespace observex
