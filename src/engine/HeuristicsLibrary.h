#pragma once

#include "core/Depth.h"
#include "core/Ground.h"
#include "core/Heuristics.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

struct ObservexDepthApi;
struct ObservexHeuristicsApi;

namespace observex {

// One observex_heuristics.dll (src/heuristics built on its own: heuristics/Abi), loaded from a copy in the temp
// folder, so the file itself can be rebuilt while the app runs.
class HeuristicsLibrary : public std::enable_shared_from_this<HeuristicsLibrary> {
public:
    // Loads the DLL `file`. Throws with why it cannot (missing, not a DLL, or built for another observex: what they
    // hand each other differs).
    static std::shared_ptr<HeuristicsLibrary> load(const std::filesystem::path& file);
    // Unloads it and deletes the copy, once nothing made from it is left.
    ~HeuristicsLibrary();

    HeuristicsLibrary(const HeuristicsLibrary&) = delete;
    HeuristicsLibrary& operator=(const HeuristicsLibrary&) = delete;

    // New heuristics with nothing followed yet; they keep the DLL loaded while they live. Throws if it makes none.
    std::unique_ptr<Heuristics> make() const;
    // New snapping, likewise.
    std::unique_ptr<Snapping> makeSnapping() const;
    // New following of the camera (the ground), likewise.
    std::unique_ptr<Grounding> makeGround() const;
    // New shading of depth (core/Depth's Depthing), likewise; the one built into the app if the DLL has none for this
    // app (heuristics/DepthAbi).
    std::unique_ptr<Depthing> makeDepth() const;

    const std::filesystem::path& file() const { return file_; }
    // When the file was written (built), as it was loaded.
    std::filesystem::file_time_type written() const { return written_; }

private:
    HeuristicsLibrary() = default;

    void* module_ = nullptr; // Its HMODULE.
    const ObservexHeuristicsApi* api_ = nullptr;
    const ObservexDepthApi* depthApi_ = nullptr; // Null: none for this app.
    std::filesystem::path file_;
    std::filesystem::path copy_;
    std::filesystem::file_time_type written_{};
};

// The heuristics the app goes by, live (engine/Recognizer, engine/BoxSnapper) and on the clip screen (Today's
// heuristics):
// observex_heuristics.dll next to the exe, loaded again whenever it is rebuilt (the user asked, 2026-11-11: to see
// a heuristics change on an open clip without closing the app); the ones built into the app (heuristics/Rules, as of
// the app's own build) while there is no DLL or it did not load.
class HeuristicsSource {
public:
    // Loads `file` if it can. Never throws.
    explicit HeuristicsSource(std::filesystem::path file);
    // Only the ones built in (the bench's live run).
    HeuristicsSource() = default;

    // For the UI thread, on every frame: looks whether the file changed (at most every kPollMs), and loads it once
    // it has stayed the same kSettleMs (the linker writes it in pieces). True when it loaded a new one.
    bool poll();

    // New heuristics with nothing followed yet, from the newest loaded. Any thread; throws if the DLL makes none.
    std::unique_ptr<Heuristics> make() const;
    // New snapping (core/Heuristics' Snapping), likewise.
    std::unique_ptr<Snapping> makeSnapping() const;
    // New following of the camera (core/Ground's Grounding), likewise.
    std::unique_ptr<Grounding> makeGround() const;
    // New shading of depth (core/Depth's Depthing), likewise.
    std::unique_ptr<Depthing> makeDepth() const;
    // One more each time poll loads a new one, so a user that keeps heuristics (the recogniser) makes new ones.
    // Any thread.
    std::uint64_t version() const { return version_.load(std::memory_order_acquire); }

    // UI thread: which are used ("observex_heuristics.dll, built 13:05:22", "built into the app"), and why the newest
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
    std::shared_ptr<HeuristicsLibrary> library_; // Under mutex_; null: the ones built into the app.
    std::atomic<std::uint64_t> version_{0};
    std::string error_;
    std::optional<Stamp> tried_;   // The file as last loaded, or tried.
    std::optional<Stamp> pending_; // A change seen, since pendingSince_.
    std::chrono::steady_clock::time_point pendingSince_{};
    std::chrono::steady_clock::time_point lastPoll_{};
};

} // namespace observex
