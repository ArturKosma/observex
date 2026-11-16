#pragma once

#include "app/ClipView.h"
#include "app/GeodataView.h"
#include "app/LogView.h"
#include "app/MouseTest.h"
#include "app/PicoLink.h"
#include "app/StateTreeView.h"
#include "app/MainWindow.h"
#include "app/Review.h"
#include "app/ScreenImage.h"
#include "app/ScreenPicker.h"
#include "app/Settings.h"
#include "app/HotKey.h"
#include "app/ShotHook.h"
#include "app/StatsPanel.h"
#include "app/Theme.h"
#include "app/Thumbnails.h"
#include "core/CameraFocus.h"
#include "core/LookAround.h"
#include "core/MonsterMemory.h"
#include "core/Costs.h"
#include "core/Distances.h"
#include "core/Log.h"
#include "core/DropPick.h"
#include "core/Flee.h"
#include "core/HpWatch.h"
#include "core/LootWatch.h"
#include "core/MouseCamera.h"
#include "core/Recordings.h"
#include "core/SkillRefresh.h"
#include "core/SkillUse.h"
#include "core/TargetSelect.h"
#include "core/TargetHitWatch.h"
#include "core/TargetWatch.h"
#include "core/Tiles.h"
#include "core/XpWatch.h"
#include "engine/BoxSnapper.h"
#include "engine/ClipRecorder.h"
#include "engine/DepthFinder.h"
#include "engine/DropFinder.h"
#include "engine/Gpu.h"
#include "engine/HeuristicsLibrary.h"
#include "engine/ImageChannel.h"
#include "engine/KindUpdater.h"
#include "engine/LiveLog.h"
#include "engine/MouseWatch.h"
#include "engine/PicoController.h"
#include "engine/Pipeline.h"
#include "engine/Recognizer.h"
#include "engine/Recorder.h"
#include "engine/Refresher.h"
#include "engine/StatsLibrary.h"
#include "engine/GroundFinder.h"
#include "engine/UiFinder.h"

#include <array>
#include <chrono>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace observex {

// Owns everything: the window, the UI's GPU device, ImGui and the pipeline. The UI runs on this thread;
// the pipeline runs on its own.
//
// The UI is a top bar, the tabs Live, Stats and State under it, and below them the tiles the tabs show (core/Tiles):
// Live (the screen being observed, a recording being reviewed or a clip being played), Stats (the numbers read from the
// game's interface) and State (the bot's brain). The bar holds the screen button (live miniatures of all screens;
// picking one starts the pipeline on it), Show (what is drawn over the observed screen, e.g. the characters the
// recogniser finds on it, or the game's interface), the Review button (training recordings or clips), Save last 10 s (a
// clip), the Record button and Settings. While reviewing, its middle steps through the images; while a clip plays, it
// plays and pauses it.
class App {
public:
    // Snapshot mode, for checking the UI without showing it: the window stays hidden, the input `steps` play in
    // order (about 0.6 s apart), the mouse rests at `mouse`, and after `wait` more seconds (at least; e.g. for the
    // finder) the frame is saved as a PNG at `path`. Then the app quits. Settings are not read or saved, recordings
    // go to `recordingsFolder` (by default a "recordings" folder next to the PNG; the Review button lists the
    // recordings in it), and no Windows dialog is shown.
    struct Snapshot {
        // A click (from == to), a drag (from somewhere to somewhere else), a turn of the mouse wheel at `from`, a
        // key press, or text typed. Window pixels.
        struct Step {
            ImVec2 from;
            ImVec2 to;
            ImGuiKey key = ImGuiKey_None;
            ImGuiKey modifier = ImGuiMod_None; // Held with the key: ImGuiMod_Ctrl (Ctrl+Z) or ImGuiMod_Shift.
            ImGuiMouseButton button = ImGuiMouseButton_Left; // Of the click or drag.
            bool release = true; // False (--hold): the button stays down, as in the middle of a drag.
            float wheel = 0.0f;                              // Wheel turns (away from the user: positive).
            std::string text;                                // Typed into the field with the keyboard (UTF-8).
        };
        std::wstring path;
        std::vector<Step> steps;
        std::optional<ImVec2> mouse;
        std::filesystem::path recordingsFolder;
        float wait = 0.0f;
        bool shot = false; // --shot: once a screen is observed, a training screenshot as Shift+Win takes.
    };

    explicit App(std::optional<Snapshot> snapshot = std::nullopt) : snapshot_(std::move(snapshot)) {}
    ~App();

    App(const App&) = delete;
    App& operator=(const App&) = delete;

    // Runs until the window is closed. Returns the process exit code.
    int run();

private:
    void init();
    void shutdown();

    // Starts the pipeline on `screen`, shows what it captures and starts looking for characters on it.
    void observe(const Screen& screen);
    void stopObserving();
    // After a display mode change: observes the same screen again if it is still connected.
    void observeAgain();

    // Recording. The user starts it, and stops it or it stops by itself (recordingDone): the Record button asks which
    // kind (RecordKind). A recording outlives pipeline restarts: pause before the pipeline stops, resume once it runs
    // again, into the same folder.
    bool canRecord() const;
    bool canRecordUnsure() const; // The model is loading or running, so it can say what it is unsure of.
    void startRecording(RecordKind kind);
    // `why` goes into the clip buffer's notes: "recording stopped", "recording done", ...
    void stopRecording(const char* why = "recording stopped");
    void pauseRecording();
    void resumeRecording();

