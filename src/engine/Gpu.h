#pragma once

#include <d3d11.h>
#include <dxgi1_5.h>
#include <wrl/client.h>

#include <string>
#include <vector>

namespace observex {

using Microsoft::WRL::ComPtr;

// Throws std::runtime_error naming the failed call and the reason. For setup code only, never per frame.
void check(HRESULT result, const char* what);

// UTF-16 (Windows API text) to UTF-8.
std::string toUtf8(const wchar_t* text);

// The GPU that drives the primary monitor. observex captures only the screens on this GPU, so the UI and
// the pipeline can share images without copying them through the CPU. Throws if there is none.
ComPtr<IDXGIAdapter1> findPrimaryGpu();

// A monitor that can be captured.
struct Screen {
    ComPtr<IDXGIAdapter1> adapter; // The GPU it is connected to.
    ComPtr<IDXGIOutput1> output;
    std::string name;    // Windows' name for it, e.g. "\\.\DISPLAY1". Stays the same while it is connected.
    std::string monitor; // The monitor's own model name, e.g. "Odyssey G7", or "Display 1" if it reports none.
    RECT bounds{};       // Where it sits on the desktop, in pixels.
    unsigned refreshHz = 0;
    bool primary = false;
};

// The screens connected to `gpu`, left to right as they are arranged on the desktop.
std::vector<Screen> listScreens(IDXGIAdapter1* gpu);

// The same screen, found again through a new DXGI factory, so a device made from it has an adapter object
// of its own. Devices made from one shared adapter object hold each other up: next to a device presenting
// every refresh, capture fell from 240 to 24 images per second. Throws if the screen is gone.
Screen findOnOwnAdapter(const Screen& screen);

struct GpuDevice {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
};

// Creates a Direct3D 11 device on `adapter`. Debug builds use the D3D debug layer when it is installed.
GpuDevice createGpuDevice(IDXGIAdapter* adapter);

} // namespace observex
