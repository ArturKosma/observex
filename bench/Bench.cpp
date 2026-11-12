// observex_bench: measures the capture pipeline from a console, without opening the app window.
//
//   observex_bench [--screen N] [--seconds S] [--no-pulse] [--no-preview] [--record FOLDER [--every S] [--unsure]]
//                  [--model FOLDER [--snap]] [--clip FOLDER] [--find-ui [--layout FOLDER]]
//   observex_bench --model FOLDER --images FOLDER --out FILE
//   observex_bench --model FOLDER --replay RECORDING
//   observex_bench --model FOLDER --update-kinds RECORDINGS
//   observex_bench --track LOG
//   observex_bench --clip-frames CLIP --out FOLDER [--every N]
//   observex_bench --clip-seek CLIP
//   observex_bench --clip-guesses CLIP [--heuristics DLL] [--snap [--out FOLDER --at FRAME...]] [--today-out FILE]
//   observex_bench --ground CLIP [--heuristics DLL] [--layout FOLDER] [--frames] [--out FOLDER [--every N]
//                  [--at FRAME]...]
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
// app, and marks once halfway, as the app's Ctrl+Alt+M did until 2026-11-12. --unsure saves a due image only if the
// model sees it differently mirrored, like Record's "Only unsure" (Pipeline::setRecordOnlyQuestions), and counts
// the answers.
//
// Clip: --clip keeps the clip buffer running as the app does while observing (engine/ClipRecorder: the last 10 s
// through the GPU's video encoder, with the recogniser's looks when --model is given too), adds its numbers (frames
// encoded per second, milliseconds per frame, seconds and megabytes held), notes halfway, and at the end saves a clip
// into a new folder in FOLDER, as the app's Save button does, and says what it wrote.
//
// Model: --model runs live recognition with the model in FOLDER (e.g. models/character) the way the app does while
// observing, and adds its numbers: images recognised per second, milliseconds per image, things found. With --snap
// also the snapper, as the app runs it (engine/BoxSnapper: the boxes put onto every new image), and its numbers:
// images snapped onto per second, milliseconds per image (the grey image on the GPU, waited for, then snapping), how
// long after the screen showed an image its boxes were there, and the share of boxes snapped.
// With --images it looks at the image files of a recording instead of the screen and writes what it found to FILE
// in the marks format (like tools/train/guess.py's <category>-guesses.txt), to compare with the Python model.
//
// UI: --find-ui also runs the UI finder as the app does while observing (engine/UiFinder: ten images a second, the
// grey image on the GPU, its full-size level read back, then core/UiLayout with ui.txt from --layout, default
// models/ui), and adds its numbers: images per second, milliseconds per image and of them finding, pieces found. With
// --model, the recogniser's milliseconds show what it costs the model (2026-11-12: within run-to-run noise, at most
// about 0.1 ms a look).
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
//
// Clip seek: --clip-seek checks the clip player's seek() on a saved clip (CLIP as for --clip-frames): it decodes every
// frame in order first and keeps a checksum of its pixels, then plays the clip with engine/ClipPlayer as the clip
// screen does (frames read by an ImageReader on a GPU device of its own), pauses it and seeks: frame 1, the last, the
// middle, 20 random frames, 30 steps of +1 and 30 of -1 from the middle, the frame shown again (nothing must come), 50
// seeks in a row (the last asked must be the last to come), then play() after a seek, a seek while playing, and seeks
// after it played to the end. For each it checks the frame that comes: its number, its pixels against the checksum and
// position() against times(). Prints how long each kind of seek took until its frame came (average and most, in ms),
// and every wrong frame. Writes nothing.
//
// Clip guesses: --clip-guesses compares what the clip screen's guessers draw over a saved clip (CLIP is a clip folder,
// for its clip.txt, or the clip.txt; core/ClipGuesses): "Recorded", what the app showed live, against "Today", the
// recorded looks through today's heuristics. At each set Recorded shows from the first frame on (half a second after
// the first look in a clip without looks before its first frame), it pairs their boxes by overlap and counts the sets
// drawn the same (boxes to the pixel, the same tags), boxes only one draws, tags that differ and boxes that moved.
// Prints both guessers' numbers too (boxes at once, boxes coming or going a second) and when sets differ. With the
// heuristics unchanged, a clip saved since 2026-11-11 (numbers written exactly, looks from 5 s before its first frame)
// is drawn the same; older ones differ (2-decimal sureness, and a replay from nothing at their first frame). Today
// is the heuristics built into the bench, or with --heuristics those of an observex_heuristics.dll, loaded as the app
// loads it (engine/HeuristicsLibrary). No GPU. With --snap it also tries snapping (heuristics/Snapper, 2026-11-11):
// it decodes the clip's video and puts Today's boxes onto their things on each frame, by how each thing looked on the
// last frame the model found it on, as if every new screen image were looked at that way the moment it came; and
// prints how precise those boxes are, snapped on the frame shown and on the frame before (moved on by their speed),
// and how long the snapping took a frame. Uses the GPU's video decoder. With --out, each frame given with --at is
// written there as frame-FRAME.png, around its boxes: Today's red, snapped green. With --today-out FILE it writes
// Today snapped as the clip screen does into FILE, a shown line per set (the screen image it was snapped onto, 0 for
// one before the first, and from when it is shown) and after each one snapped a finds line of its searches, to read
// with the scripts that read a clip's own lines. With --looks-out FILE it writes Today's heuristics' own set after each
// look (the tracker's, not snapped) into FILE, a shown line per look with the image looked at.

