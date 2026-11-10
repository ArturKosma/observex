#pragma once

#include "app/MainWindow.h"
#include "app/Review.h"
#include "app/ScreenImage.h"
#include "app/ScreenPicker.h"
#include "app/Settings.h"
#include "app/Theme.h"
#include "app/Thumbnails.h"
#include "core/Recordings.h"
#include "engine/Gpu.h"
#include "engine/ImageChannel.h"
#include "engine/KindUpdater.h"
#include "engine/LiveLog.h"
#include "engine/Pipeline.h"
#include "engine/Recognizer.h"
#include "engine/Recorder.h"
#include "engine/Refresher.h"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace observex {

// Owns everything: the window, the UI's GPU device, ImGui and the pipeline. The UI runs on this thread;
// the pipeline runs on its own.
//
// The UI is a top bar and, below it, the screen being observed or a recording being reviewed. The bar holds the
// screen button (live miniatures of all screens; picking one starts the pipeline on it), Show (what is drawn over
// the observed screen, e.g. the characters the recogniser finds on it), the Review button, the Record button and
// Settings. While reviewing, its middle steps through the images.
class App {
public:
    // Snapshot mode, for checking the UI without showing it: the window stays hidden, the input `steps` play in
    // order (about 0.6 s apart), the mouse rests at `mouse`, and after `wait` more seconds (at least; e.g. for the
    // finder) the frame is saved as a PNG at `path`. Then the app quits. Settings are not read or saved, recordings
    // go to `recordingsFolder` (by default a "recordings" folder next to the PNG; the Review button lists the
    // recordings in it), and no Windows dialog is shown.
    struct Snapshot {
        // A click (from == to), a drag (from somewhere to somewhere else), a turn of the mouse wheel at `from`, or a
        // key press. Window pixels.
        struct Step {
            ImVec2 from;
            ImVec2 to;
            ImGuiKey key = ImGuiKey_None;
            ImGuiKey modifier = ImGuiMod_None; // Held with the key: ImGuiMod_Ctrl (Ctrl+Z) or ImGuiMod_Shift.
            ImGuiMouseButton button = ImGuiMouseButton_Left; // Of the click or drag.
            float wheel = 0.0f;                              // Wheel turns (away from the user: positive).
        };
        std::wstring path;
        std::vector<Step> steps;
        std::optional<ImVec2> mouse;
        std::filesystem::path recordingsFolder;
        float wait = 0.0f;
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

    // Recording. The user starts it, and stops it or it stops by itself after kRecordImages: the Record button asks
    // which kind (RecordKind), the hotkey starts the kind picked last. A recording outlives pipeline restarts: pause
    // before the pipeline stops, resume once it runs again, into the same folder.
    bool canRecord() const;
    bool canRecordUnsure() const; // The model is loading or running, so it can say what it is unsure of.
    void toggleRecording();
    void startRecording(RecordKind kind);
    void stopRecording();
    void pauseRecording();
    void resumeRecording();
    // Ctrl+Alt+M while recording: a numbered mark in the live log, and the image on screen is saved too.
    void markMoment();

    // Review: pick a recording from the list (the recording.observex files in the recordings folder), then what to
    // look for in it (the category popup), then review it.
    void openRecordings();
    void startReview(const Category& category);
    // Closes the review (Close review, or picking a screen; closing observex just ends it) and starts the kind update
    // (KindUpdater), which learns the kinds from every reviewed answer, this review's too.
    void closeReview();

    // Writes the settings to observex.cfg (not in snapshot mode).
    void saveSettings();

    // Snapshot mode: feeds this frame's mouse input. Returns true when this frame is the one to save.
    bool playSnapshot();

    void drawFrame();
    void drawTopBar(float height);
    void drawSettings();
    void drawRecordChooser();
    void drawShowFlags();
    void drawReviewSteps(float height);
    void drawRecordings();
    void drawCategoryChooser();
    void drawChooseScreen();
    void drawObservedScreen();
    // The characters found on the observed screen, over its image at `min` to `max`.
    void drawFound(ImDrawList* drawList, ImVec2 min, ImVec2 max);

    // One recording, from Record to Stop.
    struct Recording {
        std::filesystem::path folder; // Named by the date and time it started.
        std::chrono::steady_clock::time_point started;
        unsigned saved = 0;           // Images saved by earlier Recorders, before a pipeline restart,
        std::uint64_t savedBytes = 0; // and the bytes their files take.
        std::shared_ptr<LiveLog> log; // What live recognition sees, the saved images and the marks.
        unsigned marks = 0;
        RecordingInfo info; // Written to the folder's recording.observex at the start and again at the stop.
    };
    // One recording in the Review button's list.
    struct RecordingRow {
        RecordingEntry entry;
        std::string when;                  // "Today, 12:56" or "31 Oct, 23:59".
        std::size_t images = 0;            // Image files in its folder.
        std::filesystem::path firstImage;  // Its picture in the list; empty if there are no images.
        std::vector<ReviewCount> reviewed; // How far each entry of kCategories got.
        std::string lastReviewed;          // "Last reviewed today, 10:48"; empty if no review changed answers.
    };

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
    std::string error_;                      // Why the observed screen cannot be shown.
    std::unique_ptr<Recognizer> recognizer_; // Looks for characters on the observed screen, shown or not.
    std::string recognizerError_;            // Why it could not start.
    Settings settings_;
    std::filesystem::path settingsFile_;     // Empty in snapshot mode: settings are not saved.
    std::optional<Recording> recording_;     // While recording.
    std::unique_ptr<Recorder> recorder_;     // Saves the recording while the pipeline runs.
    std::string recordingError_;             // Why the last recording stopped or could not start.
    std::unique_ptr<Review> review_;         // While reviewing; shown instead of the observed screen.
    // While reviewing, once the review's own finder is done (the GPU is free): makes the other reviewed recordings'
    // guesses fresh for the kind update after a new model.
    std::unique_ptr<Refresher> refresher_;
    std::unique_ptr<KindUpdater> kindUpdater_; // After a review closes, until it is done.
    std::vector<RecordingRow> recordings_;   // The Review button's list, read when it opens.
    std::unique_ptr<Thumbnails> thumbnails_; // The list's pictures, kept between openings.
    bool openCategory_ = false;              // A recording was picked in the list: ask for the category next.
    std::filesystem::path reviewFolder_;     // Picked for review, waiting for the category.
    std::string reviewWhen_;                 // When it was recorded, for the category chooser.
    std::size_t reviewFolderImages_ = 0;
    bool closeReview_ = false;          // Close was clicked; closed between frames.
    std::string reviewError_;           // Why the last review could not start.
    bool hotkeyRegistered_ = false;     // Ctrl+Alt+R toggles recording.
    bool markHotkeyRegistered_ = false; // Ctrl+Alt+M marks a moment while recording.
    bool showStats_ = false;            // Toggled with F3.
    bool imguiReady_ = false;
    std::optional<Snapshot> snapshot_;
    int snapshotFrame_ = 0;
};

} // namespace observex