    // Training screenshots (the user asked, 2026-11-15): Shift+Win while a screen is observed saves its next image, at
    // full size and lossless, into the hour's handmade package (core/Recordings: handmadeFolderName), made the first
    // time; the Log says each one saved. pollShots, every frame: the shortcut, and the Log lines.
    void takeShot();
    void pollShots();
    void logSavedShots(); // A Log line for each screenshot saved since the last call.

    // Clips (the user asked, 2026-11-10): the clip buffer always holds the last 10 s of the observed screen; Save last
    // 10 s saves them into a new folder in the clips folder, named by the date and time.
    bool canSaveClip() const;
    void saveClip();
    // A line for the clip buffer's text about what the user did, e.g. "show Kinds off". Nothing while there is no buffer.
    void noteInClip(std::string_view text);
    // A line of observex's log (the Log tab; the user asked, 2026-11-13): into log_ (its file) and the clip buffer,
    // timed at `qpc` (a QueryPerformanceCounter time a moment ago; 0: now). Only what the game gave: XP got and items or
    // adena picked up (the user asked, 2026-11-16: "in the log I just want the exp and drop pick up").
    void addLog(std::string text, std::int64_t qpc = 0);
    // A line of observex's trace: everything else it says (observing, clips, the brain's tasks, why something did not
    // start, Geodata, targets, drops seen), kept out of the Log tab: into trace_ (observex-trace.log, for reading back
    // what happened) and the clip buffer (clip.txt's trace lines), timed the same way.
    void addTrace(std::string text, std::int64_t qpc = 0);
    // What the Show flag Ground drew this UI frame (groundDrawn_), into the clip buffer if it changed: so a clip's
    // Recorded shows the grid exactly as it was seen.
    void noteGroundInClip();
    // Likewise the depth map the Show flag Depth drew this UI frame (depthDrawn_).
    void noteDepthInClip();
    // Likewise the drops the Show flag Drops draws (lootShown_; whatever the flag says), after each watchLoot.
    void noteDropsInClip();
    // Likewise what the State tile shows green of the brain (the user asked, 2026-11-15: a clip's review showed no
    // active state or task): a line when its current state or task changes, or it starts or stops acting, and the first
    // after the clip buffer starts.
    void noteBrainInClip();
    // The Stats tab's Camera card live: the camera by the mouse's counts on the frame shown (previewMs_).
    CameraShown liveCamera() const;
    // Likewise the Camera card (the user asked, 2026-11-15 night: "last 10s is not saving camera"; whatever the tab
    // shows): a line whenever it changes, and the first after the clip buffer starts.
    void noteCameraInClip();
    // Starts the depth finder on the observed screen (it runs while the flag Depth draws its maps or the flag Ground
    // takes the grid's pitch from them); depthFinderError_ says why it could not.
    void startDepth();