#include "core/BoxLearner.h"
#include "core/ClipGuesses.h"
#include "core/ClipLines.h"
#include "core/Features.h"
#include "core/KindLearner.h"
#include "core/LiveLines.h"
#include "core/Marks.h"
#include "core/Mirror.h"
#include "core/ModelInfo.h"
#include "core/Recordings.h"
#include "core/StatWindow.h"
#include "core/UiLayout.h"
#include "engine/BoxSnapper.h"
#include "engine/ClipGround.h"
#include "engine/ClipPlayer.h"
#include "engine/ClipRecorder.h"
#include "engine/ClipSnapper.h"
#include "engine/Detector.h"
#include "engine/Gpu.h"
#include "engine/GreyMaker.h"
#include "engine/HeuristicsLibrary.h"
#include "engine/ImageChannel.h"
#include "engine/ImageFile.h"
#include "engine/KindUpdater.h"
#include "engine/LiveLog.h"
#include "engine/Pipeline.h"
#include "engine/Recognizer.h"
#include "engine/Recorder.h"
#include "engine/Refresher.h"
#include "engine/ScreenCapture.h"
#include "engine/UiFinder.h"
#include "heuristics/GroundMotion.h"
#include "heuristics/Rules.h"
#include "heuristics/SnapRules.h"
#include "heuristics/Snapper.h"
#include "heuristics/Tracker.h"

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
#include <deque>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <random>
#include <span>
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
    const char* clipSeek = nullptr;
    const char* clipGuesses = nullptr;
    const char* heuristics = nullptr; // --heuristics for --clip-guesses: an observex_heuristics.dll.
    bool snap = false;                // --snap for --clip-guesses: also snap Today's boxes on the video's frames.
    std::vector<std::uint64_t> at;    // --at for --snap: frames to write into --out with both guessers' boxes.
    const char* todayOut = nullptr;   // --today-out for --clip-guesses: Today snapped's sets, as shown lines.
    const char* looksOut = nullptr;   // --looks-out for --clip-guesses: Today's sets after each look (not snapped).
    const char* ui = nullptr;         // --ui CLIP: the game's interface found on each frame, as the UI flag finds it.
    const char* layout = nullptr;     // --layout for --ui and --find-ui: the folder with ui.txt (default models/ui).
    bool findUi = false;              // --find-ui: the live run also runs the UI finder, as the app does.
    const char* ground = nullptr;     // --ground CLIP: the camera followed over a clip, as the Ground flag does.
    bool groundFrames = false;        // --frames for --ground: a line for every frame followed, and every camera.
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
        } else if (std::strcmp(argv[i], "--clip-seek") == 0 && i + 1 < argc) {
            options.clipSeek = argv[++i];
        } else if (std::strcmp(argv[i], "--clip-guesses") == 0 && i + 1 < argc) {
            options.clipGuesses = argv[++i];
        } else if (std::strcmp(argv[i], "--heuristics") == 0 && i + 1 < argc) {
            options.heuristics = argv[++i];
        } else if (std::strcmp(argv[i], "--snap") == 0) {
            options.snap = true;
        } else if (std::strcmp(argv[i], "--find-ui") == 0) {
            options.findUi = true;
        } else if (std::strcmp(argv[i], "--at") == 0 && i + 1 < argc) {
            options.at.push_back(std::strtoull(argv[++i], nullptr, 10));
        } else if (std::strcmp(argv[i], "--today-out") == 0 && i + 1 < argc) {
            options.todayOut = argv[++i];
        } else if (std::strcmp(argv[i], "--looks-out") == 0 && i + 1 < argc) {
            options.looksOut = argv[++i];
        } else if (std::strcmp(argv[i], "--ui") == 0 && i + 1 < argc) {
            options.ui = argv[++i];
        } else if (std::strcmp(argv[i], "--layout") == 0 && i + 1 < argc) {
            options.layout = argv[++i];
        } else if (std::strcmp(argv[i], "--ground") == 0 && i + 1 < argc) {
            options.ground = argv[++i];
        } else if (std::strcmp(argv[i], "--frames") == 0) {
            options.groundFrames = true;
        } else {
            std::printf(
                "usage: observex_bench [--screen N] [--seconds S] [--no-pulse] [--no-preview] "
                "[--record FOLDER [--every S] [--unsure]] [--model FOLDER [--snap]] [--clip FOLDER]\n"
                "                      [--find-ui [--layout FOLDER]]\n"
                "       observex_bench --model FOLDER --images FOLDER --out FILE\n"
                "       observex_bench --model FOLDER --replay RECORDING\n"
                "       observex_bench --model FOLDER --update-kinds RECORDINGS\n"
                "       observex_bench --track LOG\n"
                "       observex_bench --clip-frames CLIP --out FOLDER [--every N]\n"
                "       observex_bench --clip-seek CLIP\n"
                "       observex_bench --clip-guesses CLIP [--heuristics DLL] [--snap [--out FOLDER --at FRAME...]]"
                " [--today-out FILE]\n"
                "       observex_bench --ui CLIP [--layout FOLDER] [--out FOLDER [--every N] [--at FRAME]...]\n"
                "       observex_bench --ground CLIP [--heuristics DLL] [--layout FOLDER] [--frames]"
                " [--out FOLDER [--every N] [--at FRAME]...]\n");
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
    std::map<std::uint32_t, Life> lives;             // Per identity: when it was first and last shown,
    std::map<std::uint32_t, std::uint8_t> kindShown; // and the kind it was shown as last.
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

    // The user's answers, when the log's recording has them: at each image they reviewed (its `saved` line names its
    // screen image), the boxes handed out right after the look at it against the characters they marked (a box and a
    // character overlapping at least kAnsweredOverlap are one; the box taken back by Tracker::kLeadMs, to its image).
    std::map<std::string, std::vector<Mark>> answers;
    if (std::ifstream file(std::filesystem::path(options.track).parent_path() / "character-answers.txt",
                           std::ios::binary);
        file) {
        const std::string text((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        answers = parseMarks(text, kCharacterKindIds);
    }
    std::map<unsigned long long, const std::vector<Mark>*> answeredAt; // By screen image.
    {
        std::string savedLine;
        while (std::getline(in, savedLine)) {
            if (savedLine.rfind("saved ", 0) != 0) {
                continue;
            }
            std::istringstream fields(savedLine.substr(6));
            std::string name;
            unsigned long long savedImage = 0;
            if (fields >> name >> savedImage) {
                if (const auto marks = answers.find(name); marks != answers.end()) {
                    answeredAt[savedImage] = &marks->second;
                }
            }
        }
        in.clear();
        in.seekg(0);
    }
    constexpr float kAnsweredOverlap = 0.3f;
    long long answeredImages = 0;
    long long characters = 0;
    long long charactersShown = 0;
    long long charactersFound = 0; // By the model alone, at least kStartSure sure.
    long long boxesOnNothing = 0;
    std::vector<Box> marked;
    std::vector<Box> boxes;
    // How many of `boxes` lie on one of `marked` (each on one, best overlapping first).
    const auto onMarked = [&marked](const std::vector<Box>& candidates) {
        struct Pair {
            float shared;
            std::size_t candidate;
            std::size_t mark;
        };
        std::vector<Pair> pairs;
        for (std::size_t c = 0; c < candidates.size(); ++c) {
            for (std::size_t m = 0; m < marked.size(); ++m) {
                if (const float shared = overlap(candidates[c], marked[m]); shared >= kAnsweredOverlap) {
                    pairs.push_back({shared, c, m});
                }
            }
        }
        std::sort(pairs.begin(), pairs.end(), [](const Pair& a, const Pair& b) { return a.shared > b.shared; });
        std::vector<char> candidateTaken(candidates.size(), 0);
        std::vector<char> markTaken(marked.size(), 0);
        long long paired = 0;
        for (const Pair& pair : pairs) {
            if (candidateTaken[pair.candidate] == 0 && markTaken[pair.mark] == 0) {
                candidateTaken[pair.candidate] = 1;
                markTaken[pair.mark] = 1;
                ++paired;
            }
        }
        return paired;
    };
    const auto score = [&](const std::vector<Mark>& marks, int count) {
        marked.clear();
        for (const Mark& mark : marks) {
            if (mark.answer == Answer::Yes) {
                marked.push_back(mark.box);
            }
        }
        boxes.clear();
        for (int i = 0; i < count; ++i) {
            Tracked thing = followed[static_cast<std::size_t>(i)];
            thing.x -= thing.speedX * static_cast<float>(Tracker::kLeadMs);
            thing.y -= thing.speedY * static_cast<float>(Tracker::kLeadMs);
            boxes.push_back(boxOf(thing));
        }
        const long long shown = onMarked(boxes);
        boxes.clear();
        for (const Detection& thing : found) {
            if (thing.sure >= Tracker::kStartSure) {
                boxes.push_back(boxOf(thing));
            }
        }
        ++answeredImages;
        characters += static_cast<long long>(marked.size());
        charactersShown += shown;
        charactersFound += onMarked(boxes);
        boxesOnNothing += count - shown;
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
        if (const auto at = answeredAt.find(image); at != answeredAt.end()) {
            score(*at->second, count);
            answeredAt.erase(at);
        }
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
    if (answeredImages > 0) {
        std::printf("At %lld answered images: %lld of %lld characters shown (the model alone, at least %.0f%% sure: "
                    "%lld), %lld boxes on nothing marked\n",
                    answeredImages, charactersShown, characters, static_cast<double>(Tracker::kStartSure * 100.0f),
                    charactersFound, boxesOnNothing);
    }
    if (kinds) {
        std::printf("Kinds changing on a thing: %.2f a second from the model alone, %.2f followed\n",
                    static_cast<double>(foundKindChanges) / seconds,
                    static_cast<double>(followedKindChanges) / seconds);
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
    Count own;        // The model's own rule, as the review was before it learned.
    Count learning;   // The learning review's rule.
    Count ownCarried; // Each with the answers of the image before carried forward first, as the review does.
    Count learningCarried;
    KindLearner learner(std::move(head));
    std::vector<float> sampleFeatures; // Every guess on the images so far, with its answer.
    std::vector<int> sampleAnswers;
    const std::vector<Mark>* before = nullptr; // The answers of the image before, if it is reviewed.
    double fitMs = 0.0;
    double longestFitMs = 0.0;
    int fits = 0;
    std::size_t lastFit = 0; // Answers the last fit learned from.
    float ownApart = 0.0f;   // The model's own kind sureness from the features against the guesses file's.
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
    Close kept;                          // The user kept the finder's box.
    Close redrawn;                       // The user boxed it otherwise.
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

// A checksum of a frame's pixels (FNV-1a over the pixels), read back through a texture the CPU can read, to tell
// which frame a texture holds.
class FrameSum {
public:
    FrameSum(GpuDevice gpu, unsigned width, unsigned height) : gpu_(std::move(gpu)), width_(width), height_(height) {
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = width;
        desc.Height = height;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = ScreenCapture::kImageFormat;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_STAGING;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        check(gpu_.device->CreateTexture2D(&desc, nullptr, &staging_), "CreateTexture2D (frame sum)");
    }

    std::uint64_t operator()(ID3D11Texture2D* texture, UINT subresource = 0) {
        gpu_.context->CopySubresourceRegion(staging_.Get(), 0, 0, 0, 0, texture, subresource, nullptr);
        D3D11_MAPPED_SUBRESOURCE mapped{};
        check(gpu_.context->Map(staging_.Get(), 0, D3D11_MAP_READ, 0, &mapped), "Map (frame sum)");
        std::uint64_t sum = 14695981039346656037ull;
        for (unsigned y = 0; y < height_; ++y) {
            const auto* row =
                static_cast<const std::uint8_t*>(mapped.pData) + static_cast<std::size_t>(mapped.RowPitch) * y;
            for (unsigned x = 0; x < width_; ++x) {
                std::uint32_t pixel = 0;
                std::memcpy(&pixel, row + static_cast<std::size_t>(x) * 4, sizeof(pixel));
                sum = (sum ^ pixel) * 1099511628211ull;
            }
        }
        gpu_.context->Unmap(staging_.Get(), 0);
        return sum;
    }

private:
    GpuDevice gpu_;
    unsigned width_;
    unsigned height_;
    ComPtr<ID3D11Texture2D> staging_;
};

// --clip-seek: checks ClipPlayer::seek() on a clip the way the clip screen uses it, and times it.
int clipSeek(const Options& options) {
    namespace fs = std::filesystem;
    using Clock = std::chrono::steady_clock;
    using Number = unsigned long long; // For printf.
    const auto msSince = [](Clock::time_point start) {
        return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
    };
    fs::path video(options.clipSeek);
    if (fs::is_directory(video)) {
        video /= kClipVideo;
    }
    const ComPtr<IDXGIAdapter1> adapter = findPrimaryGpu();

    // Every frame's checksum, decoded in order on a device of its own.
    std::vector<std::uint64_t> sums;
    {
        const Clock::time_point start = Clock::now();
        const GpuDevice gpu = createGpuDevice(adapter.Get(), true); // Video support, for the GPU's video decoder.
        ClipReader reader(gpu.device.Get(), video);
        FrameSum sum(gpu, reader.width(), reader.height());
        ClipReader::Frame frame;
        while (reader.next(frame)) {
            sums.push_back(sum(frame.texture, frame.subresource));
        }
        std::vector<std::uint64_t> different(sums);
        std::sort(different.begin(), different.end());
        different.erase(std::unique(different.begin(), different.end()), different.end());
        std::printf("%s: %u x %u, %zu frames (%zu different pictures), %.3f s, decoded on the %s; checksums made in "
                    "%.1f s\n",
                    toUtf8(video.c_str()).c_str(), reader.width(), reader.height(), sums.size(), different.size(),
                    reader.times().back(), reader.onGpu() ? "GPU" : "CPU", msSince(start) / 1000.0);
    }

    // The clip screen's set-up: the player on the GPU, its frames read by a device of the UI's own.
    const GpuDevice ui = createGpuDevice(findOwnAdapter(adapter.Get()).Get());
    const Clock::time_point openStart = Clock::now();
    ClipPlayer player(adapter.Get(), video);
    player.pause();
    const double openMs = msSince(openStart);
    ImageReader frames(ui.device.Get(), player.frames()); // After the player, so it is destroyed first.
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = player.width();
    desc.Height = player.height();
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = ScreenCapture::kImageFormat;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    ComPtr<ID3D11Texture2D> texture;
    check(ui.device->CreateTexture2D(&desc, nullptr, &texture), "CreateTexture2D (clip frame)");
    FrameSum sum(ui, player.width(), player.height());
    const std::vector<double>& times = player.times();
    if (times.size() != sums.size()) {
        std::printf("The player has %zu frames, decoding in order gave %zu\n", times.size(), sums.size());
        return 1;
    }
    const auto last = static_cast<std::uint64_t>(times.size());
    const std::uint64_t middle = (last + 1) / 2;
    std::printf("Player opened in %.0f ms and paused\n", openMs);

    unsigned problems = 0;
    std::uint64_t current = 0; // The frame that came last.
    // Takes the frames that come until frame `wanted` does (then returns how long after `asked`) or `timeoutMs`
    // passed (then -1). Counts the other frames that come meanwhile in `others`.
    const auto await = [&](std::uint64_t wanted, Clock::time_point asked, double timeoutMs,
                           unsigned* others) -> double {
        for (;;) {
            ImageStamp stamp;
            if (frames.copyNewImage(ui.context.Get(), texture.Get(), &stamp)) {
                current = stamp.number;
                if (stamp.number == wanted) {
                    return msSince(asked);
                }
                if (others != nullptr) {
                    ++*others;
                }
                continue;
            }
            if (msSince(asked) > timeoutMs) {
                return -1.0;
            }
            frames.waitForImage(1);
        }
    };
    // Every frame that comes within `ms`: its number and when it came.
    struct Came {
        std::uint64_t number;
        double ms;
    };
    const auto collect = [&](double ms) {
        std::vector<Came> came;
        const Clock::time_point start = Clock::now();
        while (msSince(start) < ms) {
            ImageStamp stamp;
            if (frames.copyNewImage(ui.context.Get(), texture.Get(), &stamp)) {
                current = stamp.number;
                came.push_back({stamp.number, msSince(start)});
            } else {
                frames.waitForImage(1);
            }
        }
        return came;
    };
    // The frame in `of` must be frame `number`; while paused, position() its time and the state Paused.
    const auto checkFrame = [&](const char* what, std::uint64_t number, bool paused, ID3D11Texture2D* of) {
        const std::uint64_t got = sum(of);
        if (got != sums[static_cast<std::size_t>(number - 1)]) {
            ++problems;
            const auto same = std::find(sums.begin(), sums.end(), got);
            if (same == sums.end()) {
                std::printf("  WRONG %s: frame %llu came with pixels of no frame\n", what, static_cast<Number>(number));
            } else {
                std::printf("  WRONG %s: frame %llu came with the pixels of frame %llu\n", what,
                            static_cast<Number>(number), static_cast<Number>(same - sums.begin() + 1));
            }
        }
        if (paused && player.position() != times[static_cast<std::size_t>(number - 1)]) {
            ++problems;
            std::printf("  WRONG %s: frame %llu came at position %.6f s, its time is %.6f s\n", what,
                        static_cast<Number>(number), player.position(), times[static_cast<std::size_t>(number - 1)]);
        }
        if (paused && player.state() != ClipPlayer::State::Paused) {
            ++problems;
            std::printf("  WRONG %s: frame %llu came, the player is not Paused\n", what, static_cast<Number>(number));
        }
    };

    struct Kind {
        const char* name;
        std::vector<double> ms;
        unsigned others = 0; // Frames that came before the one asked for.
    };
    // The frame shown already: nothing comes, and the time stays.
    const auto seekAgain = [&] {
        const std::uint64_t shown = current;
        const double position = player.position();
        unsigned came = 0;
        player.seek(shown);
        await(0, Clock::now(), 100.0, &came);
        if (came > 0 || player.position() != position || player.state() != ClipPlayer::State::Paused) {
            ++problems;
            std::printf("  WRONG seeking frame %llu, the one shown: %u frames came, position %.6f -> %.6f s\n",
                        static_cast<Number>(shown), came, position, player.position());
        }
    };
    const auto seekFrame = [&](Kind& kind, std::uint64_t number) {
        if (number == current) {
            seekAgain();
            return;
        }
        const Clock::time_point asked = Clock::now();
        player.seek(number);
        const double ms = await(number, asked, 3000.0, &kind.others);
        if (ms < 0.0) {
            ++problems;
            std::printf("  WRONG %s: frame %llu never came (frame %llu shown)\n", kind.name,
                        static_cast<Number>(number), static_cast<Number>(current));
            return;
        }
        kind.ms.push_back(ms);
        checkFrame(kind.name, number, true, texture.Get());
    };
    const auto print = [](const Kind& kind) {
        double total = 0.0;
        double most = 0.0;
        for (const double ms : kind.ms) {
            total += ms;
            most = std::max(most, ms);
        }
        std::printf("  %-18s %3zu seeks  avg %6.1f ms  max %6.1f ms", kind.name, kind.ms.size(),
                    kind.ms.empty() ? 0.0 : total / static_cast<double>(kind.ms.size()), most);
        if (kind.others > 0) {
            std::printf("  (%u other frames came first)", kind.others);
        }
        std::printf("\n");
    };
    // Plays until it Ended (at most 15 s), taking the frames as they come.
    const auto playToEnd = [&](const char* what) {
        player.play();
        const Clock::time_point start = Clock::now();
        while (player.state() != ClipPlayer::State::Ended && msSince(start) < 15000.0) {
            collect(5.0);
        }
        collect(20.0); // The last frame comes before Ended is set; take any still on its way.
        if (player.state() != ClipPlayer::State::Ended || current != last) {
            ++problems;
            std::printf("  WRONG %s: not Ended after the last frame (frame %llu shown)\n", what,
                        static_cast<Number>(current));
        }
    };
    // After play(): the frames must go on from `from` one by one (the reader may miss one now and then).
    const auto checkPlaysOn = [&](const char* what, std::uint64_t from, const std::vector<Came>& came) {
        if (came.empty() || came.front().number != from + 1) {
            ++problems;
            std::printf("  WRONG %s: after frame %llu came %s %llu\n", what, static_cast<Number>(from),
                        came.empty() ? "nothing" : "frame",
                        came.empty() ? 0ull : static_cast<Number>(came.front().number));
            return;
        }
        unsigned skipped = 0;
        for (std::size_t i = 1; i < came.size(); ++i) {
            if (came[i].number <= came[i - 1].number) {
                ++problems;
                std::printf("  WRONG %s: frame %llu came after %llu\n", what, static_cast<Number>(came[i].number),
                            static_cast<Number>(came[i - 1].number));
            }
            skipped += static_cast<unsigned>(came[i].number - came[i - 1].number - 1);
        }
        const Came& end = came.back();
        std::printf("  %s: from frame %llu, frame %llu came after %.1f ms (%.1f ms after it in the clip), then on to "
                    "%llu: %zu frames, %u missed by the reader; the clip went %.3f s in %.3f s\n",
                    what, static_cast<Number>(from), static_cast<Number>(from + 1), came.front().ms,
                    (times[static_cast<std::size_t>(from)] - times[static_cast<std::size_t>(from - 1)]) * 1000.0,
                    static_cast<Number>(end.number), came.size(), skipped,
                    times[static_cast<std::size_t>(end.number - 1)] - times[static_cast<std::size_t>(from - 1)],
                    end.ms / 1000.0);
    };
    // The frames that come for `ms` after play() from frame `from`, and at least until the one after it is due.
    const auto playOn = [&](std::uint64_t from, double ms) {
        if (from < last) {
            const double gap = times[static_cast<std::size_t>(from)] - times[static_cast<std::size_t>(from - 1)];
            ms = std::max(ms, gap * 1000.0 + 100.0);
        }
        player.play();
        return collect(ms);
    };

    // What played before the pause.
    collect(300.0);
    std::printf("Seeks while paused (from frame %llu):\n", static_cast<Number>(current));
    Kind ends{"Frame 1, the last"};
    Kind middles{"The middle"};
    Kind randoms{"Random frames"};
    Kind forward{"+1 steps"};
    Kind backward{"-1 steps"};
    seekFrame(ends, 1);
    seekFrame(ends, last);
    seekFrame(middles, middle);
    std::mt19937_64 random(20261111);
    std::uniform_int_distribution<std::uint64_t> anyFrame(1, last);
    for (int i = 0; i < 20; ++i) {
        seekFrame(randoms, anyFrame(random));
    }
    seekFrame(middles, middle);
    for (int i = 0; i < 30 && current < last; ++i) {
        seekFrame(forward, current + 1);
    }
    seekFrame(middles, middle);
    for (int i = 0; i < 30 && current > 1; ++i) {
        seekFrame(backward, current - 1);
    }
    seekAgain();
    Kind keys{"Keyframes"};
    {
        const std::vector<std::uint64_t>& keyframes = player.keyframes();
        std::uniform_int_distribution<std::size_t> anyKey(0, keyframes.size() - 1);
        for (int i = 0; i < 20; ++i) {
            seekFrame(keys, keyframes[anyKey(random)]);
        }
    }
    print(ends);
    print(middles);
    print(randoms);
    print(forward);
    print(backward);
    print(keys);

    // 50 seeks in a row, as a slider dragged fast: the last asked must come, and nothing after it.
    {
        std::vector<std::uint64_t> asked(50);
        for (std::uint64_t& number : asked) {
            number = anyFrame(random);
        }
        while (asked.back() == current && last > 1) {
            asked.back() = anyFrame(random);
        }
        unsigned before = 0;
        const Clock::time_point start = Clock::now();
        for (const std::uint64_t number : asked) {
            player.seek(number);
        }
        const double ms = await(asked.back(), start, 3000.0, &before);
        if (ms < 0.0) {
            ++problems;
            std::printf("  WRONG 50 seeks in a row: the last asked, frame %llu, never came\n",
                        static_cast<Number>(asked.back()));
        } else {
            checkFrame("50 seeks in a row", asked.back(), true, texture.Get());
            const std::vector<Came> after = collect(200.0);
            if (!after.empty()) {
                ++problems;
                std::printf("  WRONG 50 seeks in a row: %zu frames came after the last asked\n", after.size());
            }
            std::printf("  50 seeks in a row: the last asked (frame %llu) came after %.1f ms, %u others before it\n",
                        static_cast<Number>(asked.back()), ms, before);
        }
    }

    // Drags as the clip screen does them: the frame under the mouse each UI frame (7 ms apart, as at 144 Hz), from
    // frame `from` to `to` in `steps`. Counts the pictures that came, and the longest time without a new one.
    const auto drag = [&](const char* what, std::uint64_t from, std::uint64_t to, std::uint64_t steps) {
        std::vector<double> came; // When each picture came, in ms since the drag started.
        std::vector<std::uint64_t> asked;
        const Clock::time_point start = Clock::now();
        for (std::uint64_t i = 0; i <= steps; ++i) {
            const std::uint64_t at = to < from ? from - (from - to) * i / steps : from + (to - from) * i / steps;
            if (asked.empty() || asked.back() != at) {
                asked.push_back(at);
            }
            player.seek(at);
            const double offset = msSince(start);
            for (const Came& one : collect(7.0)) {
                came.push_back(offset + one.ms);
            }
        }
        const double ended = msSince(start);
        double longest = came.empty() ? ended : came.front();
        for (std::size_t i = 1; i < came.size(); ++i) {
            longest = std::max(longest, came[i] - came[i - 1]);
        }
        if (!came.empty()) {
            longest = std::max(longest, ended - came.back());
        }
        std::printf("  %-26s %4zu pictures in %4.0f ms (%4.0f a second) for %4zu frames under the mouse, longest "
                    "without a new one %5.1f ms\n",
                    what, came.size(), ended, static_cast<double>(came.size()) * 1000.0 / ended, asked.size(), longest);
    };
    std::printf("Dragging the timeline:\n");
    drag("Fast back, end to start", last, 1, 143);
    drag("Fast on, start to end", 1, last, 143);
    const std::uint64_t second = std::max<std::uint64_t>(1, last / 10); // About a second of clip.
    drag("Slow back, a second in 2", middle + second / 2, middle - std::min(middle - 1, second / 2), 286);
    drag("Slow on, a second in 2", middle - std::min(middle - 1, second / 2), middle + second / 2, 286);
    collect(100.0);

    std::printf("Playing:\n");
    // play() after a seek plays on from the frame sought, at real speed.
    Kind setup{"Set-up"};
    seekFrame(setup, middle);
    checkPlaysOn("play() after a seek", middle, playOn(middle, 500.0));
    player.pause();
    collect(50.0);

    // A seek while playing: its frame comes and it plays on from it.
    if (last > 200) {
        ComPtr<ID3D11Texture2D> kept;
        check(ui.device->CreateTexture2D(&desc, nullptr, &kept), "CreateTexture2D (clip frame)");
        player.play();
        collect(100.0);
        std::uint64_t target = current;
        while (target + 100 > current && target < current + 100) {
            target = std::uniform_int_distribution<std::uint64_t>(1, last - 80)(random);
        }
        Kind playing{"While playing"};
        const Clock::time_point asked = Clock::now();
        player.seek(target);
        const double ms = await(target, asked, 3000.0, &playing.others);
        if (ms < 0.0) {
            ++problems;
            std::printf("  WRONG while playing: frame %llu never came\n", static_cast<Number>(target));
        } else {
            // Kept for the pixel check after the frames that come next, which reading it back now would delay.
            ui.context->CopyResource(kept.Get(), texture.Get());
            playing.ms.push_back(ms);
            const std::vector<Came> came = collect(300.0);
            checkFrame("while playing", target, false, kept.Get());
            print(playing);
            checkPlaysOn("then", target, came);
        }
        if (player.state() != ClipPlayer::State::Playing) {
            ++problems;
            std::printf("  WRONG while playing: not Playing after the seek\n");
        }
        player.pause();
        collect(50.0);
    }

    // Played to the end: a seek makes it Paused at once and play() goes on from the frame sought; play() alone starts
    // again from frame 1; the last frame sought at the end comes again and play() ends at once.
    const std::uint64_t nearEnd = last > 30 ? last - 30 : 1;
    seekFrame(setup, nearEnd);
    playToEnd("playing to the end");
    {
        const Clock::time_point asked = Clock::now();
        player.seek(middle);
        if (player.state() != ClipPlayer::State::Paused) {
            ++problems;
            std::printf("  WRONG a seek after Ended: the state is not Paused at once\n");
        }
        if (await(middle, asked, 3000.0, nullptr) < 0.0) {
            ++problems;
            std::printf("  WRONG a seek after Ended: frame %llu never came\n", static_cast<Number>(middle));
        } else {
            checkFrame("a seek after Ended", middle, true, texture.Get());
            checkPlaysOn("play() after Ended and a seek", middle, playOn(middle, 100.0));
            player.pause();
            collect(50.0);
        }
    }
    seekFrame(setup, nearEnd);
    playToEnd("playing to the end");
    player.play();
    {
        const std::vector<Came> came = collect(100.0);
        if (came.empty() || came.front().number != 1) {
            ++problems;
            std::printf("  WRONG play() after Ended: frame %llu came first, not frame 1\n",
                        came.empty() ? 0ull : static_cast<Number>(came.front().number));
        } else {
            std::printf("  play() after Ended: frame 1 came after %.1f ms\n", came.front().ms);
        }
    }
    player.pause();
    collect(50.0);
    seekFrame(setup, nearEnd);
    playToEnd("playing to the end");
    {
        const Clock::time_point asked = Clock::now();
        player.seek(last);
        const double ms = await(last, asked, 3000.0, nullptr);
        if (ms < 0.0) {
            ++problems;
            std::printf("  WRONG the last frame sought at the end never came again\n");
        } else {
            checkFrame("the last frame sought at the end", last, true, texture.Get());
            player.play();
            const std::vector<Came> came = collect(200.0);
            if (!came.empty() || player.state() != ClipPlayer::State::Ended) {
                ++problems;
                std::printf("  WRONG play() on the last frame: %zu frames came, %s\n", came.size(),
                            player.state() == ClipPlayer::State::Ended ? "Ended" : "not Ended");
            } else {
                std::printf("  The last frame sought at the end came again after %.1f ms; play() then Ended at once\n",
                            ms);
            }
        }
    }
    if (player.state() == ClipPlayer::State::Failed) {
        ++problems;
        std::printf("  WRONG the player failed: %s\n", player.error().c_str());
    }
    if (problems == 0) {
        std::printf("Every frame was right.\n");
        return 0;
    }
    std::printf("%u problems.\n", problems);
    return 1;
}

// --clip-guesses --snap: Today's sets snapped onto the clip's video frames as the app's snapper does live on every
// screen image (engine/BoxSnapper: the grey images made on the GPU, engine/GreyMaker, only the parts `snapping` reads
// read back, then `snapping`), and by `whole` (made as `snapping` is) on whole images read back, which must come out
// the same.
struct SnappedGuesses {
    Guesses handed;                   // As handed out, from the time the screen showed each frame's image.
    Guesses onImage;                  // Set i: snapped onto frame i's image, where on it (less the lead), so judged at
    std::vector<BoxMoment> ownImage;  // i + 0.5 on its own image: how well snapping finds the things,
    std::vector<BoxMoment> nextImage; // and on the screen's next image: as live shows them, with a lead.
    std::vector<double> nextGapMs;    // How long after its own image the screen showed the next (0: not known).
    std::size_t frames = 0;
    std::size_t boxes = 0;       // Handed out,
    std::size_t snapped = 0;     // of them snapped (the others are where their speed takes them).
    double snapMs = 0.0;         // Snapping, all frames,
    double greyMs = 0.0;         // and making the grey images (on the GPU, the parts read back, waited for).
    double leadMs = 0.0;         // The snapping's own lead.
    std::size_t greyDiffers = 0; // Pixels of the first frame's grey levels that differ from GreyImage's.
    std::size_t areasDiffer = 0; // Boxes handed out unlike `whole`'s.
    std::size_t readPixels = 0;  // Grey pixels read back, all frames (all levels),
    std::size_t wholePixels = 0; // of the whole images' (a frame).
};

// `pixels` (BGRA, `width` x `height`, rows `width * 4` apart) with `box` outlined 2 pixels wide in `colour` (BGRA).
void outline(std::vector<std::uint8_t>& pixels, int width, int height, const Detection& box, std::uint32_t colour) {
    const int left = static_cast<int>(std::lround(box.x));
    const int top = static_cast<int>(std::lround(box.y));
    const int right = static_cast<int>(std::lround(box.x + box.width)) - 1;
    const int bottom = static_cast<int>(std::lround(box.y + box.height)) - 1;
    for (int y = top - 1; y <= bottom + 1; ++y) {
        for (int x = left - 1; x <= right + 1; ++x) {
            const bool edge = x <= left || x >= right || y <= top || y >= bottom;
            if (edge && x >= 0 && x < width && y >= 0 && y < height) {
                std::memcpy(pixels.data() + (static_cast<std::size_t>(y) * static_cast<std::size_t>(width) +
                                             static_cast<std::size_t>(x)) *
                                                4,
                            &colour, 4);
            }
        }
    }
}

SnappedGuesses snappedGuesses(const std::filesystem::path& folder, const ClipText& clip, const Guesses& today,
                              Snapping& snapping, Snapping& whole, const std::vector<std::uint64_t>& at,
                              const char* out) {
    using Clock = std::chrono::steady_clock;
    const auto msSince = [](Clock::time_point start) {
        return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
    };

    // Today's sets are the looks with what the model found, in order (core/ClipGuesses' heuristicGuesses).
    std::vector<const ClipLook*> looked;
    std::map<unsigned long long, double> shownMs; // When the screen showed each image looked at.
    for (const ClipLook& look : clip.looks) {
        if (look.looked) {
            looked.push_back(&look);
            shownMs[look.image] = look.timeMs;
        }
    }
    if (looked.size() != today.sets()) {
        throw std::runtime_error("Today's sets are not the clip's looks");
    }
    const ComPtr<IDXGIAdapter1> adapter = findPrimaryGpu();
    const GpuDevice gpu = createGpuDevice(adapter.Get(), true); // The GPU's video decoder.
    ClipReader reader(gpu.device.Get(), folder / kClipVideo);
    const int width = static_cast<int>(reader.width());
    const int height = static_cast<int>(reader.height());
    GreyMaker greys(gpu.device.Get(), reader.width(), reader.height());
    GreyMaker wholeGreys(gpu.device.Get(), reader.width(), reader.height());
    const GreyArea all[] = {{0, 0, width, height}};
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = reader.width();
    desc.Height = reader.height();
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = ScreenCapture::kImageFormat;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_STAGING;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> staging; // For the CPU's grey image of the first frame and the frames written out.
    check(gpu.device->CreateTexture2D(&desc, nullptr, &staging), "CreateTexture2D (clip frame)");

    snapping.clear();
    whole.clear();
    SnappedGuesses snaps;
    for (int l = 0; l < GreyView::kLevels; ++l) {
        snaps.wholePixels += static_cast<std::size_t>(width >> l) * static_cast<std::size_t>(height >> l);
    }
    std::vector<GreyArea> learnAreas(Found::kMax);
    std::vector<GreyArea> snapAreas(Found::kMax);
    const auto readArea = [&](std::span<const GreyArea> areas) {
        for (const GreyArea& area : areas) {
            for (int l = 0; l < GreyView::kLevels; ++l) {
                const GreyArea part = area.onLevel(l, width >> l, height >> l);
                if (part.x0 < part.x1 && part.y0 < part.y1) {
                    snaps.readPixels +=
                        static_cast<std::size_t>(part.x1 - part.x0) * static_cast<std::size_t>(part.y1 - part.y0);
                }
            }
        }
    };
    snaps.leadMs = snapping.leadMs();
    const auto lead = static_cast<float>(snaps.leadMs);
    std::size_t nextSet = 0; // The first of Today's sets not handed to the snapping yet.
    std::vector<Tracked> handed(Found::kMax);
    std::vector<Tracked> handedWhole(Found::kMax);
    std::vector<Tracked> onImage;
    ClipReader::Frame frame;
    while (reader.next(frame) && frame.number <= clip.frameMs.size()) {
        const unsigned long long image = clip.frameImage[frame.number - 1];
        const auto shown = shownMs.find(image);
        const double imageMs = shown != shownMs.end() ? shown->second : clip.frameMs[frame.number - 1];
        const bool write = out != nullptr && std::find(at.begin(), at.end(), frame.number) != at.end();
        std::vector<std::uint8_t> pixels; // BGRA, rows width * 4 apart.
        if (write || frame.number == 1) {
            gpu.context->CopySubresourceRegion(staging.Get(), 0, 0, 0, 0, frame.texture, frame.subresource, nullptr);
            D3D11_MAPPED_SUBRESOURCE mapped{};
            check(gpu.context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped), "Map (clip frame)");
            pixels.resize(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4);
            for (int y = 0; y < height; ++y) {
                std::memcpy(pixels.data() + static_cast<std::size_t>(y) * static_cast<std::size_t>(width) * 4,
                            static_cast<const std::uint8_t*>(mapped.pData) +
                                static_cast<std::size_t>(y) * mapped.RowPitch,
                            static_cast<std::size_t>(width) * 4);
            }
            gpu.context->Unmap(staging.Get(), 0);
        }
        // On whole images, to check the parts read back by.
        gpu.context->CopySubresourceRegion(wholeGreys.input(), 0, 0, 0, 0, frame.texture, frame.subresource, nullptr);
        wholeGreys.make(gpu.context.Get(), image);
        wholeGreys.read(gpu.context.Get(), image, all);
        wholeGreys.wait(gpu.context.Get());
        const GreyView& wholeGrey = *wholeGreys.find(image);
        if (frame.number == 1) {
            // The GPU's grey levels against the CPU's.
            GreyImage cpu;
            cpu.fromBgra(pixels.data(), width, height, static_cast<std::ptrdiff_t>(width) * 4);
            const GreyView expected = cpu.view();
            for (int l = 0; l < GreyView::kLevels; ++l) {
                for (int y = 0; y < expected.height(l); ++y) {
                    for (int x = 0; x < expected.width(l); ++x) {
                        snaps.greyDiffers += expected.row(l, y)[x] != wholeGrey.row(l, y)[x] ? 1 : 0;
                    }
                }
            }
        }
        std::size_t wholeSet = nextSet;
        for (; wholeSet < today.sets() && today.fromMs(wholeSet) <= imageMs; ++wholeSet) {
            whole.looked(looked[wholeSet]->timeMs, wholeGreys.find(looked[wholeSet]->image), today.set(wholeSet));
        }
        const int countWhole = whole.snap(imageMs, wholeGrey, handedWhole.data(), static_cast<int>(handedWhole.size()));

        // As live: the sets done by the time the screen showed this image, each learned from its own image if it is
        // still kept, the last one's parts read back with this image's (live, only the newest look is handed over).
        Clock::time_point start = Clock::now();
        gpu.context->CopySubresourceRegion(greys.input(), 0, 0, 0, 0, frame.texture, frame.subresource, nullptr);
        greys.make(gpu.context.Get(), image);
        for (; nextSet + 1 < wholeSet; ++nextSet) {
            const int learnCount = snapping.learnAreas(today.set(nextSet), learnAreas.data(), Found::kMax);
            const std::span<const GreyArea> areas(learnAreas.data(), static_cast<std::size_t>(learnCount));
            greys.read(gpu.context.Get(), looked[nextSet]->image, areas);
            readArea(areas);
            greys.wait(gpu.context.Get());
            snapping.looked(looked[nextSet]->timeMs, greys.find(looked[nextSet]->image), today.set(nextSet));
        }
        const bool look = nextSet < wholeSet;
        const double lookMs = look ? looked[nextSet]->timeMs : 0.0;
        const std::span<const Tracked> shownThen = look ? today.set(nextSet) : std::span<const Tracked>();
        if (look) {
            const int learnCount = snapping.learnAreas(shownThen, learnAreas.data(), Found::kMax);
            const std::span<const GreyArea> areas(learnAreas.data(), static_cast<std::size_t>(learnCount));
            greys.read(gpu.context.Get(), looked[nextSet]->image, areas);
            readArea(areas);
        }
        const int snapCount = snapping.snapAreas(imageMs, look, lookMs, shownThen, snapAreas.data(), Found::kMax);
        const std::span<const GreyArea> areas(snapAreas.data(), static_cast<std::size_t>(snapCount));
        greys.read(gpu.context.Get(), image, areas);
        readArea(areas);
        greys.wait(gpu.context.Get());
        snaps.greyMs += msSince(start);
        start = Clock::now();
        if (look) {
            snapping.looked(lookMs, greys.find(looked[nextSet]->image), shownThen);
            ++nextSet;
        }
        const int count = snapping.snap(imageMs, *greys.find(image), handed.data(), static_cast<int>(handed.size()));
        snaps.snapMs += msSince(start);
        for (int i = 0; i < std::max(count, countWhole); ++i) {
            const Tracked& a = handed[static_cast<std::size_t>(i)];
            const Tracked& b = handedWhole[static_cast<std::size_t>(i)];
            snaps.areasDiffer += i >= count || i >= countWhole || a.x != b.x || a.y != b.y || a.held != b.held ? 1 : 0;
        }
        onImage.clear();
        for (int i = 0; i < count; ++i) {
            Tracked thing = handed[static_cast<std::size_t>(i)];
            snaps.snapped += thing.held ? 0 : 1;
            thing.x -= thing.speedX * lead;
            thing.y -= thing.speedY * lead;
            onImage.push_back(thing);
        }
        snaps.boxes += static_cast<std::size_t>(count);
        snaps.handed.add(imageMs, {handed.data(), static_cast<std::size_t>(count)});
        const auto set = static_cast<double>(snaps.frames++);
        snaps.onImage.add(set, onImage);
        snaps.ownImage.push_back({set + 0.5, image});
        snaps.nextImage.push_back({set + 0.5, image + 1});
        const auto next = shownMs.find(image + 1);
        snaps.nextGapMs.push_back(next != shownMs.end() ? next->second - imageMs : 0.0);

        if (write) {
            // Around the boxes: Today's as the clip screen draws them over this frame, then the snapped ones as
            // handed out.
            float left = static_cast<float>(width);
            float top = static_cast<float>(height);
            float right = 0.0f;
            float bottom = 0.0f;
            const double frameMs = clip.frameMs[frame.number - 1];
            for (const Tracked& thing : today.at(frameMs)) {
                outline(pixels, width, height, thing, 0xFFFF2020u);
            }
            for (int i = 0; i < count; ++i) {
                const Tracked& thing = handed[static_cast<std::size_t>(i)];
                outline(pixels, width, height, thing, 0xFF20FF20u);
                left = std::min(left, thing.x);
                top = std::min(top, thing.y);
                right = std::max(right, thing.x + thing.width);
                bottom = std::max(bottom, thing.y + thing.height);
            }
            const int x0 = std::clamp(static_cast<int>(left) - 80, 0, width - 1);
            const int y0 = std::clamp(static_cast<int>(top) - 80, 0, height - 1);
            const int x1 = std::clamp(static_cast<int>(right) + 80, x0 + 1, width);
            const int y1 = std::clamp(static_cast<int>(bottom) + 80, y0 + 1, height);
            char name[32];
            std::snprintf(name, sizeof(name), "frame-%06llu.png", static_cast<unsigned long long>(frame.number));
            std::filesystem::create_directories(out);
            writeImageFile(
                std::filesystem::path(out) / name, ImageFormat::Png,
                pixels.data() +
                    (static_cast<std::size_t>(y0) * static_cast<std::size_t>(width) + static_cast<std::size_t>(x0)) * 4,
                static_cast<unsigned>(x1 - x0), static_cast<unsigned>(y1 - y0), static_cast<unsigned>(width) * 4);
        }
    }
    return snaps;
}

// A snapping that does what another does and keeps each image's searches, for --today-out.
class NotedSnapping final : public Snapping {
public:
    struct Image {
        double timeMs = 0.0;
        double lookMs = -1.0; // The look taken in just before (its image's time; -1: none).
        std::vector<SnapFind> finds;
    };

    explicit NotedSnapping(Snapping& inner) : inner_(inner) {}

    void looked(double lookMs, const GreyView* image, std::span<const Tracked> shown) override {
        lookMs_ = lookMs;
        inner_.looked(lookMs, image, shown);
    }
    int snap(double timeMs, const GreyView& image, Tracked* out, int max) override {
        const int written = inner_.snap(timeMs, image, out, max);
        const std::span<const SnapFind> finds = inner_.finds();
        images_.push_back({timeMs, lookMs_, {finds.begin(), finds.end()}});
        lookMs_ = -1.0;
        return written;
    }
    int learnAreas(std::span<const Tracked> shown, GreyArea* out, int max) override {
        return inner_.learnAreas(shown, out, max);
    }
    int snapAreas(double timeMs, bool look, double lookMs, std::span<const Tracked> shown, GreyArea* out,
                  int max) override {
        return inner_.snapAreas(timeMs, look, lookMs, shown, out, max);
    }
    double leadMs() const override { return inner_.leadMs(); }
    std::span<const SnapFind> finds() const override { return inner_.finds(); }
    void replay(std::span<const SnapFind> recorded) override { inner_.replay(recorded); }
    void clear() override {
        inner_.clear();
        images_.clear();
        lookMs_ = -1.0;
    }

    const std::vector<Image>& images() const { return images_; }

private:
    Snapping& inner_;
    double lookMs_ = -1.0;
    std::vector<Image> images_;
};

// --clip-guesses: compares the clip screen's guessers on a saved clip, box by box.
int clipGuesses(const Options& options) {
    namespace fs = std::filesystem;
    fs::path path(options.clipGuesses);
    if (fs::is_directory(path)) {
        path /= kClipText;
    }
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        std::printf("Cannot read %s\n", path.string().c_str());
        return 1;
    }
    const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    const ClipText clip = readClipText(text);
    const auto unknown = std::find(clip.kinds.begin(), clip.kinds.end(), kUnknownKind);
    const int unknownKind = unknown == clip.kinds.end() ? -1 : static_cast<int>(unknown - clip.kinds.begin());
    const Guesses recorded = recordedGuesses(clip);
    std::unique_ptr<Heuristics> heuristics;
    std::unique_ptr<Snapping> snapping;
    std::unique_ptr<Snapping> wholeSnapping;
    std::unique_ptr<Snapping> replaySnapping;
    if (options.heuristics != nullptr) {
        try {
            const std::shared_ptr<HeuristicsLibrary> library =
                HeuristicsLibrary::load(fs::absolute(options.heuristics));
            heuristics = library->make();
            snapping = library->makeSnapping();
            wholeSnapping = library->makeSnapping();
            replaySnapping = library->makeSnapping();
        } catch (const std::exception& e) {
            std::printf("Cannot load %s: %s\n", options.heuristics, e.what());
            return 1;
        }
    } else {
        heuristics = std::make_unique<Rules>();
        snapping = std::make_unique<SnapRules>();
        wholeSnapping = std::make_unique<SnapRules>();
        replaySnapping = std::make_unique<SnapRules>();
    }
    const Guesses today = heuristicGuesses(clip, *heuristics);
    if (options.looksOut != nullptr) {
        std::string lines; // A set for each look looked at (heuristicGuesses), in order.
        std::size_t set = 0;
        for (const ClipLook& look : clip.looks) {
            if (look.looked && set < today.sets()) {
                appendShown(lines, look.image, today.fromMs(set), today.set(set));
                ++set;
            }
        }
        std::ofstream(options.looksOut, std::ios::binary) << lines;
    }
    std::printf("%s: %zu frames, %zu looks, %zu kinds; Today from %s\n", path.string().c_str(), clip.frameMs.size(),
                clip.looks.size(), clip.kinds.size(), options.heuristics != nullptr ? options.heuristics : "the bench");
    const double fromMs = settledMs(clip);
    const auto print = [fromMs](const char* name, const Guesses& guesses) {
        const Guesses::Summary summary = guesses.summary(fromMs);
        std::printf("  %-9s %5zu sets, %5.1f boxes at once, %5.1f coming or going a second\n", name, guesses.sets(),
                    summary.things, summary.changes);
    };
    print("Recorded", recorded);
    print("Today", today);

    const auto percent = [](std::size_t part, std::size_t whole) {
        return whole == 0 ? 0.0 : 100.0 * static_cast<double>(part) / static_cast<double>(whole);
    };
    const auto printDifference = [&](const char* what, const GuessesDifference& difference) {
        std::printf("%s, from %.1f s on:\n", what, fromMs / 1000.0);
        std::printf("  sets drawn the same  %zu of %zu (%.1f%%)\n", difference.same, difference.sets,
                    percent(difference.same, difference.sets));
        std::printf("  boxes in both        %zu; tagged differently %zu, moved %zu\n", difference.matched,
                    difference.tagDiffers, difference.moved);
        std::printf("  boxes only the first %zu, only the second %zu\n", difference.onlyFirst, difference.onlySecond);
        if (!difference.firstDiffering.empty()) {
            std::printf("  first that differ at");
            for (const double ms : difference.firstDiffering) {
                std::printf(" %.4f", ms / 1000.0);
            }
            std::printf(" s, the last at %.4f s\n", difference.lastDiffering / 1000.0);
        }
    };
    // The heuristics' own sets against the shown lines alone: with the heuristics the clip was recorded with, the same
    // (the snapper's boxes then come on top of them).
    Guesses shownLines;
    for (const ClipLook& look : clip.looks) {
        if (look.showed) {
            shownLines.add(look.doneMs, look.shown);
        }
    }
    printDifference("Today's heuristics (not snapped) against the shown lines",
                    compareGuesses(shownLines, today, unknownKind, fromMs));
    if (clip.snaps.empty()) {
        printDifference("Today (not snapped) against Recorded", compareGuesses(recorded, today, unknownKind, fromMs));
    }

    const BoxPrecision recordedPrecision = boxPrecision(clip, recorded, fromMs);
    const BoxPrecision todayPrecision = boxPrecision(clip, today, fromMs);
    std::printf("Boxes against what the model found on each frame's own image (%zu frames; middles off: on average,"
                " at most for nine in ten; in the middle half of the model's box, outside it):\n",
                recordedPrecision.frames);
    const auto printPart = [&percent](const char* name, const BoxPrecision::Part& part) {
        std::printf("    %-8s %6zu boxes, off %5.1f px, %5.1f px; in the middle half %5.1f%%, outside %5.1f%%\n", name,
                    part.boxes, part.meanOff, part.mostOff, percent(part.inMiddle, part.boxes),
                    percent(part.outside, part.boxes));
    };
    const auto printPrecision = [&printPart](const char* name, const BoxPrecision& precision) {
        std::printf("  %s (%zu boxes on nothing the model found; of its %zu things at least %.0f%% sure, %zu with no "
                    "box):\n",
                    name, precision.unpaired, precision.truths, static_cast<double>(BoxPrecision::kTruthSure * 100.0f),
                    precision.missed);
        printPart("all", precision.all);
        printPart("moving", precision.moving);
    };
    printPrecision("Recorded", recordedPrecision);
    printPrecision("Today (not snapped)", todayPrecision);

    // Today as the clip screen makes it: snapped onto the video by engine/ClipSnapper, taking the searches recorded
    // live where they are the same.
    if (!clip.snaps.empty() || options.snap) {
        const bool com = SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED)); // Media Foundation needs it.
        try {
            const ComPtr<IDXGIAdapter1> adapter = findPrimaryGpu();
            const GpuDevice gpu = createGpuDevice(adapter.Get(), true);
            const auto start = std::chrono::steady_clock::now();
            NotedSnapping noted(*replaySnapping);
            const Guesses snapped =
                ClipSnapper::snap(gpu.device.Get(), path.parent_path() / kClipVideo, clip, today, noted);
            const double ms =
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
            std::size_t recordedFinds = 0;
            std::size_t searches = 0;
            for (const ClipSnap& snap : clip.snaps) {
                recordedFinds += snap.hasFinds ? 1 : 0;
                searches += snap.finds.size();
            }
            std::printf("Today snapped as the clip screen does (%zu images in %.0f ms; %zu of %zu snapped images have"
                        " their searches recorded, %zu searches):\n",
                        snapSteps(clip).size(), ms, recordedFinds, clip.snaps.size(), searches);
            print("Today", snapped);
            if (options.todayOut != nullptr) {
                // A shown line per set, and after one snapped onto an image a finds line of that image's searches.
                const std::vector<SnapStep> steps = snapSteps(clip);
                const std::vector<NotedSnapping::Image>& images = noted.images();
                std::string lines;
                std::size_t step = 0;
                std::size_t image = 0;
                for (std::size_t set = 0; set < snapped.sets(); ++set) {
                    const double from = snapped.fromMs(set);
                    while (step < steps.size() && steps[step].doneMs < from) {
                        ++step;
                    }
                    const bool snappedOnto = step < steps.size() && steps[step].doneMs == from;
                    appendShown(lines, snappedOnto ? steps[step].image : 0, from, snapped.set(set));
                    if (!snappedOnto) {
                        continue;
                    }
                    while (image < images.size() && images[image].timeMs < steps[step].timeMs) {
                        ++image;
                    }
                    if (image < images.size() && images[image].timeMs == steps[step].timeMs) {
                        unsigned long long lookImage = 0;
                        for (const ClipLook& look : clip.looks) {
                            if (images[image].lookMs >= 0.0 && look.timeMs == images[image].lookMs) {
                                lookImage = look.image;
                            }
                        }
                        appendFinds(lines, steps[step].image, lookImage, images[image].finds);
                    }
                }
                std::ofstream(options.todayOut, std::ios::binary) << lines;
            }
            printDifference("Today snapped against Recorded", compareGuesses(recorded, snapped, unknownKind, fromMs));
            printPrecision("Today snapped", boxPrecision(clip, snapped, fromMs));
        } catch (const std::exception& e) {
            std::printf("Cannot snap Today: %s\n", e.what());
        }
        if (com) {
            CoUninitialize();
        }
    }
    if (options.snap) {
        try {
            const SnappedGuesses snapped =
                snappedGuesses(path.parent_path(), clip, today, *snapping, *wholeSnapping, options.at, options.out);
            const double frames = std::max<double>(1.0, static_cast<double>(snapped.frames));
            std::printf("Today's sets snapped onto each video frame (%zu frames, the GPU's grey images %s; %zu boxes,"
                        " %.1f%% snapped; %.2f ms a frame snapping, and %.2f making the grey image):\n",
                        snapped.frames, snapped.greyDiffers == 0 ? "the same as the CPU's" : "DIFFERENT from the CPU's",
                        snapped.boxes, percent(snapped.snapped, snapped.boxes), snapped.snapMs / frames,
                        snapped.greyMs / frames);
            if (snapped.greyDiffers != 0) {
                std::printf("  %zu grey pixels of the first frame differ\n", snapped.greyDiffers);
            }
            print("Snapped", snapped.handed);
            std::printf(
                "  Read back %.1f%% of the grey images (%.0f KB a frame); boxes unlike on whole images: %zu%s\n",
                percent(snapped.readPixels, snapped.wholePixels * snapped.frames),
                static_cast<double>(snapped.readPixels) / frames / 1024.0, snapped.areasDiffer,
                snapped.areasDiffer == 0 ? "" : " (the parts read back MISS what snapping reads)");
            printPrecision("On its own image (where it was snapped)",
                           boxPrecision(clip, snapped.onImage, snapped.ownImage));
            // Handed out ahead by `lead`, on the screen's next image (it is shown until the next image's is made).
            std::printf("  As handed out, on the screen's next image (the snapping's lead is %.1f ms):\n",
                        snapped.leadMs);
            // -1: by the time until the next image, as if it were known (it is not, live): what is left is how
            // unevenly the game moves things from image to image.
            for (const double lead : {0.0, 3.0, 4.5, 6.0, 7.5, 9.0, 12.0, -1.0}) {
                Guesses ahead;
                for (std::size_t set = 0; set < snapped.onImage.sets(); ++set) {
                    std::vector<Tracked> things(snapped.onImage.set(set).begin(), snapped.onImage.set(set).end());
                    const auto by = static_cast<float>(lead >= 0.0 ? lead : snapped.nextGapMs[set]);
                    for (Tracked& thing : things) {
                        thing.x += thing.speedX * by;
                        thing.y += thing.speedY * by;
                    }
                    ahead.add(snapped.onImage.fromMs(set), things);
                }
                const BoxPrecision precision = boxPrecision(clip, ahead, snapped.nextImage);
                char name[32];
                std::snprintf(name, sizeof(name), lead >= 0.0 ? "%4.1f ms" : "to next", lead);
                printPart(name, precision.moving);
                if (lead == snapped.leadMs) {
                    printPart("  all", precision.all);
                }
            }
        } catch (const std::exception& e) {
            std::printf("Cannot snap: %s\n", e.what());
            return 1;
        }
    }
    return 0;
}

// --ui: finds the game's interface on every frame of a clip as the UI flag does (core/UiLayout from --layout, default
// models/ui; the grey image made on the CPU, as the GPU makes it live), prints how long a find takes and how steady
// what it finds is (pieces appearing, going or moving 4+ pixels from one frame to the next), and with --out writes
// ui-found.txt (every frame's pieces) and frame 1, every Nth, the last and each --at as a JPEG with what it found
// outlined: framed windows yellow, ui.txt's pieces blue. CLIP may also be a picture
// (a PNG, JPEG or BMP, e.g. the game's own screenshot): then only its pieces and where each anchor hit.
int uiClip(const Options& options) {
    namespace fs = std::filesystem;
    using Clock = std::chrono::steady_clock;
    fs::path video(options.ui);
    if (fs::is_directory(video)) {
        video /= kClipVideo;
    }
    const fs::path layoutFolder = options.layout != nullptr ? fs::path(options.layout) : fs::path("models/ui");
    UiLayout layout;
    std::string error;
    if (!layout.load(layoutFolder, &error)) {
        std::printf("%s\n", error.c_str());
        return 1;
    }
    // Framed windows yellow, ui.txt's pieces blue, 3 pixels wide.
    const auto outlinePieces = [](std::vector<std::uint8_t>& pixels, int width, int height, const UiPiece* pieces,
                                  int count) {
        for (int i = 0; i < count; ++i) {
            const UiPiece& piece = pieces[i];
            const bool window = std::strcmp(piece.name.data(), "Window") == 0;
            for (int inset = 0; inset < 3; ++inset) {
                const Detection box{static_cast<float>(piece.x0 + inset), static_cast<float>(piece.y0 + inset),
                                    static_cast<float>(piece.x1 - piece.x0 - 2 * inset),
                                    static_cast<float>(piece.y1 - piece.y0 - 2 * inset), 1.0f};
                outline(pixels, width, height, box, window ? 0xFFFFFF00u : 0xFF0090FFu);
            }
        }
    };
    // A picture instead of a clip (e.g. the game's own screenshot, Screenshot/ShotNNNNN.bmp): its pieces and where
    // each anchor hit, and with --out the picture with them outlined (ui-picture.jpg).
    if (fs::is_regular_file(video) && video.extension() != L".mp4") {
        ImageData picture = readImageFile(video);
        const int width = static_cast<int>(picture.width);
        const int height = static_cast<int>(picture.height);
        GreyImage grey;
        grey.fromBgra(picture.bgra.data(), width, height, static_cast<std::ptrdiff_t>(width) * 4);
        std::vector<UiPiece> found(UiPieces::kMax);
        const Clock::time_point start = Clock::now();
        const int count = layout.find(grey.view().levels[0], found.data(), static_cast<int>(found.size()));
        const double ms = std::chrono::duration<double, std::milli>(Clock::now() - start).count();
        std::printf("%s: %d x %d; %d pieces in %.2f ms:\n", toUtf8(video.c_str()).c_str(), width, height, count, ms);
        for (int i = 0; i < count; ++i) {
            const UiPiece& piece = found[static_cast<std::size_t>(i)];
            std::printf("  %s %d %d %d %d\n", piece.name.data(), piece.x0, piece.y0, piece.x1, piece.y1);
        }
        std::printf("Where each anchor is (x y score):\n%s", layout.describeHits().c_str());
        if (options.out != nullptr) {
            fs::create_directories(options.out);
            outlinePieces(picture.bgra, width, height, found.data(), count);
            writeImageFile(fs::path(options.out) / "ui-picture.jpg", ImageFormat::Jpeg, picture.bgra.data(),
                           picture.width, picture.height, picture.width * 4);
        }
        return 0;
    }
    const ComPtr<IDXGIAdapter1> adapter = findPrimaryGpu();
    const GpuDevice gpu = createGpuDevice(adapter.Get(), true); // Video support, for the GPU's video decoder.
    ClipReader reader(gpu.device.Get(), video);
    const int width = static_cast<int>(reader.width());
    const int height = static_cast<int>(reader.height());
    const std::vector<double>& times = reader.times();
    std::printf("%s: %d x %d, %zu frames; %s: %d anchors, %d pieces\n", toUtf8(video.c_str()).c_str(), width, height,
                times.size(), toUtf8(layoutFolder.c_str()).c_str(), layout.anchors(), layout.pieces());

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
    std::vector<std::uint8_t> pixels(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4);
    GreyImage grey;
    std::vector<UiPiece> found(UiPieces::kMax);
    std::vector<UiPiece> before;
    std::ofstream list;
    if (options.out != nullptr) {
        fs::create_directories(options.out);
        list.open(fs::path(options.out) / "ui-found.txt", std::ios::binary);
    }
    const unsigned every = std::max(options.frameEvery, 1u);
    double findSum = 0.0;
    double findMax = 0.0;
    double framesSum = 0.0;
    double anchorsSum = 0.0;
    std::uint64_t frames = 0;
    std::uint64_t changes = 0;
    std::map<std::string, std::uint64_t> framesWith;
    const auto same = [](const UiPiece& a, const UiPiece& b) {
        return std::strcmp(a.name.data(), b.name.data()) == 0 && std::abs(a.x0 - b.x0) < 4 &&
               std::abs(a.y0 - b.y0) < 4 && std::abs(a.x1 - b.x1) < 4 && std::abs(a.y1 - b.y1) < 4;
    };
    ClipReader::Frame frame;
    while (reader.next(frame)) {
        gpu.context->CopySubresourceRegion(staging.Get(), 0, 0, 0, 0, frame.texture, frame.subresource, nullptr);
        D3D11_MAPPED_SUBRESOURCE mapped{};
        check(gpu.context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped), "Map (clip frame)");
        for (int y = 0; y < height; ++y) {
            std::memcpy(pixels.data() + static_cast<std::size_t>(y) * static_cast<std::size_t>(width) * 4,
                        static_cast<const std::uint8_t*>(mapped.pData) + static_cast<std::size_t>(y) * mapped.RowPitch,
                        static_cast<std::size_t>(width) * 4);
        }
        gpu.context->Unmap(staging.Get(), 0);
        grey.fromBgra(pixels.data(), width, height, static_cast<std::ptrdiff_t>(width) * 4);
        const GreyView view = grey.view();
        const Clock::time_point start = Clock::now();
        const int count = layout.find(view.levels[0], found.data(), static_cast<int>(found.size()));
        const double ms = std::chrono::duration<double, std::milli>(Clock::now() - start).count();
        ++frames;
        findSum += ms;
        findMax = std::max(findMax, ms);
        framesSum += layout.framesMs();
        anchorsSum += layout.anchorsMs();

        std::map<std::string, int> names;
        for (int i = 0; i < count; ++i) {
            const UiPiece& piece = found[static_cast<std::size_t>(i)];
            ++names[piece.name.data()];
            if (frames > 1) { // The first frame's pieces are not changes.
                changes += std::none_of(before.begin(), before.end(), [&](const UiPiece& b) { return same(piece, b); });
            }
        }
        for (const UiPiece& b : before) {
            changes += std::none_of(found.begin(), found.begin() + count, [&](const UiPiece& a) { return same(a, b); });
        }
        for (const auto& [name, n] : names) {
            ++framesWith[name];
        }
        before.assign(found.begin(), found.begin() + count);
        if (std::find(options.at.begin(), options.at.end(), frame.number) != options.at.end()) {
            std::printf("Frame %llu, where each anchor is (x y score):\n%s",
                        static_cast<unsigned long long>(frame.number), layout.describeHits().c_str());
        }
        if (list.is_open()) {
            list << frame.number;
            for (int i = 0; i < count; ++i) {
                const UiPiece& piece = found[static_cast<std::size_t>(i)];
                list << "  " << piece.name.data() << ' ' << piece.x0 << ' ' << piece.y0 << ' ' << piece.x1 << ' '
                     << piece.y1;
            }
            list << '\n';
        }
        const bool due = (frame.number - 1) % every == 0 || frame.number == times.size() ||
                         std::find(options.at.begin(), options.at.end(), frame.number) != options.at.end();
        if (options.out != nullptr && due) {
            outlinePieces(pixels, width, height, found.data(), count);
            char name[32];
            std::snprintf(name, sizeof(name), "ui-%06llu.jpg", static_cast<unsigned long long>(frame.number));
            writeImageFile(fs::path(options.out) / name, ImageFormat::Jpeg, pixels.data(), reader.width(),
                           reader.height(), reader.width() * 4);
        }
    }
    if (frames == 0) {
        std::printf("No frames\n");
        return 1;
    }
    std::printf("Find: %.2f ms a frame on average (framed windows %.2f, anchors and pieces %.2f), %.2f at most\n",
                findSum / static_cast<double>(frames), framesSum / static_cast<double>(frames),
                anchorsSum / static_cast<double>(frames), findMax);
    std::printf("Changes (a piece appearing, going or moving 4+ px from the frame before): %llu, %.1f a second\n",
                static_cast<unsigned long long>(changes), static_cast<double>(changes) / std::max(times.back(), 0.001));
    for (const auto& [name, n] : framesWith) {
        std::printf("  %-12s on %llu of %llu frames\n", name.c_str(), static_cast<unsigned long long>(n),
                    static_cast<unsigned long long>(frames));
    }
    return 0;
}

