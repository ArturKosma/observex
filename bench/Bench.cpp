// observex_bench: measures the capture pipeline from a console, without opening the app window.
//
//   observex_bench [--screen N] [--seconds S] [--no-pulse] [--no-preview] [--record FOLDER [--per-second N]]
//
// Lists the screens, runs the real Pipeline on one (the primary by default, or number N from the list)
// and prints its numbers once a second.
//
// Pulse: capture only has work when something on the screen changes. By default the bench keeps an
// invisible 2 x 2 pixel window in the screen's bottom-right corner that presents a new frame every refresh,
// like a game, so the screen changes at its full refresh rate. The window never takes focus and ignores
// the mouse. --no-pulse measures whatever the screen shows on its own.
//
// Preview: by default a second GPU device takes every new image the way the UI does (copy, then build
// the smaller versions), so the cost of showing the screen is included. --no-preview measures capture alone.
//
// Record: --record saves images into FOLDER the way the app's Record button does, N per second (default 1),
// so its cost on capture shows in the numbers. The pulse is near-black, so most saved images show the screen
// as it is.

#include "engine/Gpu.h"
#include "engine/ImageChannel.h"
#include "engine/Pipeline.h"
#include "engine/Recorder.h"
#include "engine/ScreenCapture.h"

#include <dwmapi.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <memory>
#include <thread>
#include <vector>

using namespace observex;

namespace {

struct Options {
    int screen = -1; // Index into the screen list; -1 for the primary screen.
    int seconds = 5;
    bool pulse = true;
    bool preview = true;
    const char* recordFolder = nullptr;
    unsigned recordPerSecond = 1; // As the app records.
};

bool parse(int argc, char** argv, Options& options) {
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--screen") == 0 && i + 1 < argc) {
            options.screen = std::atoi(argv[++i]);
        } else if (std::strcmp(argv[i], "--seconds") == 0 && i + 1 < argc) {
            options.seconds = std::atoi(argv[++i]);
        } else if (std::strcmp(argv[i], "--no-pulse") == 0) {
            options.pulse = false;
        } else if (std::strcmp(argv[i], "--no-preview") == 0) {
            options.preview = false;
        } else if (std::strcmp(argv[i], "--record") == 0 && i + 1 < argc) {
            options.recordFolder = argv[++i];
        } else if (std::strcmp(argv[i], "--per-second") == 0 && i + 1 < argc) {
            options.recordPerSecond = static_cast<unsigned>(std::atoi(argv[++i]));
        } else {
            std::printf("usage: observex_bench [--screen N] [--seconds S] [--no-pulse] [--no-preview] "
                        "[--record FOLDER [--per-second N]]\n");
            return false;
        }
    }
    return options.seconds > 0 && options.recordPerSecond > 0;
}

