#include "app/MainWindow.h"

#include "app/Resources.h"
#include "app/Theme.h"

#include <dwmapi.h>
#include <imgui.h>
#include <imgui_impl_win32.h>

#include <stdexcept>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);

namespace observex {

namespace {

constexpr wchar_t kWindowClass[] = L"observex";
constexpr int kHotkeyId = 1;

// Dark title bar in the app's own colours (Windows 11; older versions keep their default).
void styleTitleBar(HWND hwnd) {
    const BOOL dark = TRUE;
    DwmSetWindowAttribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));
    const COLORREF caption = theme::toColorRef(theme::kBackground);
    const COLORREF border = theme::toColorRef(theme::kBorder);
    const COLORREF text = theme::toColorRef(theme::kTextMuted);
    DwmSetWindowAttribute(hwnd, DWMWA_CAPTION_COLOR, &caption, sizeof(caption));
    DwmSetWindowAttribute(hwnd, DWMWA_BORDER_COLOR, &border, sizeof(border));
    DwmSetWindowAttribute(hwnd, DWMWA_TEXT_COLOR, &text, sizeof(text));
}

} // namespace

MainWindow::MainWindow(const wchar_t* title, ID3D11Device* device, float dpiScale, bool visible) : device_(device) {
    device_->GetImmediateContext(&context_);

    const HINSTANCE instance = GetModuleHandleW(nullptr);
    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.style = CS_CLASSDC;
    windowClass.lpfnWndProc = windowProc;
    windowClass.hInstance = instance;
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    windowClass.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(IDI_OBSERVEX));
    windowClass.hIconSm =
        static_cast<HICON>(LoadImageW(instance, MAKEINTRESOURCEW(IDI_OBSERVEX), IMAGE_ICON,
                                      GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR));
    windowClass.lpszClassName = kWindowClass;
    RegisterClassExW(&windowClass);

    hwnd_ = CreateWindowExW(0, kWindowClass, title, WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                            static_cast<int>(1280 * dpiScale), static_cast<int>(800 * dpiScale), nullptr, nullptr,
                            instance, this);
    if (hwnd_ == nullptr) {
        throw std::runtime_error("Could not create the main window");
    }
    styleTitleBar(hwnd_);

    ComPtr<IDXGIDevice> dxgiDevice;
    check(device_.As(&dxgiDevice), "IDXGIDevice");
    ComPtr<IDXGIAdapter> adapter;
    check(dxgiDevice->GetAdapter(&adapter), "IDXGIDevice::GetAdapter");
    ComPtr<IDXGIFactory2> factory;
    check(adapter->GetParent(IID_PPV_ARGS(&factory)), "IDXGIFactory2");

    DXGI_SWAP_CHAIN_DESC1 desc{};
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = 2;
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    check(factory->CreateSwapChainForHwnd(device_.Get(), hwnd_, &desc, nullptr, nullptr, &swapChain_),
          "CreateSwapChainForHwnd");
    factory->MakeWindowAssociation(hwnd_, DXGI_MWA_NO_ALT_ENTER);
    createBackBufferView();

    if (visible) {
        ShowWindow(hwnd_, SW_SHOWDEFAULT);
        UpdateWindow(hwnd_);
    }
}

MainWindow::~MainWindow() {
    if (hwnd_ != nullptr) {
        if (hotkeyRegistered_) {
            UnregisterHotKey(hwnd_, kHotkeyId);
        }
        DestroyWindow(hwnd_);
    }
    UnregisterClassW(kWindowClass, GetModuleHandleW(nullptr));
}

bool MainWindow::pumpMessages() {
    bool open = true;
    MSG message;
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
        if (message.message == WM_QUIT) {
            open = false;
        }
    }
    return open;
}

bool MainWindow::prepareFrame() {
    if (IsIconic(hwnd_)) {
        return false;
    }
    if (pendingWidth_ != 0 && pendingHeight_ != 0) {
        context_->OMSetRenderTargets(0, nullptr, nullptr);
        backBufferView_.Reset();
        swapChain_->ResizeBuffers(0, pendingWidth_, pendingHeight_, DXGI_FORMAT_UNKNOWN, 0);
        pendingWidth_ = 0;
        pendingHeight_ = 0;
        createBackBufferView();
    }
    return true;
}

void MainWindow::bindBackBuffer() {
    const ImVec4 clear = ImGui::ColorConvertU32ToFloat4(theme::kBackground);
    const float clearColor[4] = {clear.x, clear.y, clear.z, 1.0f};
    ID3D11RenderTargetView* view = backBufferView_.Get();
    context_->OMSetRenderTargets(1, &view, nullptr);
    context_->ClearRenderTargetView(view, clearColor);
}

void MainWindow::present() {
    swapChain_->Present(1, 0);
}

ComPtr<ID3D11Texture2D> MainWindow::backBuffer() const {
    ComPtr<ID3D11Texture2D> texture;
    check(swapChain_->GetBuffer(0, IID_PPV_ARGS(&texture)), "IDXGISwapChain::GetBuffer");
    return texture;
}

bool MainWindow::registerHotkey(UINT modifiers, UINT key) {
    hotkeyRegistered_ = RegisterHotKey(hwnd_, kHotkeyId, modifiers | MOD_NOREPEAT, key) != FALSE;
    return hotkeyRegistered_;
}

bool MainWindow::takeHotkeyPress() {
    if (hotkeyPresses_ == 0) {
        return false;
    }
    --hotkeyPresses_;
    return true;
}

void MainWindow::createBackBufferView() {
    check(device_->CreateRenderTargetView(backBuffer().Get(), nullptr, &backBufferView_), "CreateRenderTargetView");
}

LRESULT CALLBACK MainWindow::windowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    if (ImGui_ImplWin32_WndProcHandler(hwnd, message, wParam, lParam) != 0) {
        return 1;
    }
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(create->lpCreateParams));
    }
    auto* self = reinterpret_cast<MainWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));

    switch (message) {
    case WM_SIZE:
        if (self != nullptr && wParam != SIZE_MINIMIZED) {
            self->pendingWidth_ = LOWORD(lParam);
            self->pendingHeight_ = HIWORD(lParam);
        }
        return 0;
    case WM_HOTKEY:
        if (self != nullptr && wParam == kHotkeyId) {
            ++self->hotkeyPresses_;
        }
        return 0;
    case WM_SYSCOMMAND:
        if ((wParam & 0xfff0) == SC_KEYMENU) {
            return 0; // No Alt menu: Alt is free for the UI.
        }
        break;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    case WM_NCDESTROY:
        if (self != nullptr) {
            self->hwnd_ = nullptr;
        }
        break;
    default:
        break;
    }
    return DefWindowProcW(hwnd, message, wParam, lParam);
}

} // namespace observex
