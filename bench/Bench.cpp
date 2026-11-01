// observex_bench: measures the capture pipeline from a console, without opening the app window.
//
//   observex_bench [--screen N] [--seconds S] [--no-pulse] [--no-preview] [--record FOLDER [--every S]]
//                  [--model FOLDER]
//   observex_bench --model FOLDER --images FOLDER --out FILE
//   observex_bench --track LOG
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
// Record: --record saves images into FOLDER the way the app's Record button does, one every S seconds (default 3,
// like the app), so its cost on capture shows in the numbers. The pulse is near-black, so most saved images show the screen
// as it is. With --model too, it also writes the live log (e.g. monster-live.txt, see engine/LiveLog) like the app,
// and marks once halfway, as if Ctrl+Alt+M were pressed.
//
// Model: --model runs live recognition with the model in FOLDER (e.g. models/monster) the way the app does while
// observing, and adds its numbers: images recognised per second, milliseconds per image, things found.
// With --images it looks at the image files of a recording instead of the screen and writes what it found to FILE
// in the marks format (like tools/train/guess.py's <category>-guesses.txt), to compare with the Python model.
//
// Track: --track follows the things of a live log (a recording's monster-live.txt) with the Tracker, look by look as
// the recogniser does, and prints how steady the boxes are with and without it, and what following costs. No GPU.

#include "core/Marks.h"
#include "core/StatWindow.h"
#include "core/Tracker.h"
#include "engine/Detector.h"
#include "engine/Gpu.h"
#include "engine/ImageChannel.h"
#include "engine/ImageFile.h"
#include "engine/LiveLog.h"
#include "engine/Pipeline.h"
#include "engine/Recognizer.h"
#include "engine/Recorder.h"
#include "engine/ScreenCapture.h"

#include <dwmapi.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <sstream>
#include <string>
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
    unsigned recordEvery = 3; // Seconds between saved images, as the app records.
    const char* model = nullptr;
    const char* images = nullptr;
    const char* out = nullptr;
    const char* track = nullptr;
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
        } else if (std::strcmp(argv[i], "--every") == 0 && i + 1 < argc) {
            options.recordEvery = static_cast<unsigned>(std::atoi(argv[++i]));
        } else if (std::strcmp(argv[i], "--model") == 0 && i + 1 < argc) {
            options.model = argv[++i];
        } else if (std::strcmp(argv[i], "--images") == 0 && i + 1 < argc) {
            options.images = argv[++i];
        } else if (std::strcmp(argv[i], "--out") == 0 && i + 1 < argc) {
            options.out = argv[++i];
        } else if (std::strcmp(argv[i], "--track") == 0 && i + 1 < argc) {
            options.track = argv[++i];
        } else {
            std::printf("usage: observex_bench [--screen N] [--seconds S] [--no-pulse] [--no-preview] "
                        "[--record FOLDER [--every S]] [--model FOLDER]\n"
                        "       observex_bench --model FOLDER --images FOLDER --out FILE\n"
                        "       observex_bench --track LOG\n");
            return false;
        }
    }
    if (options.images != nullptr && (options.model == nullptr || options.out == nullptr)) {
        std::printf("--images needs --model and --out\n");
        return false;
    }
    return options.seconds > 0 && options.recordEvery > 0;
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

// --images: runs the Detector on each image file of a recording and writes what it found as marks.
int detectImages(const Options& options) {
    namespace fs = std::filesystem;
    std::vector<fs::path> files;
    for (const fs::directory_entry& entry : fs::directory_iterator(options.images)) {
        const std::string extension = entry.path().extension().string();
        if (extension == ".jpg" || extension == ".jpeg" || extension == ".png") {
            files.push_back(entry.path());
        }
    }
    std::sort(files.begin(), files.end());
    if (files.empty()) {
        std::printf("No images in %s\n", options.images);
        return 1;
    }
    const ImageData first = readImageFile(files.front());
    const ComPtr<IDXGIAdapter1> adapter = findPrimaryGpu();
    const GpuDevice gpu = createGpuDevice(adapter.Get());
    std::atomic<float> progress{0.0f};
    std::stop_source never;
    const auto loadStart = std::chrono::steady_clock::now();
    Detector detector(gpu.device.Get(), adapter.Get(), options.model, first.width, first.height, never.get_token(),
                      progress);
    std::printf("Model ready in %.1f s (input %u x %u)\n",
                std::chrono::duration<double>(std::chrono::steady_clock::now() - loadStart).count(),
                detector.modelSize(), detector.modelSize());

    ImageMarks marks;
    double totalMs = 0.0;
    std::vector<Detection> found(Found::kMax);
    for (const fs::path& file : files) {
        const ImageData image = readImageFile(file);
        if (image.width != first.width || image.height != first.height) {
            std::printf("Skipped %s: another size\n", file.filename().string().c_str());
            continue;
        }
        gpu.context->UpdateSubresource(detector.input(), 0, nullptr, image.bgra.data(), image.width * 4, 0);
        const auto start = std::chrono::steady_clock::now();
        const int count = detector.detect(gpu.context.Get(), Recognizer::kMinSure, found.data(), Found::kMax);
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        if (count < 0) {
            std::printf("The GPU failed on %s\n", file.filename().string().c_str());
            return 1;
        }
        if (file != files.front()) {
            totalMs += ms; // The first run includes TensorRT's warm-up.
        }
        std::vector<Mark>& imageMarks = marks[file.filename().string()];
        for (int i = 0; i < count; ++i) {
            const Detection& d = found[static_cast<std::size_t>(i)];
            Mark mark;
            mark.box = {static_cast<int>(std::lround(d.x)), static_cast<int>(std::lround(d.y)),
                        std::max(1, static_cast<int>(std::lround(d.width))),
                        std::max(1, static_cast<int>(std::lround(d.height)))};
            mark.guess = d.sure;
            imageMarks.push_back(mark);
        }
    }
    std::ofstream(options.out, std::ios::binary) << formatMarks(marks, "observex_bench --images");
    std::printf("Looked at %zu images: %.2f ms per image (squeeze, model and boxes; after the first). Wrote %s\n",
                files.size(), files.size() > 1 ? totalMs / static_cast<double>(files.size() - 1) : 0.0, options.out);
    return 0;
}