// Presents a 2 x 2 pixel window in the corner of `bounds` once per screen refresh until stopped, the way a
// game presents its frames. It alternates between two almost identical near-black colours, so it cannot be
// seen, but every present is a screen update. It never takes focus and ignores the mouse.
void runPulse(IDXGIAdapter1* adapter, RECT bounds, std::stop_token stop) {
    constexpr int kSize = 2;
    const HINSTANCE instance = GetModuleHandleW(nullptr);
    WNDCLASSW windowClass{};
    windowClass.lpfnWndProc = [](HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) -> LRESULT {
        return message == WM_NCHITTEST ? HTTRANSPARENT : DefWindowProcW(hwnd, message, wParam, lParam);
    };
    windowClass.hInstance = instance;
    windowClass.lpszClassName = L"observex_bench_pulse";
    RegisterClassW(&windowClass);

    const HWND hwnd =
        CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, windowClass.lpszClassName, L"", WS_POPUP,
                        bounds.right - kSize, bounds.bottom - kSize, kSize, kSize, nullptr, nullptr, instance, nullptr);
    try {
        const GpuDevice gpu = createGpuDevice(adapter);
        ComPtr<IDXGIFactory2> factory;
        check(adapter->GetParent(IID_PPV_ARGS(&factory)), "IDXGIFactory2");
        DXGI_SWAP_CHAIN_DESC1 desc{};
        desc.Width = kSize;
        desc.Height = kSize;
        desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        desc.BufferCount = 2;
        desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        ComPtr<IDXGISwapChain1> swapChain;
        check(factory->CreateSwapChainForHwnd(gpu.device.Get(), hwnd, &desc, nullptr, nullptr, &swapChain),
              "CreateSwapChainForHwnd");
        ComPtr<ID3D11Texture2D> buffer;
        check(swapChain->GetBuffer(0, IID_PPV_ARGS(&buffer)), "GetBuffer");
        ComPtr<ID3D11RenderTargetView> target;
        check(gpu.device->CreateRenderTargetView(buffer.Get(), nullptr, &target), "CreateRenderTargetView");
        ShowWindow(hwnd, SW_SHOWNOACTIVATE);

        const float colors[2][4] = {{0.0f, 0.0f, 0.0f, 1.0f}, {1.0f / 255.0f, 1.0f / 255.0f, 1.0f / 255.0f, 1.0f}};
        for (unsigned i = 0; !stop.stop_requested(); ++i) {
            ID3D11RenderTargetView* view = target.Get();
            gpu.context->OMSetRenderTargets(1, &view, nullptr);
            gpu.context->ClearRenderTargetView(view, colors[i & 1]);
            swapChain->Present(1, 0);
            MSG message;
            while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
                DispatchMessageW(&message);
            }
        }
    } catch (const std::exception& e) {
        std::printf("Pulse stopped: %s\n", e.what());
    }
    DestroyWindow(hwnd);
    UnregisterClassW(windowClass.lpszClassName, instance);
}

// What the UI does with each new image: copy it out of the preview channel and build the smaller versions.
class PreviewConsumer {
public:
    PreviewConsumer(IDXGIAdapter1* adapter, ImageChannel& channel)
        : gpu_(createGpuDevice(adapter)), reader_(gpu_.device.Get(), channel) {
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = channel.width();
        desc.Height = channel.height();
        desc.MipLevels = 0;
        desc.ArraySize = 1;
        desc.Format = ScreenCapture::kImageFormat;
        desc.SampleDesc.Count = 1;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
        desc.MiscFlags = D3D11_RESOURCE_MISC_GENERATE_MIPS;
        check(gpu_.device->CreateTexture2D(&desc, nullptr, &texture_), "CreateTexture2D");
        check(gpu_.device->CreateShaderResourceView(texture_.Get(), nullptr, &view_), "CreateShaderResourceView");
    }

    void poll() {
        if (reader_.copyNewImage(gpu_.context.Get(), texture_.Get())) {
            gpu_.context->GenerateMips(view_.Get());
            gpu_.context->Flush(); // The UI's Present does this; without one the work would sit in the queue.
        }
    }

private:
    GpuDevice gpu_;
    ImageReader reader_;
    ComPtr<ID3D11Texture2D> texture_;
    ComPtr<ID3D11ShaderResourceView> view_;
};

} // namespace