    // Review: first what to review (the review chooser: training recordings or clips). Recordings: pick one from the
    // list (the recording.observex files in the recordings folder), then what to look for in it (the category popup),
    // then review it. Clips: pick one from the list (the clip.observex files in the clips folder), then it plays.
    void openReviewChooser();
    void readRecordings();
    void readClips();
    void startReview(const Category& category);
    // Closes the review (Close review, picking a screen or opening a clip; closing observex just ends it) and starts
    // the kind update (KindUpdater), which learns the kinds from every reviewed answer, this review's too.
    void closeReview();
    // Between frames: opening a clip closes a review; closing it (Close clip, picking a screen, starting a review).
    void openClip(const ClipEntry& clip);
    // New heuristics for the clip screen's Today: from heuristics_, or the ones built in if the DLL makes none.
    std::unique_ptr<Heuristics> todaysHeuristics() const;
    // And its snapping, likewise.
    std::unique_ptr<Snapping> todaysSnapping() const;
    // And its following of the camera (the Ground flag), likewise.
    std::unique_ptr<Grounding> todaysGround() const;
    // And its shading of depth (the Depth flag), likewise.
    std::unique_ptr<Depthing> todaysDepth() const;
    void closeClip();
    // Each frame: the numbers the UI finder read on its newest image join statsHistory_ (the last kStatsKeptSeconds).
    void keepStats();
    // The State tile's brain takes a step on each new reading (live, or a clip's today), and stops when none is read.
    void tickBrain();
    // What observex spends on a screen image, all its parts added up (ms); with `parts`, each part's. The total of
    // costs(), so the top bar and the Debug tile say the same.
    float imageWorkMs(std::string* parts);
    // What each running part of observex spent over the last second (core/Costs), named, in the order the screen's
    // images go through them, the app's own frame last; a part that took no image that second is left out.
    std::vector<CostPart> costs();
    // Once a second while the clip buffer runs, and just before a save: costs() into it (a clip's Debug tile).
    void noteCostsInClip();
    // The Debug tile: costs(), or an open clip's as recorded around the frame shown.
    void drawDebug();
    // Into a clip being saved in `folder`: what statsHistory_ holds, as clip-stats.txt (core/GameStats).
    void writeClipStats(const std::filesystem::path& folder);
    // The Stats tile's numbers: live (the UI finder's newest), or the clip's Recorded or Today.
    void drawGameStats();
    // The Skills tab: the profile's skills, and Add Skill from the screen or the clip's frame.
    void drawSkills();
    // Each frame: the profile's skills, when they changed, to the readers and the icons.
    void syncSkills();
    // The profile's skills as the readers take them (grey).
    std::vector<SkillToFind> skillsToFind() const;
    // Each frame, live only (the user asked, 2026-11-15: "keep observing if i change the skill's position on the
    // skillbar"): a skill seen ready on another slot, or with another key on its slot, on kFollowReadings readings in a
    // row, has the profile's shortcut and miniature taken from it (logged, saved; Ctrl+Z in the State tile undoes it).
    void followSkills();
    // The shortcut `key` typed in the Skills tab for the skill `name` (already in the profile): its slot's letters are
    // learned from its newest picture (models/ui/keys.txt, which the readers read again), and the key stands while the
    // skill stays on that slot.
    void keyTyped(const std::string& name, const std::string& key);
    // The Nearby counts (core/GameStats' addNearby) of what the live view shows, into `stats`.
    void addLiveNearby(GameStats& stats);
    // The monsters the live view shows (sure of the kind, seen now), each with its box on the screen image and on the
    // desktop (pixels): what Select Target chooses from. Empty when not observing or nothing is found.
    std::vector<SelectableMonster> selectableMonsters();
    // The brain's Attacker (core/Attacker): the monsters the live view shows now (at least kAttackerKindSure a monster,
    // seen now) within kAttackerReach of the own character, nearest first, each with its distance (own heights: along
    // the ground by the camera shown, or on the screen while it is not known) and, as its desktop point, where to click
    // it beside the own character's box. `ownBox`, if given, gets the own character's box (none: x1 0).
    std::vector<SelectableMonster> attackerMonsters(StatsArea* ownBox = nullptr);
    // The same as the Mouse debug sweep takes them (their boxes on the desktop).
    std::vector<MouseTarget> visibleMonsters();
    // Acting: the brain's tasks drive the mouse and keyboard through the Pico controller (the user asked, 2026-11-15).
    // The Act button toggles it; it only runs while a live screen is observed.
    void toggleActing();
    void stopActing();
    // Each UI frame while acting: runs the current state's task that is due (they run one after another, each until
    // it is done: core/StateTree's TaskKind), and tells the state tree when it is done; and the Parallel tasks of the
    // active states alongside (runParallelTasks).
    void actuateBrain();
    // The Parallel tasks of the active states (statetree::Runner::parallelTasks), each started when its state becomes
    // active and dropped when it is left (the user asked, 2026-11-15). `mouseTask`: the task one after another that
    // runs uses the mouse (Select Target, Click Mouse), so Focus Camera lets go of it.
    void runParallelTasks(std::chrono::steady_clock::time_point now, bool mouseTask);
    // Focus Camera's step this UI frame (core/CameraFocus) on what is shown now: the target's box (the target watch's),
    // the own character's, the camera's pitch (the mouse's counts) and distance (the ground finder's); its counts go to
    // the controller with the right button held. At most one step a frame (focusRan_).
    CameraFocuser::Output driveFocus(bool mouseTask);
    // Flee's step this UI frame (core/Flee) on what is shown now: the target's box (the target watch's, or the one
    // Select Target clicked), every character's, the drops', the interface's pieces, the camera on the image shown and
    // the depth's terrain; its cursor moves and clicks go to the controller (a click only if `canClick`: no press
    // held). Returns whether it is done (nothing to flee from). From the Attacker (`fleeFrom`), its box is
    // attackerMonsters' nearest, and there is one to flee from while one was seen or HP was lost within
    // kAttackerHoldSeconds.
    bool driveFlee(double dt, bool canClick, statetree::FleeFrom fleeFrom);
    // Lets go of Focus Camera's right button if it holds it (its task stopped).
    void releaseFocus();
    // Lets go of the right button if the task one after another (Look Around, Select Target turning) holds it.
    void releaseTaskRight();
    // The buttons observex holds now (the controller's report): a click's and the right button of a turn.
    std::uint8_t buttonsHeld() const {
        return static_cast<std::uint8_t>(heldButtons_ | (focusRight_ || taskRight_ ? 0x02 : 0));
    }
    // Whether the cursor is where the game puts it when it turns the camera (its window's middle).
    bool cursorAtGameMiddle() const;
    // The camera on the image shown (the ground finder's, turned and tilted on by the mouse's counts); unknown while
    // the ground finder has not followed one.
    GroundCamera cameraShown() const;
    // Each UI frame: the newest look's monsters into the monster memory, while the camera is known.
    void rememberMonsters();
    // The selectable monsters' distances (own heights) and memory keys, by the camera shown.
    void measureMonsters(std::vector<SelectableMonster>& monsters) const;
    // The Skills tab's Refresh (the user asked, 2026-11-15: the mouse over each skill on the hotbar, its MP cost read
    // off its tooltip; core/SkillRefresh): its button starts or stops it (Win+Esc stops it too); it only runs while a
    // live screen is observed and the brain is not active (both drive the controller). driveRefresh steps it each UI
    // frame and writes the costs read into the profile.
    void toggleRefresh();
    void stopRefresh(const std::string& why);
    void driveRefresh();
    // The profile's skills as the newest live reading sees them on the hotbar (on the desktop and the screen image).
    std::vector<RefreshSkill> refreshSkills() const;
    // The name plate the UI finder is to read for Select Target (its monster; id 0: none). Select Target's goes first;
    // without one, the target watch's (watchTarget). Handed over when it changes (sendLook).
    void lookAt(const PlateLook& look);
    void sendLook();
    // On each new live reading (keepStats): the target watch (core/TargetWatch) learns which box the target is from its
    // name plate, and when its HP bar reads empty logs its death and marks that box dead in the recogniser (the user
    // asked, 2026-11-15: "once we discover the death from stats we can shift the weights of the box towards dead. i'd
    // like to see the effect on my screen that im observing").
    void watchTarget(const GameStats& stats);
    // The boxes drawFound shows (the snapper's while it runs, else the recogniser's: one tracker, the same ids), the own
    // character's too, by id; none while the recogniser is not running.
    std::vector<WatchedThing> thingsShown();
    // The own character's box on the newest look (screen image pixels; core/Ground's ownCharacter), and into `others`
    // (when given) every other character's seen now; false, `own` untouched, while it is not seen.
    bool ownShown(StatsArea& own, std::vector<StatsArea>* others) const;
    // Each UI frame while observing: the loot watch (core/LootWatch) picks where the drop finder looks (round the dead
    // target's box, then round its drops), from what is on the screen and the drop finder's newest look, and its lines
    // go to the Log. Then the drops it shows are measured (lootDrops_) and counted (dropBook_).
    void watchLoot();
    // The Move To Drop task of the brain's looting branch: the first one in the branch of Root the current state is in
    // (that child of Root and all under it), while the brain acts; null when it is in none (not looting).
    const statetree::Task* lootTask() const;
    // The drops found and where they were looked for (the Show flag Drops), over the observed screen at `min` to `max`,
    // each tagged with how far it is by `camera` on the image shown and the terrain `heights` (null: flat ground).
    void drawDrops(ImDrawList* drawList, ImVec2 min, ImVec2 max, const GroundCamera& camera,
                   const GroundHeights* heights);
    // An open clip's reading on the frame shown (`today`: read again today, else as recorded) with the Nearby counts
    // of what its guesser shows there; null if none.
    const GameStats* clipStatsShown(bool today);
    // The Log tile: observex's log, or an open clip's lines (those after the frame shown dimmed).
    void drawLog();
    // Between frames: the screen picker's None, nothing observed any more.
    void stopObservingAll();
    // Between frames: the clip in `folder` goes to the Recycle Bin (its X in the clips list), closed first if open.
    void removeClip(const std::filesystem::path& folder);

