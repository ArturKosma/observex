// observex_bench: measures the capture pipeline from a console, without opening the app window.
//
//   observex_bench [--screen N] [--seconds S] [--no-pulse] [--no-preview] [--record FOLDER [--every S] [--unsure]]
//                  [--model FOLDER] [--clip FOLDER]
//   observex_bench --model FOLDER --images FOLDER --out FILE
//   observex_bench --model FOLDER --replay RECORDING
//   observex_bench --model FOLDER --update-kinds RECORDINGS
//   observex_bench --track LOG
//   observex_bench --clip-frames CLIP --out FOLDER [--every N]
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
// like the app), so its cost on capture shows in the numbers. The pulse is near-black, so most saved images show the
// screen as it is. With --model too, it also writes the live log (e.g. character-live.txt, see engine/LiveLog) like the
// app, and marks once halfway, as if Ctrl+Alt+M were pressed. --unsure saves a due image only if the model sees it
// differently mirrored, like Record's "Only unsure" (Pipeline::setRecordOnlyQuestions), and counts the answers.
//
// Clip: --clip keeps the clip buffer running as the app does while observing (engine/ClipRecorder: the last 10 s
// through the GPU's video encoder, with the recogniser's looks when --model is given too), adds its numbers (frames
// encoded per second, milliseconds per frame, seconds and megabytes held), notes halfway, and at the end saves a clip
// into a new folder in FOLDER, as the app's Save button does, and says what it wrote.
//
// Model: --model runs live recognition with the model in FOLDER (e.g. models/character) the way the app does while
// observing, and adds its numbers: images recognised per second, milliseconds per image, things found.
// With --images it looks at the image files of a recording instead of the screen and writes what it found to FILE
// in the marks format (like tools/train/guess.py's <category>-guesses.txt), to compare with the Python model.
//
// Replay: --replay walks a reviewed recording's images in order the way the review does, with its answers as the
// user, and prints how many guesses each rule pre-fills right and wrong and how many it asks about: the model's own
// (core/Marks' presumedKind of a guess) and the learning review's (core/KindLearner re-fit before each image to every
// guess answered so far, then presumedKind), each also with the answers carried forward from the image before as the
// review does (learning: onto the guesses nudged as below), and how long the fits took. Like the review, the re-fit
// starts from the kind update when FOLDER holds
// a valid one, else from model.json's classifier. The guesses and features are the review's finder's (open the
// recording's review once with the model); its answers say which guess is which (core/Marks' matchAnswers: the yes
// answer it overlaps most, at least half, or the user's tighter box around it, the surest guess first, else no). Then
// the box nudge (core/BoxLearner, fitted before each image to the guesses matching a yes answer so far, as the review
// does): of the guesses the user kept as the finder boxed them and of those they boxed otherwise, how many land within
// 0.7 and 0.8 overlap of the user's box, as the finder boxed them and nudged, and how long the fits took. No GPU.
//
// Kind update: --update-kinds makes the guesses and features of the reviewed recordings in RECORDINGS fresh for the
// model in FOLDER where they are not (engine/Refresher: the Finder, one recording at a time, as the app does while a
// review is open), then makes the kind update as closing a review does (engine/KindUpdater), and prints what each did
// and how long it took. It writes into both folders (guesses, features and FOLDER/kinds-update.json): only ever run it
// on copies.
//
// Track: --track follows the things of a live log (a recording's character-live.txt) with the Tracker, look by look as
// the recogniser does, and prints how steady the boxes are with and without it, and what following costs; for a log
// with kinds (since 2026-11-08), also how often a thing's kind changes. No GPU.
//
// Clip frames: --clip-frames decodes a saved clip's video (CLIP is a clip folder, for its clip.mp4, or an .mp4 file)
// frame by frame as the clip player does (engine/ClipPlayer's ClipReader, on the GPU's video decoder), writes
// FOLDER/frames.txt with a "FRAME TIME_MS" line per frame (its place in the video from 1, the numbers the player and
// clip.txt use, and its time since frame 1), and frame-000123.jpg for frame 1, every Nth after it (default 10) and the
// last, so the frames can be looked at. Prints the frames, the length and how long decoding took per frame.

