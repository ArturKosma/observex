#pragma once

#include "app/MainWindow.h"
#include "app/Review.h"
#include "app/ScreenImage.h"
#include "app/ScreenPicker.h"
#include "app/Settings.h"
#include "app/Theme.h"
#include "engine/Gpu.h"
#include "engine/ImageChannel.h"
#include "engine/Pipeline.h"
#include "engine/Recorder.h"

#include <chrono>
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
// screen button (live miniatures of all screens; picking one starts the pipeline on it), the Review button,
// the Record button and Settings. While reviewing, its middle steps through the images.
class App {
public:
    // Snapshot mode, for checking the UI without showing it: the window stays hidden, the input `steps` play in
    // order (about 0.6 s apart), the mouse rests at `mouse`, and the frame is saved as a PNG at `path`. Then the
    // app quits. Settings are not read or saved, recordings go to a "recordings" folder next to the PNG, and no
    // Windows dialog is shown: the Review button opens `reviewFolder`.
    struct Snapshot {
        // A click (from == to), a drag (from somewhere to somewhere else), or a key press. Window pixels.
        struct Step {
            ImVec2 from;
            ImVec2 to;
            ImGuiKey key = ImGuiKey_None;
        };
        std::wstring path;
        std::vector<Step> steps;
        std::optional<ImVec2> mouse;
        std::filesystem::path reviewFolder;
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

    // Starts the pipeline on `screen` and shows what it captures.
    void observe(const Screen& screen);
    void stopObserving();
    // After a display mode change: observes the same screen again if it is still connected.
    void observeAgain();

    // Recording. Start and stop are the user's (Record button or hotkey). A recording outlives pipeline
    // restarts: pause before the pipeline stops, resume once it runs again, into the same folder.
    bool canRecord() const;
    void toggleRecording();
    void startRecording();
    void stopRecording();
    void pauseRecording();
    void resumeRecording();

    // Review: pick a recording's folder, then what to look for in it (the category popup), then review it.
    void chooseReviewFolder();
    void startReview(const Category& category);

    // Writes the settings to observex.cfg (not in snapshot mode).
    void saveSettings();

    // Snapshot mode: feeds this frame's mouse input. Returns true when this frame is the one to save.
    bool playSnapshot();

    void drawFrame();
    void drawTopBar(float height);
    void drawSettings();
    void drawReviewSteps(float height);
    void drawCategoryChooser();
    void drawChooseScreen();
    void drawObservedScreen();

    // One recording, from Record to Stop.
    struct Recording {
        std::filesystem::path folder; // Named by the date and time it started.
        std::chrono::steady_clock::time_point started;
        unsigned saved = 0; // Images saved by earlier Recorders, before a pipeline restart.
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
    std::optional<Screen> observed_; // The screen being observed, if any.
    std::optional<Screen> chosen_;   // Picked this frame; observed from the start of the next one.
    std::string error_;              // Why the observed screen cannot be shown.
    Settings settings_;
    std::filesystem::path settingsFile_; // Empty in snapshot mode: settings are not saved.
    std::optional<Recording> recording_; // While recording.
    std::unique_ptr<Recorder> recorder_; // Saves the recording while the pipeline runs.
    std::string recordingError_;         // Why the last recording stopped or could not start.
    std::unique_ptr<Review> review_;     // While reviewing; shown instead of the observed screen.
    std::filesystem::path reviewFolder_; // Picked for review, waiting for the category.
    std::size_t reviewFolderImages_ = 0;
    bool closeReview_ = false;      // Close was clicked; closed between frames.
    std::string reviewError_;       // Why the last review could not start.
    bool hotkeyRegistered_ = false; // Ctrl+Alt+R toggles recording.
    bool showStats_ = false;        // Toggled with F3.
    bool imguiReady_ = false;
    std::optional<Snapshot> snapshot_;
    int snapshotFrame_ = 0;
};

} // namespace observex