    // Writes the settings to observex.cfg (not in snapshot mode).
    void saveSettings();

    // Snapshot mode: feeds this frame's mouse input. Returns true when this frame is the one to save.
    bool playSnapshot();

    void drawFrame();
    // The tabs Live, State, Stats and Log at the top of the view, `height` high: each shows or hides its tile (the last
    // one shown stays).
    void drawTabs(float height);
    // What the Live tile shows: the review, the clip, the observed screen or the screens to choose from.
    void drawLive();
    // The lines between the tiles, which drag to share the view differently; `log` is the Log tile and `debug` the
    // Debug tile (not shown: none), columns right of the others, which start at `left`.
    void drawTileLines(const Tiles& tiles, const TileRect& log, const TileRect& debug, float left);
    void drawTopBar(float height);
    void drawSettings();
    void drawRecordChooser();
    void drawShowFlags();
    // The Characters flag (Show, and Guesses over a clip): saved, and noted in the clip buffer.
    void showCharacters(bool on);
    // Over a clip: `guesser`'s characters are drawn (Characters on again if it was off).
    void pickGuesser(ClipView::Guesser guesser);
    // The clip screen's Guesses popup: the Characters flag with whose characters are drawn over the clip
    // (ClipView::Guesser) under it, Kinds and UI.
    void drawGuessers();
    // In the middle of the bar, ending left of `right`: the review's back, where it is and forward; the clip's frame
    // back, play or pause, frame on, and where it is.
    void drawReviewSteps(float height, float right);
    void drawClipSteps(float height, float right);
    void drawReviewChooser();
    void drawClips();
    void drawRemoveClip();
    void drawCategoryChooser();
    void drawChooseScreen();
    void drawObservedScreen();
    // The characters found on the observed screen, over its image at `min` to `max`, each tagged with how far it is
    // (as drawDrops).
    void drawFound(ImDrawList* drawList, ImVec2 min, ImVec2 max, const GroundCamera& camera,
                   const GroundHeights* heights);
    // Hands the ground finder what is on the screen now: the things shown and the interface's pieces.
    void hintGround();

