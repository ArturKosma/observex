#pragma once

#include "app/CapturePanel.h"
#include "app/LogPanel.h"
#include "app/MainWindow.h"
#include "core/LogStore.h"
#include "engine/Gpu.h"
#include "engine/Pipeline.h"
#include "engine/PreviewChannel.h"

#include <memory>
#include <string_view>

namespace observex {

// Owns everything: the log, the UI (window, GPU device, ImGui, panels) and the pipeline.
// The UI thread runs here; the pipeline runs on its own thread.
class App {
public:
    App();
    ~App();

    App(const App&) = delete;
    App& operator=(const App&) = delete;

    // Runs until the window is closed. Returns the process exit code.
    int run();

private:
    void init();
    void shutdown();

    void startPipeline();
    void stopPipeline();

    void drawFrame();
    void drawMenuBar();
    void dockPanelsOnFirstRun(unsigned dockspace);

    // Adds an entry from observex itself (source "app") to the log.
    void log(LogLevel level, std::string_view message);

    LogStore log_;
    CaptureTarget target_;
    GpuDevice gpu_; // The UI's device, on the same GPU as the captured monitor so the preview can be shared.
    std::unique_ptr<MainWindow> window_;
    Pipeline pipeline_;
    std::unique_ptr<PreviewReader> preview_;
    LogPanel logPanel_;
    CapturePanel capturePanel_;

    bool imguiReady_ = false;
    bool restartRequested_ = false;
    bool showCapture_ = true;
    bool showLog_ = true;
    bool showImGuiDemo_ = false;
};

} // namespace observex
