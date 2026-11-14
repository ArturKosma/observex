#pragma once

#include "core/GameStats.h"
#include "core/TripleBuffer.h"
#include "core/UiLayout.h"
#include "engine/Gpu.h"
#include "engine/ImageChannel.h"
#include "engine/StatsLibrary.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <stop_token>
#include <string>
#include <thread>
#include <vector>

namespace observex {

// How the UI finder is doing, refreshed once per second.
struct UiFinderStats {
    float imagesPerSecond = 0.0f;    // Images it found the interface on.
    float workAvgMs = 0.0f;          // From taking an image to having its pieces: the grey image on the GPU, read
    float workMaxMs = 0.0f;          // back, then finding.
    float findAvgMs = 0.0f;          // Of that, the finding (core/UiLayout).
    float statsAvgMs = 0.0f;         // Then reading the bars and their numbers (heuristics/StatsReader), on the CPU.
    std::array<char, 256> problem{}; // Why ui.txt is not what it finds with (see UiFinder); empty when it is.
    std::array<char, 256> statsProblem{}; // Why stats.txt cannot be read (no numbers are read); empty when it can.
};

// The miniatures (heuristics/Miniatures: skills' and items' pictures) on the whole of one image, when asked (the Stats
// tile's Add Skill): the hotbar's first, then the other pieces', then the rest. Each one's colour pixels in its own
// cell of an atlas kMax cells wide, at the cell's top left.
struct UiMiniatures {
    static constexpr int kMax = 40;
    static constexpr int kSide = 64; // The cells' side (heuristics/Miniatures finds none larger).
    static constexpr int kPitch = kMax * kSide * 4; // The atlas's row, in bytes.
    std::uint64_t request = 0;       // The ask it answers (UiFinder::findMiniatures); 0: none yet.
    std::uint64_t image = 0;
    int count = 0;
    std::array<StatsArea, kMax> boxes{};                      // On the screen.
    std::array<std::uint8_t, kPitch * kSide> bgra{};          // The atlas (ScreenCapture::kImageFormat).
};

// A skill to look for (the profile's, the Skills tab): its name, whether its cooldown is read, and its miniature as it
// looked when added, grey (core/GameStats' SkillPicture), side x side.
struct SkillToFind {
    std::string name;
    bool cooldown = true;
    int side = 0;
    std::vector<std::uint8_t> grey;
};

// Finds where the game draws its interface on the screen (core/UiLayout: the framed windows and ui.txt's pieces), all
// the time while the app observes a screen, and on the frames of an open clip (app/ClipView); the flag UI (Show, and
// the clip's Guesses) only draws what it found (the user asked, 2026-11-12: to check every piece is found, as a depth
// model is to leave them out later, and the bot will read from them). The interface moves only when the user moves
// it, so ten images a second are enough (kImagesPerSecond).
// On the same image it reads the game's bars and the numbers on them (heuristics/StatsReader, models/ui/stats.txt: own
// HP, MP and XP, the target's HP; the user asked, 2026-11-12, for a state machine and to see them in saved clips),
// through observex_stats.dll (engine/StatsLibrary: a new reader from the next image on whenever it is rebuilt):
// only the parts of the image they are in come back, in colour (a few tens of KB), so the image is held until the
// interface is found on it (an image channel's reader keeps a slot of its own: the pipeline is never held up).
// Each goes grey on the GPU (engine/GreyMaker) and only its full-size level comes back (3.7 MB for a 2560 x 1440
// screen); the finding takes 3-5 ms on the CPU (bench --ui, 2026-11-12, eight clips).
// ui.txt and its anchors are read again within a second of changing; while they cannot be read, the one before goes
// on (with none, only framed windows are found) and stats().problem says why.
//
// It works on its own thread with its own GPU device, so it never holds up capture, the recogniser or the snapper:
// the pipeline (while Pipeline::setUiEnabled is on) or the clip screen hands it images through an ImageChannel. Talks
// to the UI only through lock-free hand-overs and atomics.
class UiFinder {
public:
    enum class State : std::uint8_t {
        Starting,
        Running,
        Failed, // See error().
    };

    // How many images a second it needs: the most its feeder hands it.
    static constexpr unsigned kImagesPerSecond = 10;

    // Starts on the images of `channel`, with a device of its own on `gpu` (the channel's), with the layout in `folder`
    // (ui.txt and its anchors' pictures, e.g. models/ui), reading the numbers with readers from `stats` (null: the
    // rules built into the app). Returns at once. Throws on failure.
    UiFinder(IDXGIAdapter1* gpu, ImageChannel& channel, std::filesystem::path folder,
             std::shared_ptr<const StatsSource> stats = nullptr);
    // Stops. Destroy it before the channel.
    ~UiFinder();

    UiFinder(const UiFinder&) = delete;
    UiFinder& operator=(const UiFinder&) = delete;

    State state() const { return state_.load(std::memory_order_acquire); }
    const std::string& error() const { return error_; } // Valid once state() is Failed.

    // For one reader thread (the UI): the pieces on the newest image it looked at (UiPieces::image 0 before the
    // first). Valid until the next call.
    const UiPieces& found();
    const UiFinderStats& stats();
    // For one reader thread (the UI): what was read on the newest image it looked at (GameStats::image 0 before the
    // first). Valid until the next call.
    const GameStats& gameStats();
    // Asks for the miniatures on the next image it looks at; returns the ask's number (miniatures().request is it
    // once answered).
    std::uint64_t findMiniatures();
    // For one reader thread (the UI): the newest miniatures found. Valid until the next call.
    const UiMiniatures& miniatures();
    // The skills to read (the profile's), from the next image on. Any thread.
    void setSkills(std::vector<SkillToFind> skills);

private:
    void run(std::stop_token stop);
    void fail(std::string message);

    GpuDevice gpu_;
    std::unique_ptr<ImageReader> reader_;
    std::filesystem::path folder_;
    std::shared_ptr<const StatsSource> statsSource_;
    unsigned width_ = 0;
    unsigned height_ = 0;
    TripleBuffer<UiPieces> found_;
    TripleBuffer<GameStats> gameStats_;
    TripleBuffer<UiFinderStats> stats_;
    TripleBuffer<UiMiniatures> miniatures_;
    std::atomic<std::uint64_t> miniaturesAsked_{0};
    std::mutex skillsMutex_;
    std::vector<SkillToFind> skills_;           // Under skillsMutex_.
    std::atomic<std::uint64_t> skillsVersion_{0}; // One more on each setSkills().
    std::atomic<State> state_{State::Starting};
    std::string error_;
    std::jthread thread_; // Last, so it stops before the members it uses are destroyed.
};

} // namespace observex
