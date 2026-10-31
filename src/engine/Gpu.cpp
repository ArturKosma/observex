#include "engine/Gpu.h"

#include <cstdio>
#include <iterator>
#include <stdexcept>

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

CaptureTarget findPrimaryMonitor() {
    ComPtr<IDXGIFactory1> factory;
    check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)), "CreateDXGIFactory1");

    const HMONITOR primary = MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
    ComPtr<IDXGIAdapter1> adapter;
    for (UINT a = 0; factory->EnumAdapters1(a, &adapter) != DXGI_ERROR_NOT_FOUND; ++a) {
        ComPtr<IDXGIOutput> output;
        for (UINT o = 0; adapter->EnumOutputs(o, &output) != DXGI_ERROR_NOT_FOUND; ++o) {
            DXGI_OUTPUT_DESC outputDesc{};
            if (FAILED(output->GetDesc(&outputDesc)) || outputDesc.Monitor != primary) {
                continue;
            }
            DXGI_ADAPTER_DESC1 adapterDesc{};
            adapter->GetDesc1(&adapterDesc);

            CaptureTarget target;
            target.adapter = adapter;
            check(output.As(&target.output), "IDXGIOutput1");
            target.name = toUtf8(outputDesc.DeviceName) + " on " + toUtf8(adapterDesc.Description);
            return target;
        }
    }
    throw std::runtime_error("No GPU output shows the primary monitor");
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