// --track: follows the things of a live log with the Tracker and prints how steady the boxes are.
int replayTrack(const Options& options) {
    std::ifstream in(options.track, std::ios::binary);
    if (!in) {
        std::printf("Cannot read %s\n", options.track);
        return 1;
    }
    struct Life {
        double firstMs;
        double lastMs;
    };
    Tracker tracker;
    std::vector<Detection> found;
    std::array<Tracked, Found::kMax> followed;
    std::map<std::uint32_t, Life> lives; // Per identity: when it was first and last shown.
    StatWindow cost;
    int looks = 0;
    double firstMs = 0.0;
    double lastMs = 0.0;
    long long previousFound = 0;
    long long previousFollowed = 0;
    long long foundChanges = 0; // Boxes appearing or vanishing between looks, as the model alone shows them.
    long long followedChanges = 0;
    std::string line;
    while (std::getline(in, line)) {
        if (line.rfind("look ", 0) != 0) {
            continue;
        }
        std::istringstream words(line.substr(5));
        unsigned long long image = 0;
        double timeMs = 0.0;
        words >> image >> timeMs;
        found.clear();
        Detection box;
        while (words >> box.sure >> box.x >> box.y >> box.width >> box.height) {
            if (box.sure >= Recognizer::kMinSure) {
                found.push_back(box);
            }
        }
        const auto start = std::chrono::steady_clock::now();
        const int count = tracker.update(timeMs, found.data(), static_cast<int>(found.size()), followed.data(),
                                         static_cast<int>(followed.size()));
        cost.add(static_cast<float>(
            std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - start).count()));
        if (looks == 0) {
            firstMs = timeMs;
        } else {
            foundChanges += std::llabs(static_cast<long long>(found.size()) - previousFound);
            followedChanges += std::llabs(count - previousFollowed);
        }
        previousFound = static_cast<long long>(found.size());
        previousFollowed = count;
        for (int i = 0; i < count; ++i) {
            const auto [life, added] =
                lives.try_emplace(followed[static_cast<std::size_t>(i)].id, Life{timeMs, timeMs});
            life->second.lastMs = timeMs;
        }
        lastMs = timeMs;
        ++looks;
    }
    if (looks < 2 || lastMs <= firstMs) {
        std::printf("Not enough looks in %s\n", options.track);
        return 1;
    }
    const double seconds = (lastMs - firstMs) / 1000.0;
    int brief = 0;
    for (const auto& [id, life] : lives) {
        brief += life.lastMs - life.firstMs < 50.0 ? 1 : 0;
    }
    const StatWindow::Summary summary = cost.take();
    std::printf("%d looks over %.1f s (%.0f a second)\n", looks, seconds, looks / seconds);
    std::printf("Boxes appearing or vanishing: %.1f a second from the model alone, %.1f followed\n",
                static_cast<double>(foundChanges) / seconds, static_cast<double>(followedChanges) / seconds);
    std::printf("Things followed: %zu, of them shown under 50 ms: %d\n", lives.size(), brief);
    std::printf("Following: %.2f us per look on average, %.2f us at most\n", static_cast<double>(summary.average),
                static_cast<double>(summary.max));
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    Options options;
    if (!parse(argc, argv, options)) {
        return 2;
    }
    if (options.track != nullptr) {
        return replayTrack(options);
    }
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2); // Required by DuplicateOutput1.
    if (options.images != nullptr) {
        try {
            return detectImages(options);
        } catch (const std::exception& e) {
            std::printf("%s\n", e.what());
            return 1;
        }
    }

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
            std::unique_ptr<Recognizer> recognizer;
            if (options.model != nullptr) {
                recognizer = std::make_unique<Recognizer>(screen, *pipeline.recognition(), options.model);
                pipeline.setRecognitionEnabled(true);
                std::printf("Loading the model");
                while (recognizer->state() == Recognizer::State::Starting) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(500));
                    if (recognizer->preparing() > 0.0f) {
                        std::printf(" %.0f%%", static_cast<double>(recognizer->preparing() * 100.0f));
                    } else {
                        std::printf(".");
                    }
                }
                std::printf("\n");
                if (recognizer->state() == Recognizer::State::Failed) {
                    std::printf("%s\n", recognizer->error().c_str());
                    return 1;
                }
            }
            // Recording with a model also writes the live log, as the app does (once the model is ready).
            std::shared_ptr<LiveLog> log;
            std::unique_ptr<Recorder> recorder;
            if (options.recordFolder != nullptr) {
                if (recognizer) {
                    const std::filesystem::path model(options.model);
                    char about[512];
                    std::snprintf(about, sizeof(about), "Model %s, screen %s (%s), %u x %u. Written by observex_bench.",
                                  toUtf8((model / L"model.onnx").c_str()).c_str(), screen.monitor.c_str(),
                                  screen.name.c_str(), pipeline.width(), pipeline.height());
                    log = std::make_shared<LiveLog>(
                        std::filesystem::path(options.recordFolder) / (model.filename().string() + "-live.txt"), about);
                    recognizer->setLog(log);
                }
                recorder = std::make_unique<Recorder>(screen, *pipeline.recording(), options.recordFolder, 1, log);
                pipeline.setRecordingInterval(options.recordEvery);
            }
            bool marked = false;
            std::jthread pulse;
            if (options.pulse) {
                pulse = std::jthread([&](std::stop_token stop) { runPulse(gpu.Get(), screen.bounds, stop); });
            }

            std::printf("\nObserving %s (%s) for %d s (pulse %s, preview %s, recording %s, recognition %s)\n",
                        screen.monitor.c_str(), screen.name.c_str(), options.seconds, options.pulse ? "on" : "off",
                        options.preview ? "on" : "off", recorder ? "on" : "off", recognizer ? "on" : "off");
            std::printf("  images/s  merged/s  latency avg / max ms  work avg / max ms%s\n",
                        recognizer ? "  recognised/s  recognise avg / max ms  found" : "");

            using Clock = std::chrono::steady_clock;
            const Clock::time_point start = Clock::now();
            Clock::time_point nextPrint =
                start + std::chrono::milliseconds(1500); // Half a second after a stats update.
            std::uint64_t lastMerged = 0;
            double rateSum = 0.0;
            double latencySum = 0.0;
            double recognisedSum = 0.0;
            double recogniseMsSum = 0.0;
            int samples = 0;
            while (Clock::now() - start < std::chrono::seconds(options.seconds) + std::chrono::milliseconds(600)) {
                if (consumer) {
                    consumer->poll();
                }
                DwmFlush(); // Pace like a UI synced to the screen.
                if (log && !marked && Clock::now() - start >= std::chrono::milliseconds(options.seconds * 500)) {
                    log->mark(1); // Halfway, as if the user pressed Ctrl+Alt+M.
                    pipeline.recordNow();
                    marked = true;
                }

                if (Clock::now() >= nextPrint) {
                    nextPrint += std::chrono::seconds(1);
                    const PipelineStats& stats = pipeline.stats();
                    std::printf("  %8.1f  %8llu  %8.2f / %-8.2f  %8.3f / %-8.3f",
                                static_cast<double>(stats.imagesPerSecond),
                                static_cast<unsigned long long>(stats.mergedUpdates - lastMerged),
                                static_cast<double>(stats.latencyAvgMs), static_cast<double>(stats.latencyMaxMs),
                                static_cast<double>(stats.workAvgMs), static_cast<double>(stats.workMaxMs));
                    if (recognizer) {
                        const RecognizerStats& seen = recognizer->stats();
                        std::printf("  %12.1f  %8.2f / %-8.2f  %5d", static_cast<double>(seen.imagesPerSecond),
                                    static_cast<double>(seen.workAvgMs), static_cast<double>(seen.workMaxMs),
                                    recognizer->found().count);
                        recognisedSum += seen.imagesPerSecond;
                        recogniseMsSum += seen.workAvgMs;
                    }
                    std::printf("\n");
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
                if (recognizer) {
                    std::printf("Recognition: %.1f images/s, %.2f ms per image\n", recognisedSum / samples,
                                recogniseMsSum / samples);
                }
            }
            if (recognizer) {
                pipeline.setRecognitionEnabled(false);
                recognizer.reset();
            }
            if (recorder) {
                pipeline.setRecordingInterval(0);
                std::this_thread::sleep_for(std::chrono::milliseconds(300)); // Let the last image finish saving.
                if (recorder->failed()) {
                    std::printf("Recording failed: %s\n", recorder->error().c_str());
                }
                std::printf("Recorded %u images (%.1f MB) into %s\n", recorder->saved(),
                            static_cast<double>(recorder->savedBytes()) / (1024.0 * 1024.0), options.recordFolder);
                recorder.reset();
            }
        } // The consumer, recorder and recogniser go before the pipeline frees its channels.
        pipeline.stop();
    } catch (const std::exception& e) {
        std::printf("%s\n", e.what());
        return 1;
    }
    return 0;
}
