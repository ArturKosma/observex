#pragma once

#include "app/ScreenImage.h"
#include "app/Theme.h"
#include "core/ClipGuesses.h"
#include "core/Depth.h"
#include "core/GameStats.h"
#include "core/Recordings.h"
#include "engine/ClipGround.h"
#include "engine/ClipPlayer.h"
#include "engine/ClipSnapper.h"
#include "engine/DepthStore.h"
#include "engine/Gpu.h"
#include "engine/ImageChannel.h"
#include "engine/StatsLibrary.h"
#include "engine/UiFinder.h"

#include <imgui.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace observex {

// The clip screen (the user asked, 2026-11-10; guesses and the timeline 2026-11-11): a saved clip (core/Recordings,
// engine/ClipRecorder), picked in the review's list of clips, plays in the window instead of the observed screen, as
// large as fits, with what a guesser shows drawn over each frame as the live view drew it (app/Overlay): by default
// what the app showed while the clip was recorded, so the same boxes and the same mistakes as then. Under the video,
// a timeline to click or drag to any frame, with the app's notes (clip.txt's note lines) marked on it: while dragged
// the picture follows the mouse frame by frame, each one as soon as it is decoded (the user asked, 2026-11-11: to see
// what happens while dragging; first dragging back showed nothing until the mouse rested, then only keyframes, 2 a
// second of clip, came while dragging). Space plays
// and pauses, Left and Right step a frame back or on (pausing), Home and End go to the first and the last. The bar
// has the guesser (App's Guesses button) and, in its middle, the buttons (first, a frame back, play, a frame on,
// last), the time and the frame number, which can be typed over to go there (Ctrl+G). Today's guesses are made again
// whenever the app loads rebuilt heuristics (setHeuristics), so a heuristics change shows on the open clip: first the
// heuristics' own (a few milliseconds), then snapped onto the video in the background as the snapper does live
// (engine/ClipSnapper, a few seconds; the user asked, 2026-11-11: a new clip's Today and Recorded the same).
// The UI finder (engine/UiFinder) looks at the frames shown as it looks at the screen live, so the UI flag (Guesses,
// as in Show) draws the game's interface found on the clip (the user asked, 2026-11-12: the same flags in both).
// The Ground flag draws the guesser's grid (the user asked, 2026-11-12: "Recorded option should ALWAYS show what the
// player saw in live play"; fixes are tried on Today, which the DLL's hot reload makes again): Recorded's is the grid
// the app drew live (clip.txt's ground lines, since 2026-11-12), Today's the ground followed again over the video with
// today's heuristics (engine/ClipGround, on the images live followed), the first time it is asked for.
// The Depth flag draws the depth instead of the frame (the user asked, 2026-11-12: "the last 10s records the exact
// same depth the user sees so that i can replay it"): Recorded's is the map the app drew live when the screen showed
// the frame's image, its levels and colours as drawn (depth.bin, clip.txt's depth lines); Today's the same map shaded
// again by today's heuristics (setHeuristics: a change to them shows on the open clip). Where the app drew none live
// there is none.
class ClipView {
public:
    // Whose characters and grid are drawn over the video (while the Characters and Ground flags are on). Each
    // guesser's set at a frame is the one the live view would have shown when the screen showed that frame's image:
    // from the latest look done by then.
    enum class Guesser : std::uint8_t {
        Recorded, // What the app showed while recording (clip.txt's shown lines).
        Today,    // The recorded looks (what the model found then) through today's heuristics (core/Heuristics),
                  // snapped onto the video as live (core/Heuristics' Snapping).
    };

    // Opens the clip in `folder` and starts playing it: decoded on `gpu` (the UI's GPU), shown with `device` (the UI's
    // device); reads its clip.txt for the guesses, Today's through `today` (used only here) and snapped by `snapping`;
    // follows the ground with `ground` once the flag Ground asks; shades Today's depth with `depth`; finds the
    // interface with the layout in `uiFolder` (models/ui) and reads the numbers on it with readers from `stats`
    // (Today's; Recorded's are its clip-stats.txt). Never throws: if the video cannot be played, draw() says why.
    ClipView(IDXGIAdapter1* gpu, ID3D11Device* device, std::filesystem::path folder, ClipInfo info, Heuristics& today,
             std::unique_ptr<Snapping> snapping, std::unique_ptr<Grounding> ground, std::unique_ptr<Depthing> depth,
             std::filesystem::path uiFolder, std::shared_ptr<const StatsSource> stats);

    ClipView(const ClipView&) = delete;
    ClipView& operator=(const ClipView&) = delete;

    const std::filesystem::path& folder() const { return folder_; }
    const ClipInfo& info() const { return info_; }