int main(int argc, char** argv) {
    Options options;
    if (!parse(argc, argv, options)) {
        return 2;
    }
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2); // Required by DuplicateOutput1.

    try {
        const ComPtr<IDXGIAdapter1> gpu = findPrimaryGpu();
        const std::vector<Screen> screens = listScreens(gpu.Get());
        std::printf("Screens:\n");
        int chosen = options.screen;
        for (std::size_t i = 0; i < screens.size(); ++i) {
            const Screen& screen = screens[i];
            std::printf("  %zu  %-14s %-24s %5ld x %-5ld %4u Hz%s\n", i, screen.name.c_str(), screen.monitor.c_str(),
                        screen.bounds.right - screen.bounds.left, screen.bounds.bottom - screen.bounds.top,
                        screen.refreshHz, screen.primary ? "  (primary)" : "");
            if (options.screen < 0 && screen.primary) {
                chosen = static_cast<int>(i);
            }
        }
        if (chosen < 0 || chosen >= static_cast<int>(screens.size())) {
            std::printf("No such screen.\n");
            return 2;
        }
        const Screen& screen = screens[static_cast<std::size_t>(chosen)];

        Pipeline pipeline;
        pipeline.setPreviewEnabled(options.preview);
        if (!pipeline.start(screen)) {
            std::printf("%s\n", pipeline.error().c_str());
            return 1;
        }
        {
            std::unique_ptr<PreviewConsumer> consumer;
            if (options.preview) {
                consumer = std::make_unique<PreviewConsumer>(gpu.Get(), *pipeline.preview());
            }
            std::unique_ptr<Recorder> recorder;
            if (options.recordFolder != nullptr) {
                recorder = std::make_unique<Recorder>(screen, *pipeline.recording(), options.recordFolder, 1);
                pipeline.setRecordingRate(options.recordPerSecond);
            }
            std::jthread pulse;
            if (options.pulse) {
                pulse = std::jthread([&](std::stop_token stop) { runPulse(gpu.Get(), screen.bounds, stop); });
            }

            std::printf("\nObserving %s (%s) for %d s (pulse %s, preview %s, recording %s)\n", screen.monitor.c_str(),
                        screen.name.c_str(), options.seconds, options.pulse ? "on" : "off",
                        options.preview ? "on" : "off", recorder ? "on" : "off");
            std::printf("  images/s  merged/s  latency avg / max ms  work avg / max ms\n");

            using Clock = std::chrono::steady_clock;
            const Clock::time_point start = Clock::now();
            Clock::time_point nextPrint =
                start + std::chrono::milliseconds(1500); // Half a second after a stats update.
            std::uint64_t lastMerged = 0;
            double rateSum = 0.0;
            double latencySum = 0.0;
            int samples = 0;
            while (Clock::now() - start < std::chrono::seconds(options.seconds) + std::chrono::milliseconds(600)) {
                if (consumer) {
                    consumer->poll();
                }
                DwmFlush(); // Pace like a UI synced to the screen.

                if (Clock::now() >= nextPrint) {
                    nextPrint += std::chrono::seconds(1);
                    const PipelineStats& stats = pipeline.stats();
                    std::printf("  %8.1f  %8llu  %8.2f / %-8.2f  %8.3f / %.3f\n",
                                static_cast<double>(stats.imagesPerSecond),
                                static_cast<unsigned long long>(stats.mergedUpdates - lastMerged),
                                static_cast<double>(stats.latencyAvgMs), static_cast<double>(stats.latencyMaxMs),
                                static_cast<double>(stats.workAvgMs), static_cast<double>(stats.workMaxMs));
                    lastMerged = stats.mergedUpdates;
                    rateSum += stats.imagesPerSecond;
                    latencySum += stats.latencyAvgMs;
                    ++samples;
                }
                if (pipeline.state() == Pipeline::State::Failed) {
                    std::printf("%s\n", pipeline.error().c_str());
                    return 1;
                }
            }
            if (samples > 0) {
                std::printf("Average: %.1f images/s (screen refresh %u Hz), %.2f ms latency\n", rateSum / samples,
                            screen.refreshHz, latencySum / samples);
            }
            if (recorder) {
                pipeline.setRecordingRate(0);
                std::this_thread::sleep_for(std::chrono::milliseconds(300)); // Let the last image finish saving.
                if (recorder->failed()) {
                    std::printf("Recording failed: %s\n", recorder->error().c_str());
                }
                std::printf("Recorded %u images into %s\n", recorder->saved(), options.recordFolder);
                recorder.reset();
            }
        } // The consumer and recorder go before the pipeline frees its channels.
        pipeline.stop();
    } catch (const std::exception& e) {
        std::printf("%s\n", e.what());
        return 1;
    }
    return 0;
}
