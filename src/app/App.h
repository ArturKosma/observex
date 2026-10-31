#pragma once

#include "app/MainWindow.h"
#include "app/ScreenImage.h"
#include "app/ScreenPicker.h"
#include "app/Theme.h"
#include "engine/Gpu.h"
#include "engine/Pipeline.h"
#include "engine/PreviewChannel.h"

#include <memory>
#include <optional>
#include <string>

namespace observex {

// Owns everything: the window, the UI's GPU device, ImGui and the pipeline. The UI runs on this thread;
// the pipeline runs on its own.
//
// The UI is a top bar with the Observer button, which shows live miniatures of all screens, and below
// it the screen being observed. Picking a screen starts the pipeline on it.
class App {
public:
    App() = default;
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

    void drawFrame();
    void drawTopBar(float height);
    void drawChooseScreen();
    void drawObservedScreen();

    float dpiScale_ = 1.0f;
    ComPtr<IDXGIAdapter1> gpuAdapter_; // The GPU of the primary monitor; observex works on its screens.
    GpuDevice gpu_;                    // The UI's device.
    std::unique_ptr<MainWindow> window_;
    theme::Fonts fonts_;
    std::unique_ptr<ScreenPicker> picker_;
    Pipeline pipeline_;
    std::unique_ptr<PreviewReader> preview_;
    std::unique_ptr<ScreenImage> observedImage_;
    std::optional<Screen> observed_; // The screen being observed, if any.
    std::optional<Screen> chosen_;   // Picked this frame; observed from the start of the next one.
    std::string error_;              // Why the observed screen cannot be shown.
    bool showStats_ = false;         // Toggled with F3.
    bool imguiReady_ = false;
};

} // namespace observex