    // Takes the newest frame into the image shown (and to the UI finder, at most UiFinder::kImagesPerSecond times a
    // second), and Today's snapped guesses once they are made. Call on every frame, outside the ImGui frame.
    void update();

    // Whether the video plays: it opened and the player did not fail.
    bool canPlay() const;
    bool playing() const;
    bool ended() const;
    // Pauses while playing, else plays (from the first frame again once it ended).
    void togglePlay();
    // Seconds into the video of the frame shown, and the video's length.
    double position() const;
    double seconds() const;
    // The frame shown, numbered from 1 as in clip.txt; 0 before the first.
    std::uint64_t frame() const { return frame_; }
    // How many frames the video has; 0 if it did not open.
    std::uint64_t frames() const;
    // Shows frame `number` (1 to frames()), playing on from it while playing.
    void seek(std::uint64_t number);
    // Pauses, and shows the frame `by` frames on (or back, below 0) from the one shown or asked for last.
    void step(int by);
    // Pauses, and shows the first frame, the last, or frame `number` (typed in the bar).
    void toFirst();
    void toLast();
    void goTo(std::uint64_t number);

    Guesser guesser() const { return guesser_; }
    void setGuesser(Guesser guesser) { guesser_ = guesser; }
    // Why there are no guesses to draw (e.g. clip.txt is missing); empty when there are.
    const std::string& guessesError() const { return textError_; }
    // A guesser's sets over the whole clip, for comparing them: null without guesses.
    const Guesses* guesses(Guesser guesser) const;
    // Makes Today's guesses again through `today` (used only here) and `snapping`, new heuristics the app loaded
    // (`about`: which, said over the video for a few seconds), follows the ground again with `ground` if it was, and
    // shades Today's depth with `depth` from now on.
    void setHeuristics(Heuristics& today, std::unique_ptr<Snapping> snapping, std::unique_ptr<Grounding> ground,
                       std::unique_ptr<Depthing> depth, std::string about);
    // Why Today's guesses could not be snapped (they are the heuristics' own); empty if they were.
    const std::string& todaySnapError() const { return snapError_; }
    // Why the UI finder finds nothing, or too little (ui.txt cannot be read); empty while it finds as live.
    std::string uiProblem();

    // The numbers read from the game's interface on the frame shown (app/StatsPanel), as recorded (clip-stats.txt:
    // read live when the screen showed its image, or the last before it; null if the clip has none) and today (the
    // UI finder's on the frames shown, read by the newest observex_stats.dll; null before the first).
    const GameStats* recordedStats() const;
    bool statsRecorded() const { return !recordedStats_.empty(); } // Saved before 2026-11-12 evening: none.
    const GameStats* todayStats();
    // Why today's numbers are not read (stats.txt cannot be read, or the finder did not start); empty when they are.
    std::string statsProblem();
    // The miniatures on the frame shown (Add Skill, engine/UiFinder): asks for them (the frame is handed to the finder
    // again), 0 if it is not running; then the newest found (null if it is not running).
    std::uint64_t findMiniatures();
    const UiMiniatures* miniatures();
    // observex_stats.dll was rebuilt (`about`: which, said over the video for a few seconds): the frame shown is read
    // again with it.
    void statsReloaded(const std::string& about);
    // The skills today's reading looks for (the profile's); the frame shown is read again.
    void setSkills(std::vector<SkillToFind> skills);
    // The characters the guesser shows on the frame shown (none before the first), and the model's kinds by number
    // (core/ClipGuesses): the Stats tile's Nearby counts.
    std::span<const Tracked> thingsShown() const;
    std::span<const std::string> kinds() const;
    // observex's log lines made while the clip was recorded (the Log tile; none in clips saved before 2026-11-13), and
    // how many of them were made by the frame shown.
    std::span<const LogLine> logLines() const { return text_->logs; }
    std::size_t logLinesBy() const;
    // Why the ground cannot be followed over the clip; empty while it is, or will be once the flag Ground asks.
    std::string groundProblem() const;
    // Why the guesser has no depth to draw over the clip; empty while it has.
    std::string depthProblem() const;