// --ground: follows the game's camera over a clip as the Ground flag does on the clip screen (engine/ClipGround, by
// the recorded guesses; the interface from --layout, default models/ui; the following built into the bench, or with
// --heuristics an observex_heuristics.dll's, loaded as the app loads it), prints how the follow went and the camera
// every half second, and with --out writes frame 1, every Nth, the last and each --at as a JPEG with the ground's grid
// drawn as the flag draws it (its lines; no fill).
int groundClip(const Options& options) {
    namespace fs = std::filesystem;
    using Clock = std::chrono::steady_clock;
    fs::path folder(options.ground);
    fs::path video = folder;
    if (fs::is_directory(folder)) {
        video /= kClipVideo;
    } else {
        folder = folder.parent_path();
    }
    std::ifstream file(folder / kClipText, std::ios::binary);
    if (!file) {
        std::printf("No %s in %s\n", toUtf8(fs::path(kClipText).c_str()).c_str(), toUtf8(folder.c_str()).c_str());
        return 1;
    }
    const std::string text{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
    const ClipText clip = readClipText(text);
    const Guesses shown = recordedGuesses(clip);
    const fs::path layoutFolder = options.layout != nullptr ? fs::path(options.layout) : fs::path("models/ui");
    UiLayout layout;
    std::string error;
    if (!layout.load(layoutFolder, &error)) {
        std::printf("Only framed windows are left out: %s\n", error.c_str());
    }
    std::unique_ptr<Grounding> ground;
    if (options.heuristics != nullptr) {
        try {
            ground = HeuristicsLibrary::load(fs::absolute(options.heuristics))->makeGround();
        } catch (const std::exception& e) {
            std::printf("Cannot load %s: %s\n", options.heuristics, e.what());
            return 1;
        }
    } else {
        ground = std::make_unique<GroundFollower>();
    }
    const ComPtr<IDXGIAdapter1> adapter = findPrimaryGpu();
    const GpuDevice gpu = createGpuDevice(adapter.Get(), true); // Video support, for the GPU's video decoder.
    ClipGround::Stats stats;
    const Clock::time_point start = Clock::now();
    const std::vector<GroundCamera> cameras = ClipGround::follow(gpu.device.Get(), *ground, video, clip, shown, &layout,
                                                                 {}, nullptr, &stats, options.groundFrames);
    const double ms = std::chrono::duration<double, std::milli>(Clock::now() - start).count();
    std::printf("%s: followed %d frames in %.0f ms (%.2f ms each reading back and fitting); fitted %d (%.0f%%), "
                "%.1f patches found a frame, %.1f agreeing, %.2f px off\n",
                toUtf8(video.c_str()).c_str(), stats.images, ms, stats.workMs, stats.fitted,
                stats.images > 0 ? 100.0 * stats.fitted / stats.images : 0.0, stats.matches, stats.agreeing,
                stats.missPixels);
    std::printf("  motions: fitted %d, no camera %d, no own character %d, few patches found %d, few agreeing %d, "
                "too far %d, searched %d\n",
                stats.motions[0], stats.motions[1], stats.motions[2], stats.motions[3], stats.motions[4],
                stats.motions[5], stats.motions[6]);
    std::printf("  minimap cone seen on %d of %d frames; between them read on %d, seen on %d\n", stats.headed,
                stats.images, stats.heads, stats.headsSeen);
    // Today against Recorded: the grid the app drew live (clip.txt's ground lines, since 2026-11-12) on the same frames.
    if (clip.grounds.empty()) {
        std::printf("  Recorded: no ground lines (saved before clips kept the grid)\n");
    } else {
        constexpr double kDegrees = 57.29578;
        int both = 0;
        int onlyToday = 0;
        int onlyRecorded = 0;
        double pitchOff = 0.0;
        double yawOff = 0.0;
        double distanceOff = 0.0;
        double pitchMost = 0.0;
        double yawMost = 0.0;
        for (const GroundCamera& today : cameras) {
            const GroundCamera recorded = recordedGround(clip.grounds, today.timeMs);
            if (today.known && recorded.known) {
                ++both;
                const double pitch = std::fabs(today.pitch - recorded.pitch) * kDegrees;
                const double yaw = std::fabs(std::remainder(today.yaw - recorded.yaw, 6.283185307)) * kDegrees;
                pitchOff += pitch;
                yawOff += yaw;
                distanceOff += std::fabs(today.distance - recorded.distance);
                pitchMost = std::max(pitchMost, pitch);
                yawMost = std::max(yawMost, yaw);
            } else if (today.known) {
                ++onlyToday;
            } else if (recorded.known) {
                ++onlyRecorded;
            }
        }
        std::printf("  against Recorded (%zu ground lines): %d frames both drawn, pitch off %.2f deg (most %.1f), yaw off "
                    "%.2f deg (most %.1f), distance off %.3f; only Today %d, only Recorded %d\n",
                    clip.grounds.size(), both, both > 0 ? pitchOff / both : 0.0, pitchMost,
                    both > 0 ? yawOff / both : 0.0, yawMost, both > 0 ? distanceOff / both : 0.0, onlyToday,
                    onlyRecorded);
    }
    const double firstMs = clip.frameMs.empty() ? 0.0 : clip.frameMs.front();
    static constexpr const char* kMotions[] = {"fitted",       "no camera", "no own",  "few found",
                                               "few agreeing", "too far",   "searched"};
    for (const ClipGround::Stats::Frame& noted : stats.frames) {
        constexpr double kDegrees = 57.29578;
        std::printf("  %8.0f ms  learned %2d  turn %6.3f  match %5.2f  found %2d  moved %6.1f px  %-12s agreeing %2d"
                    "  minimap %6.1f  yaw %7.1f  pitch %5.1f  distance %5.2f\n",
                    noted.timeMs - firstMs, noted.learned, static_cast<double>(noted.turn),
                    static_cast<double>(noted.match), noted.found, static_cast<double>(noted.moved),
                    kMotions[static_cast<std::size_t>(noted.motion)], noted.agreeing,
                    noted.heading < 0.0f ? -1.0 : noted.heading * kDegrees, noted.yaw * kDegrees,
                    noted.pitch * kDegrees, static_cast<double>(noted.distance));
    }
    for (const GroundCamera& camera : options.groundFrames ? std::span<const GroundCamera>(cameras)
                                                           : std::span<const GroundCamera>()) {
        std::printf("  %8.1f ms  camera %s  yaw %8.2f  pitch %6.2f\n", camera.timeMs - firstMs,
                    camera.known ? "known" : "unknown", static_cast<double>(camera.yaw) * 57.29578,
                    static_cast<double>(camera.pitch) * 57.29578);
    }
    double nextMs = -1e300;
    for (const GroundCamera& camera : cameras) {
        if (camera.timeMs < nextMs) {
            continue;
        }
        nextMs = camera.timeMs + 500.0;
        if (!camera.known) {
            std::printf("  %8.0f ms  unknown\n", camera.timeMs - firstMs);
            continue;
        }
        std::printf("  %8.0f ms  pitch %5.1f%s  distance %5.2f  turned %7.1f  feet %6.2f %6.2f\n",
                    camera.timeMs - firstMs, static_cast<double>(camera.pitch) * 57.29578,
                    camera.pitchMeasured ? " " : "?", static_cast<double>(camera.distance),
                    static_cast<double>(camera.yaw) * 57.29578, static_cast<double>(camera.feetX),
                    static_cast<double>(camera.feetY));
    }
    if (options.out == nullptr) {
        return 0;
    }

    // The frames with the grid, as app/Overlay's drawGround draws it (its lines: lime, fainter farther out).
    fs::create_directories(options.out);
    ClipReader reader(gpu.device.Get(), video);
    const int width = static_cast<int>(reader.width());
    const int height = static_cast<int>(reader.height());
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
    std::vector<std::uint8_t> pixels(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4);
    const auto blend = [&](int x, int y, float alpha) {
        if (x < 0 || y < 0 || x >= width || y >= height) {
            return;
        }
        std::uint8_t* pixel = pixels.data() + (static_cast<std::size_t>(y) * static_cast<std::size_t>(width) +
                                               static_cast<std::size_t>(x)) *
                                                  4;
        const float colour[3] = {53.0f, 230.0f, 163.0f}; // theme::kGround, as BGR.
        for (int c = 0; c < 3; ++c) {
            pixel[c] = static_cast<std::uint8_t>(
                std::lround(static_cast<float>(pixel[c]) + (colour[c] - static_cast<float>(pixel[c])) * alpha));
        }
    };
    const auto line = [&](float x0, float y0, float x1, float y1, float alpha) {
        const float length = std::hypot(x1 - x0, y1 - y0);
        if (length > 20000.0f) {
            return;
        }
        const int steps = std::max(1, static_cast<int>(length));
        for (int i = 0; i <= steps; ++i) {
            const float t = static_cast<float>(i) / static_cast<float>(steps);
            const int x = static_cast<int>(std::lround(x0 + (x1 - x0) * t));
            const int y = static_cast<int>(std::lround(y0 + (y1 - y0) * t));
            blend(x, y, alpha);
            blend(x + 1, y, alpha);
            blend(x, y + 1, alpha);
        }
    };
    const auto drawGrid = [&](const GroundCamera& camera) {
        constexpr float kCell = 1.5f;
        constexpr int kCells = 20;
        constexpr float kReach = kCell * static_cast<float>(kCells);
        const auto onScreen = [&](float mapX, float mapY, float& x, float& y) {
            float right = 0.0f;
            float away = 0.0f;
            mapToGround(camera, mapX, mapY, right, away);
            if (aheadOfCamera(camera, right, away, 0.0f) < 0.3f) {
                return false;
            }
            const GroundPoint point = groundToScreen(camera, right, away);
            x = point.x;
            y = point.y;
            return point.ahead;
        };
        const float firstX = (std::floor(camera.feetX / kCell) - static_cast<float>(kCells)) * kCell;
        const float firstY = (std::floor(camera.feetY / kCell) - static_cast<float>(kCells)) * kCell;
        for (int row = 0; row <= 2 * kCells; ++row) {
            for (int column = 0; column <= 2 * kCells; ++column) {
                const float x0 = firstX + static_cast<float>(column) * kCell;
                const float y0 = firstY + static_cast<float>(row) * kCell;
                const float closeness =
                    1.0f - std::hypot(x0 + kCell * 0.5f - camera.feetX, y0 + kCell * 0.5f - camera.feetY) / kReach;
                if (closeness <= 0.1f) {
                    continue;
                }
                float ax = 0.0f;
                float ay = 0.0f;
                float bx = 0.0f;
                float by = 0.0f;
                float cx = 0.0f;
                float cy = 0.0f;
                if (onScreen(x0, y0, ax, ay) && onScreen(x0 + kCell, y0, bx, by) && onScreen(x0, y0 + kCell, cx, cy)) {
                    const float alpha = 0.75f * closeness * closeness * (camera.pitchMeasured ? 1.0f : 0.5f);
                    line(ax, ay, bx, by, alpha);
                    line(ax, ay, cx, cy, alpha);
                }
            }
        }
        const float horizon = horizonRow(camera);
        if (horizon > 0.0f && horizon < static_cast<float>(height)) {
            line(0.0f, horizon, static_cast<float>(width), horizon, 0.6f);
        }
    };
    const std::vector<double>& times = reader.times();
    const unsigned every = std::max(options.frameEvery, 1u);
    ClipReader::Frame frame;
    int written = 0;
    while (reader.next(frame)) {
        const bool due = (frame.number - 1) % every == 0 || frame.number == times.size() ||
                         std::find(options.at.begin(), options.at.end(), frame.number) != options.at.end();
        if (!due) {
            continue;
        }
        gpu.context->CopySubresourceRegion(staging.Get(), 0, 0, 0, 0, frame.texture, frame.subresource, nullptr);
        D3D11_MAPPED_SUBRESOURCE mapped{};
        check(gpu.context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped), "Map (clip frame)");
        for (int y = 0; y < height; ++y) {
            std::memcpy(pixels.data() + static_cast<std::size_t>(y) * static_cast<std::size_t>(width) * 4,
                        static_cast<const std::uint8_t*>(mapped.pData) + static_cast<std::size_t>(mapped.RowPitch) * y,
                        static_cast<std::size_t>(width) * 4);
        }
        gpu.context->Unmap(staging.Get(), 0);
        const std::size_t index = static_cast<std::size_t>(frame.number) - 1;
        const double timeMs = index < clip.frameMs.size() ? clip.frameMs[index] : frame.seconds * 1000.0;
        if (const GroundCamera camera = groundAmong(cameras, timeMs); camera.known) {
            drawGrid(camera);
            std::printf("  frame %llu  %.3f ms  pitch %.6f  distance %.6f  yaw %.6f  feet %.6f %.6f\n",
                        static_cast<unsigned long long>(frame.number), timeMs, static_cast<double>(camera.pitch),
                        static_cast<double>(camera.distance), static_cast<double>(camera.yaw),
                        static_cast<double>(camera.feetX), static_cast<double>(camera.feetY));
        }
        char name[32];
        std::snprintf(name, sizeof(name), "ground-%06llu.jpg", static_cast<unsigned long long>(frame.number));
        writeImageFile(fs::path(options.out) / name, ImageFormat::Jpeg, pixels.data(), reader.width(),
                       reader.height(), reader.width() * 4);
        ++written;
    }
    std::printf("Wrote %d JPEGs into %s\n", written, options.out);
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
    if (options.clipGuesses != nullptr) {
        return clipGuesses(options);
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
    if (options.clipFrames != nullptr || options.clipSeek != nullptr || options.ui != nullptr ||
        options.ground != nullptr) {
        const bool com = SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED)); // Media Foundation needs it.
        int result = 1;
        try {
            result = options.ground != nullptr       ? groundClip(options)
                     : options.ui != nullptr         ? uiClip(options)
                     : options.clipFrames != nullptr ? clipFrames(options)
                                                     : clipSeek(options);
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
            const auto heuristics = std::make_shared<HeuristicsSource>(); // The ones built in.
            if (options.model != nullptr) {
                recognizer = std::make_unique<Recognizer>(screen, *pipeline.recognition(), options.model, heuristics,
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
            // The snapper, as the app runs it beside the recogniser.
            std::shared_ptr<BoxSnapper> snapper;
            if (options.snap && recognizer) {
                snapper = std::make_shared<BoxSnapper>(screen, *pipeline.snap(), heuristics);
                snapper->setClip(clip);
                recognizer->setSnapper(snapper);
                pipeline.setSnapEnabled(true);
            }
            // The UI finder, as the app runs it while observing.
            std::unique_ptr<UiFinder> uiFinder;
            if (options.findUi) {
                uiFinder = std::make_unique<UiFinder>(screen.adapter.Get(), *pipeline.ui(),
                                                      options.layout != nullptr ? std::filesystem::path(options.layout)
                                                                                : std::filesystem::path("models/ui"));
                pipeline.setUiEnabled(true);
            }
            bool uiFailed = false;
            bool uiProblem = false;
            bool snapperFailed = false;
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
            std::printf("  images/s  merged/s  latency avg / max ms  work avg / max ms%s%s%s%s\n",
                        recognizer ? "  recognised/s  recognise avg / max ms  found" : "",
                        snapper ? "  snapped/s  snap avg / max ms  late ms  snapped" : "",
                        clip ? "  encoded/s  encode avg / max ms  held s    MB  dropped" : "",
                        uiFinder ? "  ui/s  ui avg / max ms  finding ms  pieces" : "");

            using Clock = std::chrono::steady_clock;
            const Clock::time_point start = Clock::now();
            Clock::time_point nextPrint =
                start + std::chrono::milliseconds(1500); // Half a second after a stats update.
            std::uint64_t lastMerged = 0;
            double rateSum = 0.0;
            double latencySum = 0.0;
            double recognisedSum = 0.0;
            double recogniseMsSum = 0.0;
            double snappedSum = 0.0;
            double snapMsSum = 0.0;
            double snapLateSum = 0.0;
            double uiSum = 0.0;
            double uiMsSum = 0.0;
            double uiFindSum = 0.0;
            int samples = 0;
            while (Clock::now() - start < std::chrono::seconds(options.seconds) + std::chrono::milliseconds(600)) {
                if (consumer) {
                    consumer->poll();
                }
                DwmFlush(); // Pace like a UI synced to the screen.
                if (log && !marked && Clock::now() - start >= std::chrono::milliseconds(options.seconds * 500)) {
                    log->mark(1); // Halfway, as Ctrl+Alt+M did in the app until 2026-11-12.
                    pipeline.recordNow();
                    marked = true;
                }
                if (clip && !clipNoted && Clock::now() - start >= std::chrono::milliseconds(options.seconds * 500)) {
                    clip->note("halfway (observex_bench)");
                    clipNoted = true;
                }
                if (snapper && !snapperFailed && snapper->state() == BoxSnapper::State::Failed) {
                    std::printf("Snapper failed: %s\n", snapper->error().c_str());
                    snapperFailed = true;
                }
                if (clip && !clipFailed && clip->state() == ClipRecorder::State::Failed) {
                    std::printf("Clip buffer failed: %s\n", clip->error().c_str());
                    clipFailed = true;
                }
                if (uiFinder && !uiFailed && uiFinder->state() == UiFinder::State::Failed) {
                    std::printf("UI finder failed: %s\n", uiFinder->error().c_str());
                    uiFailed = true;
                }
                if (uiFinder && !uiProblem && uiFinder->stats().problem[0] != '\0') {
                    std::printf("UI finder: %s\n", uiFinder->stats().problem.data());
                    uiProblem = true;
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
                    if (snapper) {
                        const SnapperStats& snapped = snapper->stats();
                        std::printf(
                            "  %9.1f  %7.2f / %-7.2f  %7.2f  %6.1f%%", static_cast<double>(snapped.imagesPerSecond),
                            static_cast<double>(snapped.workAvgMs), static_cast<double>(snapped.workMaxMs),
                            static_cast<double>(snapped.lateAvgMs), static_cast<double>(snapped.snapped * 100.0f));
                        snappedSum += snapped.imagesPerSecond;
                        snapMsSum += snapped.workAvgMs;
                        snapLateSum += snapped.lateAvgMs;
                    }
                    if (clip) {
                        const ClipRecorderStats& held = clip->stats();
                        std::printf("  %9.1f  %8.2f / %-8.2f  %6.1f  %4.0f  %7llu",
                                    static_cast<double>(held.framesPerSecond), static_cast<double>(held.encodeAvgMs),
                                    static_cast<double>(held.encodeMaxMs), held.seconds,
                                    static_cast<double>(held.bytes) / (1024.0 * 1024.0),
                                    static_cast<unsigned long long>(held.dropped));
                    }
                    if (uiFinder) {
                        const UiFinderStats& ui = uiFinder->stats();
                        std::printf("  %4.1f  %6.2f / %-6.2f  %10.2f  %6d", static_cast<double>(ui.imagesPerSecond),
                                    static_cast<double>(ui.workAvgMs), static_cast<double>(ui.workMaxMs),
                                    static_cast<double>(ui.findAvgMs), uiFinder->found().count);
                        uiSum += ui.imagesPerSecond;
                        uiMsSum += ui.workAvgMs;
                        uiFindSum += ui.findAvgMs;
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
                if (snapper) {
                    std::printf(
                        "Snapping: %.1f images/s, %.2f ms per image, boxes there %.2f ms after the screen showed"
                        " the image\n",
                        snappedSum / samples, snapMsSum / samples, snapLateSum / samples);
                }
                if (uiFinder) {
                    std::printf("UI finding: %.1f images/s, %.2f ms per image (finding %.2f)\n", uiSum / samples,
                                uiMsSum / samples, uiFindSum / samples);
                }
            }
            if (clip) {
                saveClip(*clip, options.clipFolder, screen, recognizer.get(), options.model);
                if (recognizer) {
                    recognizer->setClip(nullptr);
                }
            }
            if (snapper) {
                recognizer->setSnapper(nullptr);
                pipeline.setSnapEnabled(false);
                snapper.reset();
            }
            if (uiFinder) {
                pipeline.setUiEnabled(false);
                uiFinder.reset();
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
