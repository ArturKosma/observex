#pragma once

#include <d3d11.h>
#include <dxgi1_5.h>
#include <wrl/client.h>

#include <cstdint>
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

// The same GPU as `gpu`, through a new DXGI factory: an adapter object of its own, for a device that must not hold up
// others (see findOnOwnAdapter), when there is no screen to find it by. Throws if the GPU is gone.
ComPtr<IDXGIAdapter1> findOwnAdapter(IDXGIAdapter1* gpu);

// Creates a Direct3D 11 device on `adapter`. Debug builds use the D3D debug layer when it is installed. With `video`,
// the device also works with the GPU's video decoder (Media Foundation; D3D11_CREATE_DEVICE_VIDEO_SUPPORT) and is
// safe to use from Media Foundation's threads too (multithread protected).
GpuDevice createGpuDevice(IDXGIAdapter* adapter, bool video = false);

// How eagerly the GPU takes a part's work when it competes with other work (observex's other parts, the game, a
// training run). The reaction path (capture, the recogniser, the snapper) goes first, the drop finder yields, the rest
// is any program's (the depth finder too: it reads its shared image in place, so lowering it could hold up capture).
// Only the order the GPU takes work in changes, never what it computes. (The user asked, 2026-11-16: the Debug tab
// showed the character network at 11 ms, waiting behind the drop network that had the same top priority, while a
// training run kept the GPU 99% busy.)
enum class GpuPriority : std::uint8_t { Reaction, Normal, Background };

// Gives `device`'s work `priority` on the GPU (IDXGIDevice::SetGPUThreadPriority): Reaction +7, Background -7, Normal
// left as made (0). False if Windows refused it (the device keeps priority 0); never throws.
bool setGpuPriority(ID3D11Device* device, GpuPriority priority);

// Whether priorities are given at all (default on). Off, setGpuPriority leaves every device at 0 and every Detector
// takes the top CUDA priority, as before 2026-11-16: observex_bench --no-gpu-priority, to compare. Set it before any
// part starts.
void useGpuPriorities(bool on);
bool gpuPrioritiesUsed();

} // namespace observex
