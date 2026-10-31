#pragma once

#include "app/LogPanel.h"
#include "core/DemoSource.h"
#include "core/LogStore.h"

#include <string_view>

struct GLFWwindow;

namespace observex {

// Owns the window, the ImGui context, the log store and the panels.
class App {
public:
    App();
    ~App();

    App(const App&) = delete;
    App& operator=(const App&) = delete;

    // Opens the window and runs until it is closed. Returns the process exit code.
    int run();

private:
    bool init();
    void shutdown();

    void drawFrame();
    void drawMenuBar();

    // Adds an entry from observex itself (source "observex") to the store.
    void log(LogLevel level, std::string_view message);

    GLFWwindow* window_ = nullptr;
    bool glfwReady_ = false;
    bool imguiReady_ = false;

    // Declared in this order so the demo thread stops before the store it writes to is destroyed.
    LogStore store_;
    DemoSource demo_;
    LogPanel logPanel_;

    bool showLogPanel_ = true;
    bool showImGuiDemo_ = false;
};

} // namespace observex
