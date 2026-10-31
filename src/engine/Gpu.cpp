#include "engine/Gpu.h"

#include <algorithm>
#include <cstdio>
#include <iterator>
#include <stdexcept>
#include <utility>

namespace observex {

namespace {

const char* reason(HRESULT result) {
    switch (result) {
    case E_ACCESSDENIED:
        return "access denied; the secure desktop (UAC prompt or lock screen) may be showing";
    case DXGI_ERROR_NOT_CURRENTLY_AVAILABLE:
        return "too many apps are capturing the screen at once";
    case DXGI_ERROR_UNSUPPORTED:
        return "not supported by this GPU or driver";
    case DXGI_ERROR_SESSION_DISCONNECTED:
        return "the remote desktop session is disconnected";
    case DXGI_ERROR_DEVICE_REMOVED:
        return "the GPU was removed or reset";
    default:
        return nullptr;
    }
}

} // namespace

void check(HRESULT result, const char* what) {
    if (SUCCEEDED(result)) {
        return;
    }
    char text[256];
    const char* why = reason(result);
    std::snprintf(text, sizeof(text), "%s failed (0x%08lX)%s%s", what, static_cast<unsigned long>(result),
                  why != nullptr ? ": " : "", why != nullptr ? why : "");
    throw std::runtime_error(text);
}

std::string toUtf8(const wchar_t* text) {
    const int size = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
    if (size <= 1) {
        return {};
    }
    std::string result(static_cast<std::size_t>(size - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text, -1, result.data(), size, nullptr, nullptr);
    return result;
}

ComPtr<IDXGIAdapter1> findPrimaryGpu() {
    ComPtr<IDXGIFactory1> factory;
    check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)), "CreateDXGIFactory1");

    const HMONITOR primary = MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
    ComPtr<IDXGIAdapter1> adapter;
    for (UINT a = 0; factory->EnumAdapters1(a, &adapter) != DXGI_ERROR_NOT_FOUND; ++a) {
        ComPtr<IDXGIOutput> output;
        for (UINT o = 0; adapter->EnumOutputs(o, &output) != DXGI_ERROR_NOT_FOUND; ++o) {
            DXGI_OUTPUT_DESC desc{};
            if (SUCCEEDED(output->GetDesc(&desc)) && desc.Monitor == primary) {
                return adapter;
            }
        }
    }
    throw std::runtime_error("No GPU drives the primary monitor");
}

std::vector<Screen> listScreens(IDXGIAdapter1* gpu) {
    const HMONITOR primary = MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
    std::vector<Screen> screens;
    ComPtr<IDXGIOutput> output;
    for (UINT o = 0; gpu->EnumOutputs(o, &output) != DXGI_ERROR_NOT_FOUND; ++o) {
        DXGI_OUTPUT_DESC desc{};
        Screen screen;
        if (FAILED(output->GetDesc(&desc)) || !desc.AttachedToDesktop || FAILED(output.As(&screen.output))) {
            continue;
        }
        DEVMODEW mode{};
        mode.dmSize = sizeof(mode);
        if (EnumDisplaySettingsW(desc.DeviceName, ENUM_CURRENT_SETTINGS, &mode)) {
            screen.refreshHz = mode.dmDisplayFrequency;
        }
        screen.adapter = gpu;
        screen.name = toUtf8(desc.DeviceName);
        screen.bounds = desc.DesktopCoordinates;
        screen.primary = desc.Monitor == primary;
        screens.push_back(std::move(screen));
    }
    std::sort(screens.begin(), screens.end(), [](const Screen& a, const Screen& b) {
        return a.bounds.left != b.bounds.left ? a.bounds.left < b.bounds.left : a.bounds.top < b.bounds.top;
    });
    return screens;
}

Screen findOnOwnAdapter(const Screen& screen) {
    DXGI_ADAPTER_DESC1 wanted{};
    check(screen.adapter->GetDesc1(&wanted), "IDXGIAdapter1::GetDesc1");

    ComPtr<IDXGIFactory1> factory;
    check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)), "CreateDXGIFactory1");
    ComPtr<IDXGIAdapter1> adapter;
    for (UINT a = 0; factory->EnumAdapters1(a, &adapter) != DXGI_ERROR_NOT_FOUND; ++a) {
        DXGI_ADAPTER_DESC1 desc{};
        if (FAILED(adapter->GetDesc1(&desc)) || desc.AdapterLuid.LowPart != wanted.AdapterLuid.LowPart ||
            desc.AdapterLuid.HighPart != wanted.AdapterLuid.HighPart) {
            continue;
        }
        for (Screen& found : listScreens(adapter.Get())) {
            if (found.name == screen.name) {
                return std::move(found);
            }
        }
    }
    throw std::runtime_error("The screen " + screen.name + " is no longer connected");
}

GpuDevice createGpuDevice(IDXGIAdapter* adapter) {
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    GpuDevice gpu;
    const auto create = [&](UINT flags) {
        return D3D11CreateDevice(adapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr, flags, levels,
                                 static_cast<UINT>(std::size(levels)), D3D11_SDK_VERSION, &gpu.device, nullptr,
                                 &gpu.context);
    };

    const UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
    HRESULT result = E_FAIL;
#ifndef NDEBUG
    // Fails when the optional "Graphics Tools" Windows feature (the debug layer) is not installed.
    result = create(flags | D3D11_CREATE_DEVICE_DEBUG);
#endif
    if (FAILED(result)) {
        result = create(flags);
    }
    check(result, "D3D11CreateDevice");
    return gpu;
}

} // namespace observex
