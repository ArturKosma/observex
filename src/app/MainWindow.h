#pragma once

#include "engine/Gpu.h"

#include <array>

namespace observex {

// The app's top-level Win32 window and its swap chain.
class MainWindow {
public:
    // Creates the window, and shows it unless `visible` is false (snapshot mode). `dpiScale` sizes it for
    // the monitor. Throws on failure.
    MainWindow(const wchar_t* title, ID3D11Device* device, float dpiScale, bool visible);
    ~MainWindow();

    MainWindow(const MainWindow&) = delete;
    MainWindow& operator=(const MainWindow&) = delete;

    HWND handle() const { return hwnd_; }

    // Handles queued window messages. Returns false once the window has been closed.
    bool pumpMessages();

    // Applies a pending resize. Returns false while the window is minimized; skip drawing then.
    bool prepareFrame();

    // Binds the back buffer as the render target and clears it.
    void bindBackBuffer();

    // Shows the frame, synced to the monitor refresh.
    void present();

    // The image being drawn this frame (for snapshots).
    ComPtr<ID3D11Texture2D> backBuffer() const;

    // Registers a key combination that reaches observex even while another app (a game) has focus, e.g.
    // registerHotkey(0, MOD_CONTROL | MOD_ALT, 'R'). `id` (0 to kMaxHotkeys - 1) tells them apart. Returns false if
    // another app already uses it.
    static constexpr int kMaxHotkeys = 4;
    bool registerHotkey(int id, UINT modifiers, UINT key);

    // True once for each press of hotkey `id` since the last call.
    bool takeHotkeyPress(int id);

private:
    static LRESULT CALLBACK windowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);
    void createBackBufferView();

    HWND hwnd_ = nullptr;
    ComPtr<ID3D11Device> device_;
    ComPtr<ID3D11DeviceContext> context_;
    ComPtr<IDXGISwapChain1> swapChain_;
    ComPtr<ID3D11RenderTargetView> backBufferView_;
    UINT pendingWidth_ = 0;
    UINT pendingHeight_ = 0;
    std::array<bool, kMaxHotkeys> hotkeyRegistered_{};
    std::array<unsigned, kMaxHotkeys> hotkeyPresses_{};
};

} // namespace observex
