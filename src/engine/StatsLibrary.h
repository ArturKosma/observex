#pragma once

#include "core/GameStats.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

struct ObservexStatsApi;

namespace observex {

// One observex_stats.dll (heuristics/StatsAbi), loaded from a copy in the temp folder, so the file itself can be
// rebuilt while the app runs.
class StatsLibrary : public std::enable_shared_from_this<StatsLibrary> {
public:
    // Loads the DLL `file`. Throws with why it cannot (missing, not a DLL, or built for another observex).
    static std::shared_ptr<StatsLibrary> load(const std::filesystem::path& file);
    // Unloads it and deletes the copy, once nothing made from it is left.
    ~StatsLibrary();

    StatsLibrary(const StatsLibrary&) = delete;
    StatsLibrary& operator=(const StatsLibrary&) = delete;

    // A new reader by `folder`'s stats.txt (models/ui); it keeps the DLL loaded while it lives. One that cannot read
    // stats.txt reads nothing, and *error says why (empty when it reads). Throws if the DLL makes none.
    std::unique_ptr<StatsReading> make(const std::filesystem::path& folder, std::string* error) const;

    const std::filesystem::path& file() const { return file_; }
    // When the file was written (built), as it was loaded.
    std::filesystem::file_time_type written() const { return written_; }

private:
    StatsLibrary() = default;

    void* module_ = nullptr; // Its HMODULE.
    const ObservexStatsApi* api_ = nullptr;
    std::filesystem::path file_;
    std::filesystem::path copy_;
    std::filesystem::file_time_type written_{};
};

// How the app reads the game's numbers, live (engine/UiFinder) and on the clip screen (Today, next to what was read
// while it was recorded): observex_stats.dll next to the exe, loaded again whenever it is rebuilt (the user asked,
// 2026-11-12: "make sure the info is a separate dll so we can hot reload with new heuristics and compare to
// recorded"; build only that target, `--target observex_stats`, as the app's exe is locked while it runs); the rules
// built into the app (heuristics/StatsReader, as of the app's own build) while there is no DLL or it did not load.
class StatsSource {
public:
    // Loads `file` if it can. Never throws.
    explicit StatsSource(std::filesystem::path file);
    // Only the rules built in.
    StatsSource() = default;

    // For the UI thread, on every frame: looks whether the file changed (at most every kPollMs), and loads it once it
    // has stayed the same kSettleMs (the linker writes it in pieces). True when it loaded a new one.
    bool poll();

    // A new reader by `folder`'s stats.txt, from the newest loaded (else the rules built in); *error as
    // StatsLibrary::make's. Any thread; never throws (a DLL that makes none: the rules built in, and *error says so).
    std::unique_ptr<StatsReading> make(const std::filesystem::path& folder, std::string* error) const;
    // One more each time poll loads a new one, so a user that keeps a reader (the UI finder) makes a new one. Any
    // thread.
    std::uint64_t version() const { return version_.load(std::memory_order_acquire); }

    // UI thread: which rules read ("observex_stats.dll, built 13:05:22", "built into the app"), and why the newest
    // file did not load (empty when it did).
    std::string describe() const;
    const std::string& error() const { return error_; }

    static constexpr auto kPollMs = std::chrono::milliseconds(250);
    static constexpr auto kSettleMs = std::chrono::milliseconds(400);

private:
    struct Stamp {
        std::filesystem::file_time_type written;
        std::uintmax_t bytes = 0;
        bool operator==(const Stamp&) const = default;
    };
    std::optional<Stamp> stampOf() const;
    // Loads the file; false (and error_) if it cannot.
    bool load(Stamp stamp);

    std::filesystem::path file_;
    mutable std::mutex mutex_;
    std::shared_ptr<StatsLibrary> library_; // Under mutex_; null: the rules built into the app.
    std::atomic<std::uint64_t> version_{0};
    std::string error_;
    std::optional<Stamp> tried_;   // The file as last loaded, or tried.
    std::optional<Stamp> pending_; // A change seen, since pendingSince_.
    std::chrono::steady_clock::time_point pendingSince_{};
    std::chrono::steady_clock::time_point lastPoll_{};
};

} // namespace observex