#include "core/BoxLearner.h"
#include "core/Features.h"
#include "core/KindLearner.h"
#include "core/LiveLines.h"
#include "core/Marks.h"
#include "core/Mirror.h"
#include "core/ModelInfo.h"
#include "core/Recordings.h"
#include "core/StatWindow.h"
#include "core/Tracker.h"
#include "engine/ClipPlayer.h"
#include "engine/ClipRecorder.h"
#include "engine/Detector.h"
#include "engine/Gpu.h"
#include "engine/ImageChannel.h"
#include "engine/ImageFile.h"
#include "engine/KindUpdater.h"
#include "engine/LiveLog.h"
#include "engine/Pipeline.h"
#include "engine/Recognizer.h"
#include "engine/Recorder.h"
#include "engine/Refresher.h"
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
#include <ctime>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <string_view>
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
    unsigned recordEvery = 3;  // Seconds between saved images, as the app records.
    bool recordUnsure = false; // Only when the model is unsure, like Record's "Only unsure".
    const char* model = nullptr;
    const char* images = nullptr;
    const char* out = nullptr;
    const char* track = nullptr;
    const char* replay = nullptr;
    const char* updateKinds = nullptr;
    const char* clipFolder = nullptr;
    const char* clipFrames = nullptr;
    unsigned frameEvery = 10; // --every for --clip-frames: a JPEG of every Nth frame.
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
            options.frameEvery = options.recordEvery;
        } else if (std::strcmp(argv[i], "--unsure") == 0) {
            options.recordUnsure = true;
        } else if (std::strcmp(argv[i], "--model") == 0 && i + 1 < argc) {
            options.model = argv[++i];
        } else if (std::strcmp(argv[i], "--images") == 0 && i + 1 < argc) {
            options.images = argv[++i];
        } else if (std::strcmp(argv[i], "--out") == 0 && i + 1 < argc) {
            options.out = argv[++i];
        } else if (std::strcmp(argv[i], "--track") == 0 && i + 1 < argc) {
            options.track = argv[++i];
        } else if (std::strcmp(argv[i], "--replay") == 0 && i + 1 < argc) {
            options.replay = argv[++i];
        } else if (std::strcmp(argv[i], "--update-kinds") == 0 && i + 1 < argc) {
            options.updateKinds = argv[++i];
        } else if (std::strcmp(argv[i], "--clip") == 0 && i + 1 < argc) {
            options.clipFolder = argv[++i];
        } else if (std::strcmp(argv[i], "--clip-frames") == 0 && i + 1 < argc) {
            options.clipFrames = argv[++i];
        } else {
            std::printf("usage: observex_bench [--screen N] [--seconds S] [--no-pulse] [--no-preview] "
                        "[--record FOLDER [--every S] [--unsure]] [--model FOLDER] [--clip FOLDER]\n"
                        "       observex_bench --model FOLDER --images FOLDER --out FILE\n"
                        "       observex_bench --model FOLDER --replay RECORDING\n"
                        "       observex_bench --model FOLDER --update-kinds RECORDINGS\n"
                        "       observex_bench --track LOG\n"
                        "       observex_bench --clip-frames CLIP --out FOLDER [--every N]\n");
            return false;
        }
    }
    if (options.clipFrames != nullptr && options.out == nullptr) {
        std::printf("--clip-frames needs --out\n");
        return false;
    }
    if (options.images != nullptr && (options.model == nullptr || options.out == nullptr)) {
        std::printf("--images needs --model and --out\n");
        return false;
    }
    if (options.replay != nullptr && options.model == nullptr) {
        std::printf("--replay needs --model\n");
        return false;
    }
    if (options.updateKinds != nullptr && options.model == nullptr) {
        std::printf("--update-kinds needs --model\n");
        return false;
    }
    if (options.recordUnsure && (options.recordFolder == nullptr || options.model == nullptr)) {
        std::printf("--unsure needs --record and --model\n");
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

// --images: runs the Detector on each image file of a recording and writes what it found as marks. Also looks at
// each mirrored, as recording only unsure does, and writes how differently it saw it (core/Mirror) to
// FILE.mirror.txt, one "IMAGE DISAGREEMENT" line each, to compare with Python; with kinds, each line also says how
// close any thing's two surest kinds were (core/Detection's closestKinds over both looks).
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
    std::vector<Detection> mirrored(Found::kMax);
    std::ofstream mirrorOut(std::string(options.out) + ".mirror.txt", std::ios::binary);
    std::vector<float> disagreements;
    long long kindsUnsure = 0; // Images where a look was unsure which kind something is.
    long long unsure = 0;      // Images recording only unsure would save, for either reason.
    for (const fs::path& file : files) {
        const ImageData image = readImageFile(file);
        if (image.width != first.width || image.height != first.height) {
            std::printf("Skipped %s: another size\n", file.filename().string().c_str());
            continue;
        }
        gpu.context->UpdateSubresource(detector.input(), 0, nullptr, image.bgra.data(), image.width * 4, 0);
        const auto start = std::chrono::steady_clock::now();
        const int count = detector.detect(gpu.context.Get(), LiveLog::kMinSure, found.data(), Found::kMax);
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        const int mirroredCount =
            detector.detect(gpu.context.Get(), LiveLog::kMinSure, mirrored.data(), Found::kMax, true);
        if (count < 0 || mirroredCount < 0) {
            std::printf("The GPU failed on %s\n", file.filename().string().c_str());
            return 1;
        }
        if (file != files.front()) {
            totalMs += ms; // The first run includes TensorRT's warm-up.
        }
        const std::span<const Detection> plain{found.data(), static_cast<std::size_t>(count)};
        const std::span<const Detection> turned{mirrored.data(), static_cast<std::size_t>(mirroredCount)};
        disagreements.push_back(mirrorDisagreement(plain, turned, Recognizer::kMinSure));
        const float closest = closestKinds(plain, turned, Recognizer::kMinSure);
        const bool closeKinds = unsureOfKind(closest);
        kindsUnsure += closeKinds ? 1 : 0;
        unsure += closeKinds || disagreements.back() >= Recognizer::kMirrorDisagrees ? 1 : 0;
        mirrorOut << file.filename().string() << ' ' << disagreements.back();
        if (closest >= 0.0f) {
            mirrorOut << ' ' << closest;
        }
        mirrorOut << '\n';
        std::vector<Mark>& imageMarks = marks[file.filename().string()];
        for (int i = 0; i < count && found[static_cast<std::size_t>(i)].sure >= Recognizer::kMinSure; ++i) {
            const Detection& d = found[static_cast<std::size_t>(i)];
            Mark mark;
            mark.box = {static_cast<int>(std::lround(d.x)), static_cast<int>(std::lround(d.y)),
                        std::max(1, static_cast<int>(std::lround(d.width))),
                        std::max(1, static_cast<int>(std::lround(d.height)))};
            mark.guess = d.sure;
            mark.kind = d.kind;
            mark.secondKind = d.secondKind;
            mark.secondGuess = d.second;
            mark.kindGuess = d.kindSure;
            imageMarks.push_back(mark);
        }
    }
    const std::vector<std::string_view> kinds(detector.kinds().begin(), detector.kinds().end());
    std::ofstream(options.out, std::ios::binary) << formatMarks(marks, "observex_bench --images", kinds);
    std::printf("Looked at %zu images: %.2f ms per image (squeeze, model and boxes; after the first). Wrote %s\n",
                files.size(), files.size() > 1 ? totalMs / static_cast<double>(files.size() - 1) : 0.0, options.out);
    std::sort(disagreements.begin(), disagreements.end());
    const auto saved = std::count_if(disagreements.begin(), disagreements.end(),
                                     [](float d) { return d >= Recognizer::kMirrorDisagrees; });
    std::printf("Mirror test: median %.3f; %lld of %zu at least %.2f (recording only unsure would save them)\n",
                static_cast<double>(disagreements[disagreements.size() / 2]), static_cast<long long>(saved),
                disagreements.size(), static_cast<double>(Recognizer::kMirrorDisagrees));
    if (!detector.kinds().empty()) {
        std::printf("Kinds: unsure which kind something is (two kinds under %.2f apart) on %lld; only unsure would "
                    "save %lld of %zu\n",
                    static_cast<double>(kKindsClose), kindsUnsure, unsure, disagreements.size());
    }
    return 0;
}