    // One recording, from Record to Stop.
    struct Recording {
        std::filesystem::path folder; // Named by the date and time it started.
        std::chrono::steady_clock::time_point started;
        unsigned saved = 0;           // Images saved by earlier Recorders, before a pipeline restart,
        std::uint64_t savedBytes = 0; // and the bytes their files take.
        std::shared_ptr<LiveLog> log; // What live recognition sees and the saved images.
        RecordingInfo info; // Written to the folder's recording.observex at the start and again at the stop.
    };
    // One recording in the Review button's list.
    struct RecordingRow {
        RecordingEntry entry;
        std::string when;                  // "Today, 12:56" or "31 Oct, 23:59".
        std::size_t images = 0;            // Image files in its folder,
        std::uint64_t bytes = 0;           // and their size together.
        std::filesystem::path firstImage;  // Its picture in the list; empty if there are no images.
        std::vector<ReviewCount> reviewed; // How far each entry of kCategories got.
        std::string lastReviewed;          // "Last reviewed today, 10:48"; empty if no review changed answers.
    };
    // One clip in the clips list.
    struct ClipRow {
        ClipEntry entry;
        std::string when;        // When it was saved: "Today, 18:22".
        std::uint64_t bytes = 0; // Its files together (the video most).
    };
    // A list of recordings (`rows`) in the popup `popup`, under `title`: the training recordings, or the handmade
    // packages (`handmade`: no recording's length, "screenshots"). Picking one asks what to look for in it.
    void drawRecordings(const char* popup, const char* title, const std::vector<RecordingRow>& rows, bool handmade);

