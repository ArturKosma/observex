#include "app/App.h"

#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>

#define GL_SILENCE_DEPRECATION
#include <GLFW/glfw3.h> // Also brings in the system OpenGL header.

#include <cstdio>
#include <string>
#include <utility>

#ifndef OBSERVEX_VERSION
#define OBSERVEX_VERSION "dev"
#endif

namespace observex {

namespace {

// GLFW reports errors through a plain callback with no user data, so it reaches the store through this.
LogStore* g_errorLog = nullptr;

void onGlfwError(int code, const char* description) {
    std::fprintf(stderr, "GLFW error %d: %s\n", code, description);
    if (g_errorLog != nullptr) {
        LogEntry entry;
        entry.time = Clock::now();
        entry.level = LogLevel::Error;
        entry.source = "glfw";
        entry.message = "Error " + std::to_string(code) + ": " + description;
        g_errorLog->push(std::move(entry));
    }
}

const char* glString(GLenum name) {
    const auto* text = reinterpret_cast<const char*>(glGetString(name));
    return text != nullptr ? text : "?";
}

} // namespace

App::App() : demo_(store_), logPanel_(store_) {}

App::~App() {
    shutdown();
}

int App::run() {
    if (!init()) {
        shutdown();
        return 1;
    }

    while (!glfwWindowShouldClose(window_)) {
        glfwPollEvents();
        if (glfwGetWindowAttrib(window_, GLFW_ICONIFIED) != 0) {
            ImGui_ImplGlfw_Sleep(10);
            continue;
        }

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();
        drawFrame();
        ImGui::Render();

        int width = 0;
        int height = 0;
        glfwGetFramebufferSize(window_, &width, &height);
        glViewport(0, 0, width, height);
        glClearColor(0.08f, 0.08f, 0.09f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

        // Panels dragged outside the main window live in their own OS windows.
        if ((ImGui::GetIO().ConfigFlags & ImGuiConfigFlags_ViewportsEnable) != 0) {
            GLFWwindow* context = glfwGetCurrentContext();
            ImGui::UpdatePlatformWindows();
            ImGui::RenderPlatformWindowsDefault();
            glfwMakeContextCurrent(context);
        }
        glfwSwapBuffers(window_);
    }

    shutdown();
    return 0;
}

bool App::init() {
    g_errorLog = &store_;
    glfwSetErrorCallback(onGlfwError);
    if (glfwInit() == GLFW_FALSE) {
        return false;
    }
    glfwReady_ = true;

#if defined(__APPLE__)
    const char* glslVersion = "#version 150";
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 2);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE);
#else
    const char* glslVersion = "#version 130";
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 0);
#endif

    const float scale = ImGui_ImplGlfw_GetContentScaleForMonitor(glfwGetPrimaryMonitor());
    window_ =
        glfwCreateWindow(static_cast<int>(1280 * scale), static_cast<int>(800 * scale), "observex", nullptr, nullptr);
    if (window_ == nullptr) {
        return false;
    }
    glfwMakeContextCurrent(window_);
    glfwSwapInterval(1); // vsync

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

    ImGui_ImplGlfw_InitForOpenGL(window_, true);
    ImGui_ImplOpenGL3_Init(glslVersion);
    imguiReady_ = true;

    log(LogLevel::Info, std::string("observex ") + OBSERVEX_VERSION + " started (Dear ImGui " + ImGui::GetVersion() +
                            ", GLFW " + glfwGetVersionString() + ")");
    log(LogLevel::Info, std::string("OpenGL ") + glString(GL_VERSION) + " on " + glString(GL_RENDERER));

    demo_.start();
    return true;
}

void App::shutdown() {
    demo_.stop();
    if (imguiReady_) {
        ImGui_ImplOpenGL3_Shutdown();
        ImGui_ImplGlfw_Shutdown();
        ImGui::DestroyContext();
        imguiReady_ = false;
    }
    if (window_ != nullptr) {
        glfwDestroyWindow(window_);
        window_ = nullptr;
    }
    if (glfwReady_) {
        glfwTerminate();
        glfwReady_ = false;
    }
    g_errorLog = nullptr;
}

void App::drawFrame() {
    drawMenuBar();

    const ImGuiID dockspace = ImGui::DockSpaceOverViewport();
    if (showLogPanel_) {
        // On first launch the Log panel fills the window; after that ImGui remembers the layout (imgui.ini).
        ImGui::SetNextWindowDockID(dockspace, ImGuiCond_FirstUseEver);
        logPanel_.draw("Log", &showLogPanel_);
    }
    if (showImGuiDemo_) {
        ImGui::ShowDemoWindow(&showImGuiDemo_);
    }
}

void App::drawMenuBar() {
    if (!ImGui::BeginMainMenuBar()) {
        return;
    }

    if (ImGui::BeginMenu("File")) {
        if (ImGui::MenuItem("Exit", "Alt+F4")) {
            glfwSetWindowShouldClose(window_, GLFW_TRUE);
        }
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("View")) {
        ImGui::MenuItem("Log", nullptr, &showLogPanel_);
        ImGui::Separator();
        ImGui::MenuItem("ImGui demo", nullptr, &showImGuiDemo_);
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("Sources")) {
        const bool demoRunning = demo_.running();
        if (ImGui::MenuItem("Demo source", nullptr, demoRunning)) {
            if (demoRunning) {
                demo_.stop();
            } else {
                demo_.start();
            }
        }
        float rate = demo_.rate();
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 12.0f);
        if (ImGui::SliderFloat("Rate", &rate, DemoSource::kMinRate, DemoSource::kMaxRate, "%.0f / s",
                               ImGuiSliderFlags_Logarithmic)) {
            demo_.setRate(rate);
        }
        ImGui::EndMenu();
    }

    // Right-aligned status: stored entries and frame rate.
    char status[64];
    std::snprintf(status, sizeof(status), "%zu entries  |  %.0f FPS", store_.size(),
                  static_cast<double>(ImGui::GetIO().Framerate));
    const float statusWidth = ImGui::CalcTextSize(status).x + ImGui::GetStyle().ItemSpacing.x;
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - statusWidth);
    ImGui::TextDisabled("%s", status);

    ImGui::EndMainMenuBar();
}

void App::log(LogLevel level, std::string_view message) {
    LogEntry entry;
    entry.time = Clock::now();
    entry.level = level;
    entry.source = "observex";
    entry.message = std::string(message);
    store_.push(std::move(entry));
}

} // namespace observex