// --track: follows the things of a live log with the Tracker and prints how steady the boxes are (and the kinds).
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
    std::vector<Detection> looked; // All the last look line logged, with its kinds line's kinds.
    std::vector<Detection> found;  // What of it the recogniser hands the tracker.
    std::vector<Detection> before; // `found` of the look before.
    std::array<Tracked, Found::kMax> followed;
    std::map<std::uint32_t, Life> lives;               // Per identity: when it was first and last shown,
    std::map<std::uint32_t, std::uint8_t> kindShown;   // and the kind it was shown as last.
    StatWindow cost;
    int looks = 0;
    bool kinds = false; // The log has kinds lines.
    double firstMs = 0.0;
    double lastMs = 0.0;
    long long previousFound = 0;
    long long previousFollowed = 0;
    long long foundChanges = 0; // Boxes appearing or vanishing between looks, as the model alone shows them.
    long long followedChanges = 0;
    long long foundKindChanges = 0;    // A thing found on two looks in a row (boxes overlapping half) as another kind.
    long long followedKindChanges = 0; // A followed thing shown as another kind than on the look before.
    unsigned long long image = 0;
    double timeMs = 0.0;
    const auto boxOf = [](const Detection& thing) {
        return Box{static_cast<int>(thing.x), static_cast<int>(thing.y), static_cast<int>(thing.width),
                   static_cast<int>(thing.height)};
    };
    const auto follow = [&] {
        found.clear();
        for (const Detection& thing : looked) {
            if (thing.sure >= Recognizer::kMinSure) {
                found.push_back(thing);
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
            const Tracked& thing = followed[static_cast<std::size_t>(i)];
            const auto [life, added] = lives.try_emplace(thing.id, Life{timeMs, timeMs});
            life->second.lastMs = timeMs;
            const auto [shown, first] = kindShown.try_emplace(thing.id, thing.kind);
            if (!first && shown->second != thing.kind) {
                ++followedKindChanges;
                shown->second = thing.kind;
            }
        }
        for (const Detection& thing : found) {
            const Detection* same = nullptr;
            float most = 0.5f;
            for (const Detection& old : before) {
                const float shared = overlap(boxOf(thing), boxOf(old));
                if (shared >= most) {
                    most = shared;
                    same = &old;
                }
            }
            foundKindChanges += same != nullptr && same->kind != thing.kind ? 1 : 0;
        }
        before = found;
        lastMs = timeMs;
        ++looks;
    };
    std::string line;
    bool pending = false; // A look read and not followed yet: its kinds line may come next.
    while (std::getline(in, line)) {
        if (line.rfind("look ", 0) == 0) {
            if (pending) {
                follow();
            }
            pending = readLook(line, image, timeMs, looked);
        } else if (pending && readKinds(line, image, looked)) {
            kinds = true;
        }
    }
    if (pending) {
        follow();
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
    if (kinds) {
        std::printf("Kinds changing on a thing: %.2f a second from the model alone, %.2f followed\n",
                    static_cast<double>(foundKindChanges) / seconds, static_cast<double>(followedKindChanges) / seconds);
    }
    std::printf("Following: %.2f us per look on average, %.2f us at most\n", static_cast<double>(summary.average),
                static_cast<double>(summary.max));
    return 0;
}

std::string readAll(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

// The kinds the answers name, for a model of `modelKinds`: the review's (core/Marks' kCharacterKindIds) when the model
// knows their first ones, e.g. a model trained before "can't tell" and "dead" came; else the model's. Read with the
// model's kinds alone, an answer of a later kind would be left out, and its guess taken for no.
std::vector<std::string> answerKindsOf(std::vector<std::string> modelKinds) {
    if (kindsFit(modelKinds, kCharacterKindIds)) {
        return {std::begin(kCharacterKindIds), std::end(kCharacterKindIds)};
    }
    return modelKinds;
}

// --replay: walks a reviewed recording as the review does, its answers as the user (see the top).
int replayReview(const Options& options) {
    namespace fs = std::filesystem;
    const fs::path model(options.model);
    const fs::path recording(options.replay);
    const std::string category = model.filename().string(); // E.g. "character", as the review names its files.
    const std::string info = readAll(model / L"model.json");
    const std::vector<std::string> kindNames = parseModelKinds(info);
    const std::vector<std::string_view> kinds(kindNames.begin(), kindNames.end());
    KindHead head = kindHeadToUse(model); // The kind update when there is a valid one, as the review starts from.
    if (kinds.empty() || head.kinds() != static_cast<int>(kinds.size())) {
        std::printf("%s/model.json has no kinds or no kind head (export the model again)\n", options.model);
        return 1;
    }
    const std::vector<std::string> answerKindNames = answerKindsOf(kindNames);
    const std::vector<std::string_view> answerKinds(answerKindNames.begin(), answerKindNames.end());
    const ImageMarks guesses = parseMarks(readAll(recording / (category + "-guesses.txt")), answerKinds);
    const GuessFeatures features = parseFeatures(readAll(recording / (category + "-features.bin")));
    const ImageMarks answers = parseMarks(readAll(recording / (category + "-answers.txt")), answerKinds);
    if (!featuresMatch(features, guesses) || features.size != head.features()) {
        std::printf("No features of these guesses in %s (open its review once with this model)\n", options.replay);
        return 1;
    }

    struct Count {
        int right = 0;
        int wrong = 0;
        int asked = 0;
        // A pre-filled answer: `kind` for a yes, or KindLearner::kNo; -2 for none (asked).
        void add(int filled, int truth) { ++(filled == -2 ? asked : filled == truth ? right : wrong); }
    };
    Count own;            // The model's own rule, as the review was before it learned.
    Count learning;       // The learning review's rule.
    Count ownCarried;     // Each with the answers of the image before carried forward first, as the review does.
    Count learningCarried;
    KindLearner learner(std::move(head));
    std::vector<float> sampleFeatures; // Every guess on the images so far, with its answer.
    std::vector<int> sampleAnswers;
    const std::vector<Mark>* before = nullptr; // The answers of the image before, if it is reviewed.
    double fitMs = 0.0;
    double longestFitMs = 0.0;
    int fits = 0;
    std::size_t lastFit = 0; // Answers the last fit learned from.
    float ownApart = 0.0f; // The model's own kind sureness from the features against the guesses file's.
    int images = 0;
    int changed = 0; // Guesses the learning rule pre-answered otherwise than the model alone.
    std::size_t guessCount = 0;
    std::vector<float> ownSure(kinds.size());
    std::vector<float> learnedSure(kinds.size());

    // The box nudge: per guess matching a yes answer, how close to the user's box it lands as boxed and nudged.
    struct Close {
        int count = 0;
        int within7[2] = {}; // As the finder boxed it, nudged.
        int within8[2] = {};
        double overlap[2] = {};
        void add(const Box& truth, const Box& guess, const Box& nudged) {
            ++count;
            const float amounts[2] = {observex::overlap(truth, guess), observex::overlap(truth, nudged)};
            for (int i = 0; i < 2; ++i) {
                within7[i] += amounts[i] >= 0.7f ? 1 : 0;
                within8[i] += amounts[i] >= 0.8f ? 1 : 0;
                overlap[i] += amounts[i];
            }
        }
    };
    Close kept;    // The user kept the finder's box.
    Close redrawn; // The user boxed it otherwise.
    std::vector<std::string> imageNames; // The recording's images with guesses, in name order, as the review's.
    for (const auto& image : guesses) {
        imageNames.push_back(image.first);
    }
    BoxLearner boxLearner;
    BoxSamples boxSamples;
    boxSamples.size = features.size;
    double boxFitMs = 0.0;
    double longestBoxFitMs = 0.0;
    int boxFits = 0;
    int place = -1;
    for (const auto& [name, marks] : guesses) {
        ++place;
        const auto answered = answers.find(name);
        if (answered == answers.end()) {
            before = nullptr;
            continue;
        }
        ++images;
        guessCount += marks.size();

        // Which guess is which, as the Python simulation matched them: surest guess first, the yes answer it
        // overlaps most if at least half (or the user's tighter box around it), else no (KindLearner::kNo, -1).
        const std::vector<int> truth = matchGuesses(marks, answered->second);

        // The box nudge, fitted for this image to the guesses answered so far, as the review does.
        {
            const auto boxStart = std::chrono::steady_clock::now();
            boxLearner.fit(boxSamples, place);
            const double boxMs =
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - boxStart).count();
            if (boxSamples.count() > 0) {
                boxFitMs += boxMs;
                longestBoxFitMs = std::max(longestBoxFitMs, boxMs);
                ++boxFits;
            }
            const std::vector<int> matched = matchAnswers(marks, answered->second);
            for (std::size_t i = 0; i < marks.size(); ++i) {
                if (matched[i] < 0) {
                    continue;
                }
                const Box& truthBox = answered->second[static_cast<std::size_t>(matched[i])].box;
                const Box nudgedBox = nudged(marks[i].box, boxLearner.nudge(features.of(name, i)));
                (truthBox == marks[i].box ? kept : redrawn).add(truthBox, marks[i].box, nudgedBox);
            }
            ImageMarks one;
            one.emplace(name, answered->second);
            addAnsweredBoxes(guesses, features, one, imageNames, boxSamples);
        }

        // Re-fit to everything answered so far, as entering the image does.
        const auto start = std::chrono::steady_clock::now();
        learner.fit(sampleFeatures, sampleAnswers);
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        if (!sampleAnswers.empty()) {
            fitMs += ms;
            longestFitMs = std::max(longestFitMs, ms);
            ++fits;
            lastFit = sampleAnswers.size();
        }

        std::vector<Mark> ownMarks = marks;     // As the review shows them without learning,
        std::vector<Mark> learnedMarks = marks; // and with, their boxes nudged (before carrying, as the review).
        for (std::size_t i = 0; i < learnedMarks.size(); ++i) {
            learnedMarks[i].box = nudged(marks[i].box, boxLearner.nudge(features.of(name, i)));
        }
        std::vector<int> presumed;
        for (std::size_t i = 0; i < marks.size(); ++i) {
            learner.sure(features.of(name, i), ownSure, learnedSure);
            ownApart = std::max(ownApart, std::abs(ownSure[marks[i].kind] - kindGuessOf(marks[i])));
            showKinds(learnedMarks[i], learnedSure);
            presumed.push_back(presumedKind(ownSure, learnedSure));
        }
        const auto filled = [](const Mark& mark) {
            return mark.answer == Answer::Yes ? mark.kind : mark.answer == Answer::No ? KindLearner::kNo : -2;
        };
        std::vector<int> ownPresumed;
        for (const Mark& mark : marks) {
            ownPresumed.push_back(presumedKind(mark));
        }
        std::vector<Mark> plain = ownMarks;
        presumeKinds(plain, ownPresumed);
        for (std::size_t i = 0; i < marks.size(); ++i) {
            own.add(filled(plain[i]), truth[i]);
            learning.add(presumed[i] >= 0 ? presumed[i] : -2, truth[i]);
            changed += filled(plain[i]) != (presumed[i] >= 0 ? presumed[i] : -2) ? 1 : 0;
        }
        if (before != nullptr) {
            carryAnswers(*before, ownMarks, ownPresumed);
            carryAnswers(*before, learnedMarks, presumed);
        }
        presumeKinds(ownMarks, ownPresumed);
        presumeKinds(learnedMarks, presumed);
        for (std::size_t i = 0; i < marks.size(); ++i) {
            ownCarried.add(filled(ownMarks[i]), truth[i]);
            learningCarried.add(filled(learnedMarks[i]), truth[i]);
        }

        // The user answered them all: the next fit learns from these too, but for a kind the model does not know yet
        // (as core/KindLearner's addAnsweredGuesses).
        for (std::size_t i = 0; i < marks.size(); ++i) {
            if (truth[i] >= learner.kinds()) {
                continue;
            }
            const std::span<const float> feature = features.of(name, i);
            sampleFeatures.insert(sampleFeatures.end(), feature.begin(), feature.end());
            sampleAnswers.push_back(truth[i]);
        }
        before = &answered->second;
    }

    std::printf("Replayed %d reviewed images of %s: %zu guesses\n", images, options.replay, guessCount);
    std::printf("                                      pre-filled right  wrong  asked\n");
    const auto line = [](const char* rule, const Count& count) {
        std::printf("  %-36s %16d %6d %6d\n", rule, count.right, count.wrong, count.asked);
    };
    line("the model alone (before 2026-11-08)", own);
    line("learning (the model and the re-fit)", learning);
    std::printf("  with the answers of the image before carried forward first, as the review does:\n");
    line("the model alone", ownCarried);
    line("learning", learningCarried);
    std::printf("Learning pre-answered %d guesses otherwise than the model alone\n", changed);
    std::printf("Fits: %d, %.1f ms on average, %.1f ms at most (the last with %zu answers)\n", fits,
                fits > 0 ? fitMs / fits : 0.0, longestFitMs, lastFit);
    std::printf("The model's own kind sureness from the features is within %.4f of the guesses file's\n",
                static_cast<double>(ownApart));
    std::printf("Box nudge: guesses matching a yes answer, how many land within 0.7 / 0.8 overlap of the user's box\n");
    const auto closeLine = [](const char* which, const Close& close) {
        const double count = std::max(close.count, 1);
        std::printf("  %-28s %4d: as boxed %3.0f%% / %3.0f%% (overlap %.2f), nudged %3.0f%% / %3.0f%% (overlap %.2f)\n",
                    which, close.count, 100.0 * close.within7[0] / count, 100.0 * close.within8[0] / count,
                    close.overlap[0] / count, 100.0 * close.within7[1] / count, 100.0 * close.within8[1] / count,
                    close.overlap[1] / count);
    };
    closeLine("boxed otherwise by the user", redrawn);
    closeLine("kept as the finder boxed", kept);
    std::printf("Box fits: %d, %.1f ms on average, %.1f ms at most (the last with %zu guesses)\n", boxFits,
                boxFits > 0 ? boxFitMs / boxFits : 0.0, longestBoxFitMs, boxSamples.count());
    return 0;
}

// What --update-kinds learns: the only category so far (app/Review.h's kCategories), whatever FOLDER is called.
constexpr char kKindsCategory[] = "character";

// --update-kinds: makes the reviewed recordings' guesses fresh, then the kind update (see the top).
int updateKinds(const Options& options) {
    namespace fs = std::filesystem;
    using Clock = std::chrono::steady_clock;
    const fs::path model(options.model);
    const fs::path recordings(options.updateKinds);
    const std::vector<std::string> modelKinds = parseModelKinds(readAll(model / L"model.json"));
    if (modelKinds.empty()) {
        std::printf("%s/model.json names no kinds\n", options.model);
        return 1;
    }
    const std::vector<std::string> kinds = answerKindsOf(modelKinds);
    const auto seconds = [](Clock::time_point since) {
        return std::chrono::duration<double>(Clock::now() - since).count();
    };

    // The reviewed recordings whose guesses are not from this model, and their images.
    const std::vector<std::string_view> kindIds(kinds.begin(), kinds.end());
    const std::vector<fs::path> stale = KindUpdater::stale(recordings, kKindsCategory, kindIds, model);
    std::size_t images = 0;
    for (const fs::path& folder : stale) {
        images += listImages(folder).size();
    }
    std::printf("%zu reviewed recordings need fresh guesses (%zu images)\n", stale.size(), images);
    const Clock::time_point refreshStart = Clock::now();
    Refresher refresher(recordings, kKindsCategory, kinds, model);
    int shown = 0;
    while (!refresher.done()) {
        if (refresher.refreshed() != shown) {
            shown = refresher.refreshed();
            std::printf("  %d of %d fresh (%.1f s)\n", shown, refresher.stale(), seconds(refreshStart));
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    const double refreshSeconds = seconds(refreshStart);
    std::printf("Made %d of %d recordings' guesses fresh in %.1f s (%.1f ms per image)%s%s\n", refresher.refreshed(),
                refresher.stale(), refreshSeconds,
                images > 0 ? refreshSeconds * 1000.0 / static_cast<double>(images) : 0.0,
                refresher.error().empty() ? "" : ": ", refresher.error().c_str());

    const Clock::time_point updateStart = Clock::now();
    KindUpdater updater(recordings, kKindsCategory, kinds, model);
    while (updater.state() == KindUpdater::State::Running) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    switch (updater.state()) {
    case KindUpdater::State::Done:
        std::printf("Kind update written to %s: %zu answers from %zu recordings, fit %.0f ms (%.2f s in all)\n",
                    (model / kKindUpdateFile).string().c_str(), updater.answers(), updater.recordings(),
                    updater.fitMs(), seconds(updateStart));
        return 0;
    case KindUpdater::State::NothingNew:
        std::printf("Nothing new: no answers newer than the model, or than the kind update there (%.2f s)\n",
                    seconds(updateStart));
        return 0;
    case KindUpdater::State::NeedsGuesses:
        std::printf("Not made: %zu reviewed recordings' guesses are not from this model:\n",
                    updater.staleRecordings().size());
        for (const fs::path& folder : updater.staleRecordings()) {
            std::printf("  %s\n", folder.string().c_str());
        }
        return 1;
    default:
        std::printf("Not made: %s\n", updater.error().c_str());
        return 1;
    }
}

// The local time now, as std::strftime writes it with `format`.
std::string localTime(const char* format) {
    const std::time_t now = std::time(nullptr);
    std::tm local{};
    localtime_s(&local, &now);
    char text[64];
    return std::string(text, std::strftime(text, sizeof(text), format, &local));
}

// Saves what `clip` holds into a new folder in `clipsFolder`, as the app's Save button does, waits for it and says what
// it wrote.
void saveClip(ClipRecorder& clip, const char* clipsFolder, const Screen& screen, const Recognizer* recognizer,
              const char* model) {
    if (clip.state() == ClipRecorder::State::Failed) {
        std::printf("Clip buffer failed: %s\n", clip.error().c_str());
        return;
    }
    ClipAbout about;
    about.saved = localTime("%Y-%m-%d %H:%M:%S");
    about.screen = screen.monitor;
    if (recognizer != nullptr) {
        about.model = toUtf8((std::filesystem::path(model) / L"model.onnx").c_str());
        about.kinds = recognizer->kinds();
    }
    const std::filesystem::path folder = std::filesystem::path(clipsFolder) / localTime("%Y-%m-%d_%H-%M-%S");
    const auto start = std::chrono::steady_clock::now();
    if (!clip.save(folder, about)) {
        std::printf("The clip buffer holds nothing to save yet.\n");
        return;
    }
    while (clip.saveState() == ClipRecorder::SaveState::Saving) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    if (clip.saveState() == ClipRecorder::SaveState::Failed) {
        std::printf("Saving the clip failed: %s\n", clip.saveError().c_str());
        return;
    }
    const ClipInfo& info = clip.savedInfo();
    const auto megabytes = [&](const char* name) {
        std::error_code error;
        const auto size = std::filesystem::file_size(folder / name, error);
        return error ? 0.0 : static_cast<double>(size) / (1024.0 * 1024.0);
    };
    std::printf("Saved a clip of %.2f s (%u frames, %u looks) in %.0f ms into %s\n"
                "  %s %.1f MB, %s %.2f MB, %s %.2f MB\n",
                info.seconds, info.frames, info.looks, ms, toUtf8(folder.c_str()).c_str(), kClipVideo,
                megabytes(kClipVideo), kClipText, megabytes(kClipText), kClipPicture, megabytes(kClipPicture));
}

// --clip-frames: decodes a clip's video as the clip player does, writes frames.txt and every Nth frame as a JPEG.
int clipFrames(const Options& options) {
    namespace fs = std::filesystem;
    using Clock = std::chrono::steady_clock;
    const auto msSince = [](Clock::time_point start) {
        return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
    };
    fs::path video(options.clipFrames);
    if (fs::is_directory(video)) {
        video /= kClipVideo;
    }
    const ComPtr<IDXGIAdapter1> adapter = findPrimaryGpu();
    const GpuDevice gpu = createGpuDevice(adapter.Get(), true); // Video support, for the GPU's video decoder.
    const Clock::time_point openStart = Clock::now();
    ClipReader reader(gpu.device.Get(), video);
    const double openMs = msSince(openStart);
    const fs::path out(options.out);
    fs::create_directories(out);
    const std::vector<double>& times = reader.times();
    std::printf("%s: %u x %u, %zu frames, %.3f s, decoded on the %s (opened in %.0f ms)\n",
                toUtf8(video.c_str()).c_str(), reader.width(), reader.height(), times.size(), times.back(),
                reader.onGpu() ? "GPU" : "CPU", openMs);

    // First every frame, decoding alone, timed until the GPU is done with it.
    std::ofstream list(out / "frames.txt", std::ios::binary);
    ClipReader::Frame frame;
    std::uint64_t frames = 0;
    const Clock::time_point decodeStart = Clock::now();
    while (reader.next(frame)) {
        ++frames;
        char line[64];
        std::snprintf(line, sizeof(line), "%llu %.3f\n", static_cast<unsigned long long>(frame.number),
                      frame.seconds * 1000.0);
        list << line;
    }
    D3D11_QUERY_DESC queryDesc{D3D11_QUERY_EVENT, 0};
    ComPtr<ID3D11Query> done;
    check(gpu.device->CreateQuery(&queryDesc, &done), "CreateQuery");
    gpu.context->End(done.Get());
    while (gpu.context->GetData(done.Get(), nullptr, 0, 0) == S_FALSE) {
        std::this_thread::yield();
    }
    const double decodeMs = msSince(decodeStart);
    list.close();
    std::printf("Decoded %llu frames in %.0f ms: %.2f ms per frame\n", static_cast<unsigned long long>(frames),
                decodeMs, frames > 0 ? decodeMs / static_cast<double>(frames) : 0.0);

    // Then again from the start, reading frame 1, every Nth after it and the last back for their JPEGs.
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = reader.width();
    desc.Height = reader.height();
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = ScreenCapture::kImageFormat;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_STAGING;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> staging;
    check(gpu.device->CreateTexture2D(&desc, nullptr, &staging), "CreateTexture2D (clip frame)");
    const unsigned every = std::max(options.frameEvery, 1u);
    const Clock::time_point writeStart = Clock::now();
    unsigned written = 0;
    reader.rewind();
    while (reader.next(frame)) {
        if ((frame.number - 1) % every != 0 && frame.number != times.size()) {
            continue;
        }
        gpu.context->CopySubresourceRegion(staging.Get(), 0, 0, 0, 0, frame.texture, frame.subresource, nullptr);
        D3D11_MAPPED_SUBRESOURCE mapped{};
        check(gpu.context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped), "Map (clip frame)");
        char name[32];
        std::snprintf(name, sizeof(name), "frame-%06llu.jpg", static_cast<unsigned long long>(frame.number));
        try {
            writeImageFile(out / name, ImageFormat::Jpeg, mapped.pData, reader.width(), reader.height(),
                           mapped.RowPitch);
        } catch (...) {
            gpu.context->Unmap(staging.Get(), 0);
            throw;
        }
        gpu.context->Unmap(staging.Get(), 0);
        ++written;
    }
    std::printf("Wrote frames.txt and %u JPEGs (frame 1, every %u after it, the last) into %s in %.1f s\n", written,
                every, toUtf8(out.c_str()).c_str(), msSince(writeStart) / 1000.0);
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
    if (options.replay != nullptr) {
        return replayReview(options);
    }
    if (options.updateKinds != nullptr) {
        try {
            return updateKinds(options);
        } catch (const std::exception& e) {
            std::printf("%s\n", e.what());
            return 1;
        }
    }
    if (options.clipFrames != nullptr) {
        const bool com = SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED)); // Media Foundation needs it.
        int result = 1;
        try {
            result = clipFrames(options);
        } catch (const std::exception& e) {
            std::printf("%s\n", e.what());
        }
        if (com) {
            CoUninitialize();
        }
        return result;
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
                recognizer = std::make_unique<Recognizer>(screen, *pipeline.recognition(), options.model,
                                                          &pipeline.question());
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
                pipeline.setRecordOnlyQuestions(options.recordUnsure);
                pipeline.setRecordingInterval(options.recordEvery);
            }
            // The clip buffer, as the app keeps it while observing.
            std::shared_ptr<ClipRecorder> clip;
            if (options.clipFolder != nullptr) {
                clip = std::make_shared<ClipRecorder>(screen, *pipeline.clip());
                pipeline.setClipEnabled(true);
                if (recognizer) {
                    recognizer->setClip(clip);
                }
            }
            bool clipNoted = false;
            bool clipFailed = false;
            bool marked = false;
            std::jthread pulse;
            if (options.pulse) {
                pulse = std::jthread([&](std::stop_token stop) { runPulse(gpu.Get(), screen.bounds, stop); });
            }

            std::printf("\nObserving %s (%s) for %d s (pulse %s, preview %s, recording %s, recognition %s)\n",
                        screen.monitor.c_str(), screen.name.c_str(), options.seconds, options.pulse ? "on" : "off",
                        options.preview ? "on" : "off",
                        !recorder              ? "off"
                        : options.recordUnsure ? "only unsure"
                                               : "on",
                        recognizer ? "on" : "off");
            std::printf("  images/s  merged/s  latency avg / max ms  work avg / max ms%s%s\n",
                        recognizer ? "  recognised/s  recognise avg / max ms  found" : "",
                        clip ? "  encoded/s  encode avg / max ms  held s    MB  dropped" : "");

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
                if (clip && !clipNoted && Clock::now() - start >= std::chrono::milliseconds(options.seconds * 500)) {
                    clip->note("halfway (observex_bench)");
                    clipNoted = true;
                }
                if (clip && !clipFailed && clip->state() == ClipRecorder::State::Failed) {
                    std::printf("Clip buffer failed: %s\n", clip->error().c_str());
                    clipFailed = true;
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
                    if (clip) {
                        const ClipRecorderStats& held = clip->stats();
                        std::printf("  %9.1f  %8.2f / %-8.2f  %6.1f  %4.0f  %7llu",
                                    static_cast<double>(held.framesPerSecond), static_cast<double>(held.encodeAvgMs),
                                    static_cast<double>(held.encodeMaxMs), held.seconds,
                                    static_cast<double>(held.bytes) / (1024.0 * 1024.0),
                                    static_cast<unsigned long long>(held.dropped));
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
            if (clip) {
                saveClip(*clip, options.clipFolder, screen, recognizer.get(), options.model);
                if (recognizer) {
                    recognizer->setClip(nullptr);
                }
            }
            if (recognizer) {
                pipeline.setRecognitionEnabled(false);
                recognizer.reset();
            }
            if (clip) {
                pipeline.setClipEnabled(false);
                clip.reset();
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
