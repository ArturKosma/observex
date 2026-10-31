#pragma once

#include <d3d11.h>
#include <dxgi1_5.h>
#include <wrl/client.h>

#include <string>

namespace observex {

using Microsoft::WRL::ComPtr;

// Throws std::runtime_error naming the failed call and the reason. For setup code only, never per frame.
void check(HRESULT result, const char* what);

// UTF-16 (Windows API text) to UTF-8.
std::string toUtf8(const wchar_t* text);

// The monitor to capture, and the GPU it is connected to.
struct CaptureTarget {
    ComPtr<IDXGIAdapter1> adapter;
    ComPtr<IDXGIOutput1> output;
    std::string name; // e.g. "\\.\DISPLAY1 on NVIDIA GeForce RTX 4080"
};

// Finds the primary monitor and its GPU. Throws if there is none.
CaptureTarget findPrimaryMonitor();

struct GpuDevice {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
};

// Creates a Direct3D 11 device on `adapter`. Debug builds use the D3D debug layer when it is installed.
GpuDevice createGpuDevice(IDXGIAdapter* adapter);

} // namespace observex