    // Draws the frame as large as fits into the rest of the current window, with the guesser's things over it and
    // the timeline under it, or why it cannot. Handles the keys above. `showCharacters`, `showKinds`, `showUi` and
    // `showGround` as the live view's Characters, Kinds, UI and Ground flags (App's Guesses popup, keys 1 and 2 pick
    // the guesser there); the ground is followed over the clip the first time it is asked for (in the background,
    // about a second for a 10 s clip), and drawn once it has been. `showDepth` as the Depth flag: depth.bin is read the
    // first time it is on.
    void draw(const theme::Fonts& fonts, bool showCharacters, bool showKinds, bool showUi, bool showGround,
              bool showDepth, float dpiScale);

private:
    // Why the video cannot be played (it did not open, or the player failed); empty while it can.
    const std::string& error() const;
    // The frame asked for last if it has not come yet, else the one shown.
    std::uint64_t current() const { return wanted_ != 0 ? wanted_ : frame_; }
    // When the screen showed frame `number`'s image, in clip.txt's milliseconds.
    double frameMs(std::uint64_t number) const;
    void drawTimeline(ImVec2 min, ImVec2 max, float dpiScale);
    // Today's guesses through `today`, then snapped by `snapping` in the background.
    void makeToday(Heuristics& today, std::unique_ptr<Snapping> snapping);
    // Starts following the ground over the clip, if it has not yet.
    void followGround();
    // Reads depth.bin, if it has not yet.
    void loadDepth();
    // The guesser's depth of the frame shown, over `min` to `max`, if it has one.
    void drawDepth(ImDrawList* drawList, ImVec2 min, ImVec2 max, float rounding);

    std::filesystem::path folder_;
    ClipInfo info_;
    std::filesystem::path uiFolder_; // The interface's layout (models/ui).
    ComPtr<ID3D11DeviceContext> context_;
    // Declared in this order so they go in the reverse one: the image, the reader of the player's frames, the player.
    std::unique_ptr<ClipPlayer> player_; // Null if the video did not open.
    std::unique_ptr<ImageReader> reader_;
    std::unique_ptr<ScreenImage> image_;
    std::string openError_;
    // The UI finder on the frames shown, handed to it through uiImages_ (on the UI's device). Declared so the finder
    // goes before its channel.
    std::unique_ptr<ImageChannel> uiImages_;
    std::unique_ptr<UiFinder> uiFinder_; // Null if it could not start (uiError_), or the video did not open.
    std::string uiError_;
    bool uiBehind_ = false;                          // The frame shown is not handed to the finder yet,
    std::chrono::steady_clock::time_point nextUi_{}; // and may be from then on.
    std::uint64_t frame_ = 0;
    std::uint64_t wanted_ = 0; // A frame asked for (seek, step) that has not come yet; 0: none.

    std::shared_ptr<const ClipText> text_ = std::make_shared<const ClipText>();
    Guesses recorded_;
    Guesses today_; // The heuristics' own until snapped.
    std::string textError_;
    Guesser guesser_ = Guesser::Recorded;
    ComPtr<IDXGIAdapter1> gpu_;
    std::unique_ptr<ClipSnapper> snapper_; // Snapping Today's guesses; null once taken.
    // The ground followed over the clip (by the recorded guesses: what the app showed), once the flag Ground asked for
    // Today's; Recorded's is clip.txt's (text_), drawn live if any of it was (recordedGrid_).
    bool recordedGrid_ = false;
    bool groundAsked_ = false;
    std::unique_ptr<Grounding> grounding_; // What follows it next (today's heuristics').
    std::unique_ptr<ClipGround> ground_;   // Following it; null once taken.
    std::vector<GroundCamera> groundCameras_;
    std::string groundError_;
    // The depth maps the app drew live (depth.bin), read the first time the flag Depth asks, in image order.
    bool recordedDepth_ = false; // clip.txt says some were drawn.
    bool depthAsked_ = false;
    std::vector<DepthRecord> depths_;
    std::string depthError_;              // Why they cannot be drawn.
    std::unique_ptr<Depthing> depthing_;  // Today's shading (today's heuristics').
    std::string depthTodayError_;         // Why it could not shade a map.
    std::unique_ptr<DepthPacker> packer_;
    std::unique_ptr<ScreenImage> depthImage_; // The map drawn, at its size:
    std::uint64_t depthUploaded_ = 0;         // which one (DepthRecord::image; 0: none),
    Guesser depthUploadedBy_ = Guesser::Recorded; // as whose,
    bool depthStale_ = false;                 // and Today's made again with new heuristics since.
    std::vector<std::uint16_t> depthMap_;
    std::vector<std::uint8_t> depthLevels_;
    std::vector<std::uint32_t> depthPixels_;
    std::string snapError_;
    std::vector<ClipStats> recordedStats_; // clip-stats.txt's lines, in image order; empty: none.
    std::string heuristicsNotice_; // "Today's heuristics loaded again: ...", shown until noticeUntil_.
    std::chrono::steady_clock::time_point noticeUntil_{};

    bool scrubbing_ = false;      // The timeline is held down,
    bool playAfterScrub_ = false; // and it played before;
    std::uint64_t scrubAt_ = 0;   // the frame under the mouse.
};

} // namespace observex
