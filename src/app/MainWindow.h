#pragma once

#include "engine/Gpu.h"

namespace observex {

// The app's top-level Win32 window and its swap chain.
class MainWindow {
public:
    // Creates and shows the window. `dpiScale` sizes it for the monitor. Throws on failure.
    MainWindow(const wchar_t* title, ID3D11Device* device, float dpiScale);
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
};

} // namespace observex
