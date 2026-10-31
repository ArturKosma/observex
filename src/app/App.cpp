#include "app/App.h"

#include <imgui.h>
#include <imgui_impl_dx11.h>
#include <imgui_impl_win32.h>
#include <imgui_internal.h> // DockBuilder, for the first-run layout

#include <cstdio>
#include <exception>
#include <string>
#include <utility>

#ifndef OBSERVEX_VERSION
#define OBSERVEX_VERSION "dev"
#endif

namespace observex {

namespace {

constexpr char kCapturePanel[] = "Capture";
constexpr char kLogPanel[] = "Log";

} // namespace

App::App() : pipeline_(log_), logPanel_(log_) {}

App::~App() {
    shutdown();
}

int App::run() {
    try {
        init();
    } catch (const std::exception& e) {
        MessageBoxA(nullptr, e.what(), "observex could not start", MB_ICONERROR | MB_OK);
        shutdown();
        return 1;
    }

    while (window_->pumpMessages()) {
        // Restarts happen here, outside an ImGui frame, so no draw data still points at the old preview.
        if (restartRequested_ || pipeline_.state() == Pipeline::State::NeedsRestart) {
            restartRequested_ = false;
            startPipeline();
        }
        if (!window_->prepareFrame()) {
            pipeline_.setPreviewEnabled(false); // Minimized: nobody sees the preview.
            Sleep(10);
            continue;
        }
        if (preview_) {
            preview_->update();
        }

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();
        drawFrame();
        ImGui::Render();

        window_->bindBackBuffer();
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        // Panels dragged outside the main window live in their own OS windows.
        if ((ImGui::GetIO().ConfigFlags & ImGuiConfigFlags_ViewportsEnable) != 0) {
            ImGui::UpdatePlatformWindows();
            ImGui::RenderPlatformWindowsDefault();
        }
        window_->present();
    }

    shutdown();
    return 0;
}

void App::init() {
    // Per-monitor DPI awareness: sharp UI, and required by the capture API (DuplicateOutput1).
    ImGui_ImplWin32_EnableDpiAwareness();
    const float scale = ImGui_ImplWin32_GetDpiScaleForMonitor(MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY));

    target_ = findPrimaryMonitor();
    gpu_ = createGpuDevice(target_.adapter.Get());
    window_ = std::make_unique<MainWindow>(L"observex", gpu_.device.Get(), scale);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    io.ConfigFlags |= ImGuiConfigFlags_ViewportsEnable;
    io.ConfigDpiScaleFonts = true;
    io.ConfigDpiScaleViewports = true;

    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    style.ScaleAllSizes(scale);
    style.FontScaleDpi = scale;
    // With viewports on, square corners and an opaque background make panels in their own
    // OS windows look the same as docked ones.
    style.WindowRounding = 0.0f;
    style.Colors[ImGuiCol_WindowBg].w = 1.0f;

    ImGui_ImplWin32_Init(window_->handle());
    ImGui_ImplDX11_Init(gpu_.device.Get(), gpu_.context.Get());
    imguiReady_ = true;

    log(LogLevel::Info,
        std::string("observex ") + OBSERVEX_VERSION + " started (Dear ImGui " + ImGui::GetVersion() + ")");
    startPipeline();
}

void App::shutdown() {
    stopPipeline();
    if (imguiReady_) {
        ImGui_ImplDX11_Shutdown();
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext();
        imguiReady_ = false;
    }
    window_.reset();
    gpu_ = {};
}

void App::startPipeline() {
    stopPipeline();
    try {
        target_ = findPrimaryMonitor(); // The primary monitor may have changed since the last start.
    } catch (const std::exception& e) {
        log(LogLevel::Error, e.what());
    }
    if (!pipeline_.start(target_)) {
        return; // The pipeline logged why; the Capture panel shows it.
    }
    try {
        preview_ = std::make_unique<PreviewReader>(gpu_.device.Get(), *pipeline_.preview());
    } catch (const std::exception& e) {
        log(LogLevel::Error, std::string("Preview unavailable: ") + e.what());
    }
}

void App::stopPipeline() {
    preview_.reset(); // Before the pipeline frees the channel it reads from.
    pipeline_.stop();
}

void App::drawFrame() {
    drawMenuBar();

    const ImGuiID dockspace = ImGui::GetID("MainDockspace");
    dockPanelsOnFirstRun(dockspace);
    ImGui::DockSpaceOverViewport(dockspace);

    if (showCapture_) {
        capturePanel_.draw(kCapturePanel, &showCapture_, pipeline_, preview_.get(), restartRequested_);
    }
    pipeline_.setPreviewEnabled(showCapture_ && capturePanel_.wantsPreview());

    if (showLog_) {
        logPanel_.draw(kLogPanel, &showLog_);
    }
    if (showImGuiDemo_) {
        ImGui::ShowDemoWindow(&showImGuiDemo_);
    }
}

void App::dockPanelsOnFirstRun(unsigned dockspace) {
    // A saved layout (imgui.ini) already has this node; only build the default one when there is none.
    if (ImGui::DockBuilderGetNode(dockspace) != nullptr) {
        return;
    }
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::DockBuilderAddNode(dockspace, ImGuiDockNodeFlags_DockSpace);
    ImGui::DockBuilderSetNodeSize(dockspace, viewport->WorkSize);
    ImGuiID top = 0;
    ImGuiID bottom = 0;
    ImGui::DockBuilderSplitNode(dockspace, ImGuiDir_Down, 0.25f, &bottom, &top);
    ImGui::DockBuilderDockWindow(kCapturePanel, top);
    ImGui::DockBuilderDockWindow(kLogPanel, bottom);
    ImGui::DockBuilderFinish(dockspace);
}

void App::drawMenuBar() {
    if (!ImGui::BeginMainMenuBar()) {
        return;
    }
    if (ImGui::BeginMenu("File")) {
        if (ImGui::MenuItem("Exit", "Alt+F4")) {
            window_->close();
        }
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("View")) {
        ImGui::MenuItem(kCapturePanel, nullptr, &showCapture_);
        ImGui::MenuItem(kLogPanel, nullptr, &showLog_);
        ImGui::Separator();
        ImGui::MenuItem("ImGui demo", nullptr, &showImGuiDemo_);
        ImGui::EndMenu();
    }

    // Right-aligned: how fast the UI itself is drawing (separate from the capture rate).
    char status[32];
    std::snprintf(status, sizeof(status), "UI %.0f FPS", static_cast<double>(ImGui::GetIO().Framerate));
    const float statusWidth = ImGui::CalcTextSize(status).x + ImGui::GetStyle().ItemSpacing.x;
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - statusWidth);
    ImGui::TextDisabled("%s", status);

    ImGui::EndMainMenuBar();
}

void App::log(LogLevel level, std::string_view message) {
    LogEntry entry;
    entry.time = Clock::now();
    entry.level = level;
    entry.source = "app";
    entry.message = std::string(message);
    log_.push(std::move(entry));
}

} // namespace observex