    float dpiScale_ = 1.0f;
    ComPtr<IDXGIAdapter1> gpuAdapter_; // The GPU of the primary monitor; observex works on its screens.
    GpuDevice gpu_;                    // The UI's device.
    std::unique_ptr<MainWindow> window_;
    theme::Fonts fonts_;
    std::unique_ptr<ScreenPicker> picker_;
    Pipeline pipeline_;
    std::unique_ptr<ImageReader> preview_;
    std::unique_ptr<ScreenImage> observedImage_;
    std::optional<Screen> observed_;         // The screen being observed, if any.
    std::optional<Screen> chosen_;           // Picked this frame; observed from the start of the next one.
    bool chosenNone_ = false;                // None picked this frame: nothing observed from the next one.
    std::string error_;                      // Why the observed screen cannot be shown.
    // What the recogniser and the clip screen's Today go by: observex_heuristics.dll, loaded again when rebuilt.
    std::shared_ptr<HeuristicsSource> heuristics_;
    // What reads the game's numbers (the UI finder's, live and on a clip): observex_stats.dll, likewise.
    std::shared_ptr<StatsSource> stats_;
    // The numbers the live UI finder read, oldest first, the last kStatsKeptSeconds: a saved clip gets them.
    std::deque<GameStats> statsHistory_;
    static constexpr double kStatsKeptSeconds = 15.0;
    // The bot's brain, edited in the State tile (the tree stays while observex runs).
    StateTreeView stateTree_;
    std::uint64_t brainImage_ = 0; // The reading the brain last stepped on.
    // The events seen on the live readings since the brain's last step (by statetree::Event), for its next one only
    // (the user asked, 2026-11-16: event-based conditions); none waits while it does not act.
    std::array<bool, statetree::kEvents> brainEvents_{};
    StatsTile statsTile_;          // The Stats tile.
    SkillsView skillsView_;        // The Skills tab (its Add Skill popup keeps what it shows).
    SkillIcons skillIcons_;        // The profile's skills' icons, for both.
    // The profile's skills as last given to the readers (the UI finder's, an open clip's) and the icons, and the
    // profile's text when they were last compared (syncSkills).
    std::vector<statetree::Skill> skillsGiven_;
    std::string profileSeen_;
    // followSkills: per skill (by name), the slot and key it was last seen ready with, on how many readings in a row,
    // and its picture then; the slot the profile last took it at (x1 0: not yet since observex started); and the slot
    // a key was typed for it at (x1 0: none), where what is read does not change it.
    struct SkillFollow {
        std::string name;
        StatsArea at{};
        std::string key;
        int same = 0;
        int side = 0;
        std::vector<std::uint8_t> bgra;
        StatsArea taken{};
        StatsArea held{};
    };
    std::vector<SkillFollow> skillFollow_;
    std::uint64_t skillPicturesImage_ = 0; // The UI finder's image they were last followed on.
    GameStats liveStats_; // The UI finder's newest reading, with the Nearby counts (keepStats); image 0: none.
    GameStats clipStats_; // An open clip's on the frame shown, likewise (clipStatsShown).
    // How the tiles share the view (core/Tiles; the lines between them drag): Live's share of the width beside the
    // others, and Stats' share of the height above State.
    float liveSplit_ = 0.6f; // Live's share; State and Stats 2/5 (the user asked, 2026-11-13).
    float statsSplit_ = 0.5f;
    float logSplit_ = 0.8f; // The share of the width left of the Log tile (a column at the right),
    float debugSplit_ = 0.66f; // and of the Debug tile (a column right of it; wider, for its table).
    // The app's own frame (the UI thread), step by step for the Debug tab, in the order the steps run; summed up once a
    // second into frameCosts_. Presenting is not timed: it waits for the screen.
    struct FrameStep {
        enum : int { Housekeeping, Skills, Stats, Geodata, Loot, Brain, Steering, Preview, Clip, NewFrame, Acting,
                     Drawing, Rendering };
    };
    CostMeter frameMeter_{"Housekeeping", "Skills followed", "Stats kept", "Geodata", "Loot watch", "Brain tick",
                          "Finders steered", "Preview copy", "Clip shown", "New frame", "Brain acting", "Drawing",
                          "Rendering"};
    CostPart frameCosts_;
    std::chrono::steady_clock::time_point frameCostsSince_ = std::chrono::steady_clock::now();
    // The Log tab's lines (core/Log: XP and pickups, addLog), kept in observex.log next to observex.exe (in snapshot
    // mode next to the PNG), and the trace (addTrace) in observex-trace.log beside it; made first in init(), gone after
    // shutdown().
    std::unique_ptr<LogBook> log_;
    std::unique_ptr<LogBook> trace_;
    std::filesystem::path crashLogFile_; // Where a crash is told (engine/CrashLog): the trace,
    std::filesystem::path crashFolder_;  // and its dump.
    LogView logView_;
    LogView clipLogView_;          // An open clip's lines,
    std::uint64_t clipsOpened_ = 0; // which change with each clip opened.
    XpWatch xpWatch_;               // The XP read live, for "Got 0.12% XP" lines.
    std::uint32_t pickupRun_ = 0;   // The stats reader's pickups (GameStats::pickups) logged: its run,
    std::uint32_t pickupCount_ = 0; // and how many of its pickups,
    std::int64_t pickupAdena_ = 0;  // and its Adena in all (PickupRead::adena), for brainTally_'s.
    HpWatch hpWatch_;               // The own HP read live: the brain's HP Lost event, and "HP lost 12" lines.
    TargetHitWatch targetHitWatch_; // The target's HP read live: the brain's Target Hit By Others event, and its lines.
    std::unique_ptr<Recognizer> recognizer_; // Looks for characters on the observed screen, shown or not.
    std::string recognizerError_;            // Why it could not start.
    // Puts the recogniser's boxes onto every new image; shown instead of the recogniser's while it runs. Shared with
    // the recogniser, which hands it every look.
    std::shared_ptr<BoxSnapper> snapper_;
    std::string snapperError_; // Why it could not start.
    // Where the game's interface is, found all the time while observing; the Show flag UI draws it.
    std::unique_ptr<UiFinder> uiFinder_;
    std::string uiFinderError_; // Why it could not start.
    // Follows the game's camera while the Show flag Ground is on, which draws the ground plane it sees.
    std::unique_ptr<GroundFinder> groundFinder_;
    std::string groundFinderError_; // Why it could not start.
    bool groundFollowing_ = false;  // It was handed images last frame (else it starts afresh when it is again).
    double previewMs_ = 0.0;        // When the screen showed the image last previewed (groundMs),
    std::int64_t previewQpc_ = 0;   // likewise in QPC ticks, and its number.
    std::uint64_t previewImage_ = 0;
    // The grid the flag Ground drew over the preview this UI frame (drawObservedScreen), and the last one the clip
    // buffer was given.
    struct GroundDrawn {
        bool drawn = false;
        std::uint64_t image = 0;    // The preview image it was drawn over,
        std::uint64_t followed = 0; // from the ground finder's camera, of this image followed whole (GroundSeen),
        GroundCamera camera;
        std::shared_ptr<const GroundHeights> heights; // on this terrain (null: flat).
    };
    GroundDrawn groundDrawn_;
    GroundDrawn groundNoted_;
    // Guesses how near each pixel is while the Show flag Depth (which draws it over the screen) or Ground (whose pitch
    // comes from its maps) is on. Made the first time one is on while observing, kept until observing stops.
    std::unique_ptr<DepthFinder> depthFinder_;
    std::string depthFinderError_;            // Why it could not start.
    bool depthRunning_ = false;               // It was handed images this frame,
    bool depthShowing_ = false;               // and the flag Depth draws them.
    std::unique_ptr<ScreenImage> depthImage_; // The newest map drawn (at the map's size),
    std::uint64_t depthUploaded_ = 0;         // which one (DepthSeen::image).
    std::uint64_t depthDrawn_ = 0;            // The map drawn over the preview this UI frame (0: none),
    std::uint64_t depthNoted_ = 0;            // and the last one the clip buffer was given.
    // Where the character is in the world (the minimap followed, /loc read from the chat) and the Show flag Geodata's
    // grid round it. Made at start; follows every stats reading, flag on or off.
    std::unique_ptr<GeodataView> geodata_;
    // Keeps the last 10 s of the observed screen and what the recogniser saw, to save as a clip on request. Shared
    // with the recogniser, which hands it every look.
    std::shared_ptr<ClipRecorder> clipRecorder_;
    // Hands the clip buffer every report of the mouse (its counts say how far the camera turned while the right button
    // was held; the user asked, 2026-11-15), and the camera by them (core/MouseCamera: the ground finder follows it,
    // the grid and Stats' card Camera show it on every frame). Made with the observing.
    std::unique_ptr<MouseWatch> mouseWatch_;
    std::shared_ptr<SharedMouseCamera> mouseCamera_;
    std::string clipError_;                             // Why it could not start.
    bool clipSaving_ = false;                           // Asked it to save; waiting to hear how it went.
    std::chrono::steady_clock::time_point clipSavedAt_; // The last save that worked: the button says so for a while.
    std::string clipSaveError_;                         // Why the last save failed.
    std::unique_ptr<ClipView> clipView_;                // While a clip plays; shown instead of the observed screen.
    std::optional<ClipEntry> clipToOpen_;               // Picked in the clips list; opened between frames.
    bool closeClip_ = false;                            // Close clip was clicked; closed between frames.
    std::optional<ClipRow> clipToConfirm_;              // Its X was clicked in the clips list: asking first.
    std::optional<std::filesystem::path> clipToRemove_; // Removing it was confirmed; removed between frames.
    float guessesRight_ = 0.0f;                         // Where the Guesses button ended, the frame before.
    std::vector<ClipRow> clips_;                        // The clips list, read when Review opens.
    Settings settings_;
    std::filesystem::path settingsFile_; // Empty in snapshot mode: settings are not saved.
    std::optional<Recording> recording_; // While recording.
    std::unique_ptr<Recorder> recorder_; // Saves the recording while the pipeline runs.
    std::string recordingError_;         // Why the last recording stopped or could not start.
    std::unique_ptr<Review> review_;     // While reviewing; shown instead of the observed screen.
    // While reviewing, once the review's own finder is done (the GPU is free): makes the other reviewed recordings'
    // guesses fresh for the kind update after a new model.
    std::unique_ptr<Refresher> refresher_;
    std::unique_ptr<KindUpdater> kindUpdater_; // After a review closes, until it is done.
    std::vector<RecordingRow> recordings_;     // The Review button's list, read when it opens.
    std::vector<RecordingRow> handmade_;       // Likewise the handmade packages (RecordKind::Handmade).
    std::unique_ptr<Thumbnails> thumbnails_;   // The list's pictures, kept between openings.
    const char* openList_ = nullptr;           // Picked in the review chooser: the list popup to open next.
    bool openCategory_ = false;                // A recording was picked in the list: ask for the category next.
    std::filesystem::path reviewFolder_;       // Picked for review, waiting for the category.
    std::string reviewAbout_; // What was picked, for the category chooser: "Recorded Today, 12:56 · 110 images".
    // The hour's handmade package the training screenshots go into while a screen is observed (none until the first).
    struct ShotPackage {
        std::filesystem::path folder;
        std::string title;   // "15/11/2026 - 15:00".
        unsigned first = 1;  // The number of the first image its recorder saves.
        unsigned logged = 0; // Of its recorder's saved images, those already in the Log.
    };
    std::unique_ptr<ShotHook> shotHook_;     // While a screen is observed (not in snapshot mode): Shift+Win.
    std::optional<ShotPackage> shotPackage_;
    std::unique_ptr<Recorder> shotRecorder_; // Saves the screenshots into shotPackage_ (reads the pipeline's shots()).
    bool snapshotShot_ = false;              // Snapshot mode's --shot taken.
    bool closeReview_ = false; // Close was clicked; closed between frames.
    std::string reviewError_;  // Why the last review could not start.
    PicoLink picoLink_;        // Whether the reactor's Raspberry Pi Pico is plugged in (the top bar's green/red light).
    // TEMPORARY (the user asked, 2026-11-15; to be removed with app/MouseTest.* and engine/PicoController.*): the
    // serial link to the Pico and the "Mouse debug" sweep that drives the real cursor over the visible monsters.
    PicoController controller_;    // Declared before mouseTest_, so the sweep is torn down before the link it uses.
    MouseTest mouseTest_;
    std::string mouseTestStatus_;  // What the Settings button last reported (why it could not start, or that it did).
    // Acting (the brain drives the mouse/keyboard through the controller): off by default, toggled by the Act button.
    bool brainActs_ = false;
    // What the brain got since it last started (the Stats tile's card Since the Brain Started; the user asked,
    // 2026-11-16): reset by toggleActing, kills by watchTarget, adena by keepStats' pickups, while it acts. Its time from
    // brainStartedAt_ to now, or to brainStoppedAt_ once stopped (brainTallyNow).
    BrainTally brainTally_;
    std::chrono::steady_clock::time_point brainStartedAt_{};
    std::chrono::steady_clock::time_point brainStoppedAt_{};
    BrainTally brainTallyNow() const;
    // The brain the clip buffer was given last (noteBrainInClip): its current state shown (0: none, inactive) and its
    // task; none yet since the buffer started.
    bool brainNoted_ = false;
    int brainNotedState_ = 0;
    int brainNotedTask_ = -1;
    // The Camera card the clip buffer was given last (noteCameraInClip); none yet since the buffer started.
    bool cameraNoted_ = false;
    CameraShown notedCamera_;
    // The task that runs: the state tree's count of task starts when it started (another: a task starts afresh),
    // whether its press was sent (Click Mouse: done once it is let go), and Select Target's and Use Skill's own (Use
    // Skill on the newest live reading's variables, made once a reading).
    std::uint64_t actingTaskStart_ = ~std::uint64_t{0};
    bool taskPressed_ = false;
    // The Parallel tasks running: where each is, its kind (another kind there now: it starts afresh), and when Debug
    // Print writes next. Emptied when the brain stops or starts.
    struct ParallelRun {
        statetree::TaskAt at;
        statetree::TaskKind kind = statetree::TaskKind::DebugPrint;
        std::chrono::steady_clock::time_point next;
    };
    std::vector<ParallelRun> parallelRuns_;
    // Focus Camera (one at a time, Parallel or not): whether it holds the right button, whether it stepped this UI
    // frame (and so the ground finder follows, for the camera's pitch and distance), and what that step said.
    CameraFocuser focuser_;
    bool focusRight_ = false;
    bool focusRan_ = false;
    CameraFocuser::Output focusOut_;
    // The target's box as the target watch last found it (its tracker id), kept while it looks for it again; 0 when
    // there is no target, or another one (by name) since.
    std::uint32_t targetBox_ = 0;
    std::string targetBoxName_;
    TargetSelector selector_;
    // Look Around (core/LookAround), and whether it or Select Target (turning to a monster off the screen) holds the
    // right button: the task one after another that runs, let go of when it stops.
    LookAround looker_;
    bool taskRight_ = false;
    // The monsters remembered round the own character (core/MonsterMemory; the user asked, 2026-11-16): fed by every
    // look while the camera is known (rememberMonsters), forgotten by Look Around, chosen from by Select Target, and
    // counted in the Nearby gauges Monsters Around / Monster Distance.
    MonsterMemory monsterMemory_;
    std::uint64_t rememberedLook_ = 0; // The recogniser's (or snapper's) look it was fed last.
    SkillUser skillUser_;
    statetree::Variables skillVariables_;
    std::uint64_t skillVariablesImage_ = 0;
    // Refresh, while it runs (skillRefresher_.running()), and its OCR read asked of the UI finder (0: none; answered
    // once UiFinder::text().request is it).
    SkillRefresher skillRefresher_;
    std::uint64_t refreshAsked_ = 0;
    PlateLook lookAsked_{}; // What the UI finder was last asked to look at (sendLook's pick).
    PlateLook brainLook_{}; // Select Target's look (lookAt), first.
    TargetWatch targetWatch_;
    PlateLook watchLook_{}; // The target watch's look, when Select Target asks for none.
    // Drops (the user asked, 2026-11-15): the drop model (models/drop) sweeping the screen, and looking closely at the
    // spot the loot watch picks (round a kill, or drops found). Made when observing starts, if there is a drop model.
    std::unique_ptr<DropFinder> dropFinder_;
    std::string dropFinderError_; // Why it could not start (no model yet).
    LootWatch lootWatch_;
    LootWatch::Output loot_;      // Where it looks closely now and what it found (watchLoot).
    std::vector<Detection> lootShown_; // The drops of the spot's newest close look, off the game's interface.
    // The distances written this UI frame (core/Distances; game units, under 0 none), by the characters drawFound drew
    // (the snapper's or the recogniser's newest, in their order) and by lootShown_: what the brain's conditions are to
    // read (the user, 2026-11-16: "later on this distance will be used as a condition").
    std::vector<float> thingDistances_;
    std::vector<float> dropDistances_;
    std::uint64_t lootImage_ = 0;      // The screen image of that look.
    bool dropsNoted_ = false;          // What noteDropsInClip handed the clip buffer last: the spot drawn round
    LootSquare dropsNotedSquare_{};    // (empty: nothing drawn), the close look's image and whether a kill's.
    std::uint64_t dropsNotedImage_ = 0;
    bool dropsNotedKill_ = false;
    std::uint32_t deadNow_ = 0;   // A box the target watch marked dead since the last watchLoot,
    std::string deadName_;        // and the target's name then.
    // When the target last died (the target watch's death, the reading's screen image's QPC; 0: no kill yet): the
    // Since Kill gauge (addLiveNearby) the brain waits on for a kill's drops.
    std::int64_t killQpc_ = 0;
    // When HP was last lost (hpWatch_; QPC, 0: not yet; also starts the recogniser's close-up looks, lookCloser) and when
    // Flee's Attacker was last seen (QPC, 0: not yet).
    std::int64_t hpLostQpc_ = 0;
    std::int64_t attackerSeenQpc_ = 0;
    // Looting (the user asked, 2026-11-16: drops picked up before the next monster): the drops of lootShown_ as looting
    // sees them (lootDrops_, each frame in watchLoot: distance and place round the own feet by the camera shown),
    // which of them count (dropBook_: the Drops and Drop Distance numbers, those given up on left out) and the Move To
    // Drop task (dropPicker_).
    std::vector<LootDrop> lootDrops_;
    DropBook dropBook_;
    DropBook::Output dropsCounted_;
    DropPicker dropPicker_;
    // The Flee task (core/Flee), and whether it is the task that runs (the depth finder runs for it).
    FleeWalker flee_;
    bool fleeing_ = false;
    // A Click Mouse / Use Skill press is held for a short while, not tapped, so the game registers it. The held mouse
    // button rides along with Select Target's moves (so it can drag), and both are released after their time.
    std::uint8_t heldButtons_ = 0;
    std::chrono::steady_clock::time_point buttonReleaseAt_{};
    bool keyHeld_ = false;
    std::chrono::steady_clock::time_point keyReleaseAt_{};
    std::unique_ptr<HotKey> brainKey_; // Win+Esc, the Brain's start/stop shortcut, while a screen is observed.
    bool showStats_ = false;   // Toggled with F3.
    bool imguiReady_ = false;
    std::optional<Snapshot> snapshot_;
    int snapshotFrame_ = 0;
};

} // namespace observex
