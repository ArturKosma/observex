#include "engine/MouseWatch.h"

#include <windows.h>

#include <cstdio>
#include <future>
#include <stdexcept>

namespace observex {

namespace {

constexpr wchar_t kWindowClass[] = L"ObservexMouseWatch";

std::int64_t qpcNow() {
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return now.QuadPart;
}

// The watch's thread's: what it knows between reports.
struct Watching {
    const MouseWatch::Sink* sink = nullptr;
    std::uint8_t held = 0;
};

void read(Watching& watching, HRAWINPUT handle) {
    RAWINPUT raw{};
    UINT size = sizeof(raw);
    if (GetRawInputData(handle, RID_INPUT, &raw, &size, sizeof(RAWINPUTHEADER)) == static_cast<UINT>(-1) ||
        raw.header.dwType != RIM_TYPEMOUSE) {
        return;
    }
    const RAWMOUSE& mouse = raw.data.mouse;
    MouseInput input;
    input.qpc = qpcNow();
    if ((mouse.usFlags & MOUSE_MOVE_ABSOLUTE) == 0) {
        input.dx = static_cast<int>(mouse.lLastX);
        input.dy = static_cast<int>(mouse.lLastY);
    }
    input.buttons = mouse.usButtonFlags;
    static constexpr struct {
        USHORT down;
        USHORT up;
        std::uint8_t bit;
    } kButtons[] = {{RI_MOUSE_LEFT_BUTTON_DOWN, RI_MOUSE_LEFT_BUTTON_UP, 1},
                    {RI_MOUSE_RIGHT_BUTTON_DOWN, RI_MOUSE_RIGHT_BUTTON_UP, 2},
                    {RI_MOUSE_MIDDLE_BUTTON_DOWN, RI_MOUSE_MIDDLE_BUTTON_UP, 4}};
    for (const auto& button : kButtons) {
        if ((mouse.usButtonFlags & button.down) != 0) {
            watching.held = static_cast<std::uint8_t>(watching.held | button.bit);
        }
        if ((mouse.usButtonFlags & button.up) != 0) {
            watching.held = static_cast<std::uint8_t>(watching.held & ~button.bit);
        }
    }
    input.held = watching.held;
    if ((mouse.usButtonFlags & RI_MOUSE_WHEEL) != 0) {
        input.wheel = static_cast<std::int16_t>(static_cast<SHORT>(mouse.usButtonData));
    }
    POINT cursor{};
    if (GetCursorPos(&cursor)) {
        input.cursorX = cursor.x;
        input.cursorY = cursor.y;
    }
    (*watching.sink)(input);
}

LRESULT CALLBACK windowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    if (message == WM_INPUT) {
        if (auto* watching = reinterpret_cast<Watching*>(GetWindowLongPtrW(window, GWLP_USERDATA))) {
            read(*watching, reinterpret_cast<HRAWINPUT>(lParam));
        }
    }
    return DefWindowProcW(window, message, wParam, lParam); // Frees WM_INPUT's data too.
}

std::string lastError(const char* what) {
    return std::string(what) + " failed (" + std::to_string(GetLastError()) + ")";
}

} // namespace

MouseWatch::MouseWatch(Sink sink) : sink_(std::move(sink)) {
    std::promise<void> ready;
    std::future<void> started = ready.get_future();
    std::string error;
    thread_ = std::thread([this, &error, &ready] { run(&error, &ready); });
    started.wait();
    if (!error.empty()) {
        thread_.join();
        throw std::runtime_error(error);
    }
}

MouseWatch::~MouseWatch() {
    if (thread_.joinable()) {
        PostThreadMessageW(threadId_, WM_QUIT, 0, 0);
        thread_.join();
    }
}

void MouseWatch::run(std::string* error, void* readyPromise) {
    auto& ready = *static_cast<std::promise<void>*>(readyPromise);
    threadId_ = GetCurrentThreadId();
    const HINSTANCE instance = GetModuleHandleW(nullptr);
    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.lpfnWndProc = windowProc;
    windowClass.hInstance = instance;
    windowClass.lpszClassName = kWindowClass;
    if (RegisterClassExW(&windowClass) == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        *error = lastError("RegisterClassEx");
        ready.set_value();
        return;
    }
    // A message-only window: never shown, gets only what is sent to it.
    const HWND window = CreateWindowExW(0, kWindowClass, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, instance, nullptr);
    if (window == nullptr) {
        *error = lastError("CreateWindowEx");
        ready.set_value();
        return;
    }
    Watching watching;
    watching.sink = &sink_;
    // The buttons down already (the raw reports say only what changes).
    watching.held = static_cast<std::uint8_t>(((GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0 ? 1 : 0) |
                                              ((GetAsyncKeyState(VK_RBUTTON) & 0x8000) != 0 ? 2 : 0) |
                                              ((GetAsyncKeyState(VK_MBUTTON) & 0x8000) != 0 ? 4 : 0));
    SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(&watching));
    RAWINPUTDEVICE device{};
    device.usUsagePage = 0x01; // Generic desktop,
    device.usUsage = 0x02;     // mouse;
    device.dwFlags = RIDEV_INPUTSINK; // also while another window is in front.
    device.hwndTarget = window;
    if (!RegisterRawInputDevices(&device, 1, sizeof(device))) {
        *error = lastError("RegisterRawInputDevices");
        DestroyWindow(window);
        ready.set_value();
        return;
    }
    ready.set_value();

    MSG message;
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        DispatchMessageW(&message);
    }

    device.dwFlags = RIDEV_REMOVE;
    device.hwndTarget = nullptr;
    RegisterRawInputDevices(&device, 1, sizeof(device));
    SetWindowLongPtrW(window, GWLP_USERDATA, 0);
    DestroyWindow(window);
}

std::string MouseWatch::pointerSettings() {
    int speed = 0;
    int mouse[3] = {}; // The two thresholds and the acceleration (0: enhance pointer precision off).
    if (!SystemParametersInfoW(SPI_GETMOUSESPEED, 0, &speed, 0) || !SystemParametersInfoW(SPI_GETMOUSE, 0, mouse, 0)) {
        return "unknown";
    }
    char text[160];
    std::snprintf(text, sizeof(text), "pointer speed %d of 20%s, enhance pointer precision %s", speed,
                  speed == 10 ? " (as moved)" : "", mouse[2] != 0 ? "on" : "off");
    return text;
}

} // namespace observex
