// observex_bench --turns CLIP: the camera's turn and tilt between video frames of a saved clip, by the points the
// screen shows (heuristics/TurnPitch, the pitch by the camera's turns), against the mouse's counts (the user asked,
// 2026-11-15 evening: "figure out the proper math as to how much the camera is moving ingame based on my mouse
// stride delta"). Each frame's points are looked for on the frame --step later where the counts held between the two
// (the right button down, LAG ms before each frame's time) say the camera went, at kCountRadians a count: so a fast
// turn is followed as far as the blur lets the points be found. Prints a line a pair: what the counts say and what
// the points say. See bench/Bench.cpp's header for the options.
#include "TurnsBench.h"

#include "core/ClipGuesses.h"
#include "core/ClipLines.h"
#include "core/Ground.h"
#include "core/MouseCamera.h"
#include "core/Recordings.h"
#include "core/UiLayout.h"
#include "engine/ClipPlayer.h"
#include "engine/GreyMaker.h"
#include "engine/Gpu.h"
#include "heuristics/TurnPitch.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <vector>

namespace observex {

namespace {

namespace fs = std::filesystem;

constexpr double kDegrees = 57.29577951308232;

} // namespace

int turnsBench(int argc, char** argv) {
    if (argc < 1) {
        std::printf("usage: observex_bench --turns CLIP [--lag MS] [--spread MS] [--top UNITS] [--bottom UNITS] "
                    "[--focal PX] [--anchor] [--layout FOLDER] [--step N]\n");
        return 2;
    }
    fs::path folder(argv[0]);
    MouseCamera::Settings settings;
    settings.keepMs = 1e12; // The whole clip.
    float focal = kGroundFocal;
    bool anchor = false;
    const char* layoutFolder = "models/ui";
    int step = 1;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--lag") == 0 && i + 1 < argc) {
            settings.lagMs = std::atof(argv[++i]);
        } else if (std::strcmp(argv[i], "--spread") == 0 && i + 1 < argc) {
            settings.spreadMs = std::atof(argv[++i]);
        } else if (std::strcmp(argv[i], "--top") == 0 && i + 1 < argc) {
            settings.topUnits = std::atoi(argv[++i]);
        } else if (std::strcmp(argv[i], "--bottom") == 0 && i + 1 < argc) {
            settings.bottomUnits = std::atoi(argv[++i]);
        } else if (std::strcmp(argv[i], "--focal") == 0 && i + 1 < argc) {
            focal = static_cast<float>(std::atof(argv[++i]));
        } else if (std::strcmp(argv[i], "--anchor") == 0) {
            anchor = true;
        } else if (std::strcmp(argv[i], "--layout") == 0 && i + 1 < argc) {
            layoutFolder = argv[++i];
        } else if (std::strcmp(argv[i], "--step") == 0 && i + 1 < argc) {
            step = std::max(1, std::atoi(argv[++i]));
        } else {
            std::printf("Unknown option %s\n", argv[i]);
            return 2;
        }
    }
    std::ifstream file(folder / kClipText, std::ios::binary);
    if (!file) {
        std::printf("No clip.txt in %s\n", folder.string().c_str());
        return 1;
    }
    const std::string text{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
    const ClipText clip = readClipText(text);
    if (clip.mice.empty()) {
        std::printf("The clip has no mouse lines (saved before the evening of 2026-11-15).\n");
        return 1;
    }
    const Guesses shown = recordedGuesses(clip);
    UiLayout layout;
    std::string error;
    if (!layout.load(layoutFolder, &error)) {
        std::printf("Only framed windows are left out: %s\n", error.c_str());
    }

    const ComPtr<IDXGIAdapter1> adapter = findPrimaryGpu();
    const GpuDevice gpu = createGpuDevice(adapter.Get(), true);
    ClipReader reader(gpu.device.Get(), folder / kClipVideo);
    GreyMaker greys(gpu.device.Get(), reader.width(), reader.height(), step + 1);
    ComPtr<ID3D11DeviceContext> context;
    gpu.device->GetImmediateContext(&context);
    const auto width = static_cast<int>(reader.width());
    const auto height = static_cast<int>(reader.height());
    const GreyArea whole{0, 0, width, height};
    // The counts as the game took them (core/MouseCamera): its camera mode, the spread and lag, the limits once the
    // pitch is known (--anchor: by the turns' pitch; else never, the counts summed as they come), given as they came.
    MouseCamera mouse(settings);
    mouse.middle(width / 2, height / 2);
    std::size_t reported = 0; // The reports given (as live: those before each frame).

    std::printf("# frame TIME COUNTSX COUNTSY turn PREDICTED tilt PREDICTED | fitted POINTS AGREEING turn DEG tilt DEG "
                "pitch DEG spread DEG miss PX | mouse TURNUNITS PITCHUNITS pitch DEG (lag %.1f ms, spread %.1f ms, step "
                "%d, focal %.0f, %.5f deg a count)\n",
                settings.lagMs, settings.spreadMs, step, static_cast<double>(focal),
                MouseCamera::kCountRadians * kDegrees);
    std::deque<TurnPitch> learnedOn; // learnedOn[k]: points learned on the frame k + 1 back (step of them).
    std::deque<double> learnedMs;
    std::deque<float> learnedPitch;
    std::vector<GreyArea> windows;
    std::vector<GreyArea> avoid;
    std::vector<UiPiece> pieces(UiPieces::kMax);
    double nextUiMs = -1e300;
    float pitch = 0.3f;
    bool pitchKnown = false;
    ClipReader::Frame frame;
    int fitted = 0;
    int pairs = 0;
    while (reader.next(frame)) {
        const std::size_t index = static_cast<std::size_t>(frame.number) - 1;
        const double timeMs = index < clip.frameMs.size() ? clip.frameMs[index] : frame.seconds * 1000.0;
        context->CopySubresourceRegion(greys.input(), 0, 0, 0, 0, frame.texture, frame.subresource, nullptr);
        greys.make(context.Get(), frame.number);
        const bool findUi = timeMs >= nextUiMs;
        if (findUi) {
            greys.read(context.Get(), frame.number, {&whole, 1});
            nextUiMs = timeMs + 1000.0;
        } else {
            greys.read(context.Get(), frame.number, {&whole, 1}, GreyView::kLevels, TurnPitch::kLevel);
        }
        greys.wait(context.Get());
        const GreyView* grey = greys.find(frame.number);
        if (grey == nullptr) {
            continue;
        }
        if (findUi) {
            const int count = layout.find(grey->levels[0], pieces.data(), UiPieces::kMax);
            windows.clear();
            for (int i = 0; i < count; ++i) {
                const UiPiece& piece = pieces[static_cast<std::size_t>(i)];
                windows.push_back({piece.x0, piece.y0, piece.x1, piece.y1});
            }
        }
        const GroundCamera recorded = recordedGround(clip.grounds, timeMs);
        GroundCamera camera = recorded;
        if (!camera.known) {
            camera.known = true;
            camera.middleX = static_cast<float>(width) * 0.5f;
            camera.middleY = static_cast<float>(height) * 0.5f;
            camera.distance = 10.0f;
        }
        camera.focal = focal;
        if (!pitchKnown) {
            pitch = camera.pitch;
            pitchKnown = true;
        }
        for (; reported < clip.mice.size() && clip.mice[reported].timeMs <= timeMs; ++reported) {
            mouse.add(clip.mice[reported]);
        }
        if (anchor && recorded.known) {
            mouse.guessPitch(timeMs, recorded.pitch); // As live: the follower's pitch until something better.
        }
        const MouseCamera::View now = mouse.at(timeMs);

        // The pair from the frame `step` back (learned then) to this one.
        if (static_cast<int>(learnedOn.size()) == step) {
            TurnPitch& turns = learnedOn.front();
            const MouseCamera::View then = mouse.at(learnedMs.front());
            const long long dx = (now.turnUnits - then.turnUnits) / MouseCamera::kUnitsPerCount;
            const long long dy = (now.pitchUnits - then.pitchUnits) / MouseCamera::kUnitsPerCount;
            const auto turn = static_cast<float>(static_cast<double>(now.turnUnits - then.turnUnits) *
                                                 MouseCamera::kUnitRadians);
            const auto tilt = static_cast<float>((now.pitchUnits - then.pitchUnits) * MouseCamera::kUnitRadians);
            camera.pitch = learnedPitch.front();
            const TurnPitch::Pair pair = turns.follow(*grey, camera, learnedPitch.front(), turn, tilt);
            ++pairs;
            fitted += pair.fitted ? 1 : 0;
            if (anchor && pair.fitted && pair.agreeing >= 100 && pair.spread < 0.5f / kDegrees &&
                std::fabs(pair.turn) >= 0.6f / kDegrees) { // Only a turn says the pitch.
                mouse.pitched(timeMs, pair.pitch + pair.tilt);
            }
            const MouseCamera::View said = mouse.at(timeMs);
            std::printf("frame %5llu %9.3f %5lld %5lld turn %8.3f tilt %8.3f | %d %3d %3d turn %8.3f tilt %8.3f pitch "
                        "%7.2f spread %6.2f miss %5.2f | mouse %7lld %6d pitch %7.2f%s\n",
                        static_cast<unsigned long long>(frame.number), timeMs, dx, dy, turn * kDegrees,
                        tilt * kDegrees, pair.fitted ? 1 : 0, pair.points, pair.agreeing, pair.turn * kDegrees,
                        pair.tilt * kDegrees, pair.pitch * kDegrees, pair.spread * kDegrees,
                        static_cast<double>(pair.miss), said.turnUnits, said.pitchUnits, said.pitch * kDegrees,
                        said.pitchPinned ? " pinned" : "");
            // The pitch carried on: the counts' once they are known, else what the pair said if it turned enough to
            // say it surely, else its tilt (or the counts' when the points were lost).
            if (said.pitchKnown) {
                pitch = said.pitch;
            } else if (step == 1) {
                if (pair.fitted && pair.agreeing >= 100 && pair.spread < 0.5f / kDegrees) {
                    pitch = pair.pitch + pair.tilt;
                } else {
                    pitch += pair.fitted && pair.agreeing >= 100 ? pair.tilt : tilt;
                }
            }
            learnedOn.pop_front();
            learnedMs.pop_front();
            learnedPitch.pop_front();
        }
        if (step > 1 && !learnedMs.empty()) {
            // Carry the pitch on by the counts between frames (the pairs are a step long).
            pitch += static_cast<float>((now.pitchUnits - mouse.at(learnedMs.back()).pitchUnits) *
                                        MouseCamera::kUnitRadians);
        }
        pitch = std::clamp(pitch, -0.6f, 1.55f);

        // Learn on this frame, the characters and the interface left out.
        avoid.assign(windows.begin(), windows.end());
        for (const Tracked& thing : shown.at(timeMs)) {
            const float marginX = thing.width * 0.1f;
            const float marginY = thing.height * 0.1f;
            avoid.push_back({static_cast<int>(std::floor(thing.x - marginX)),
                             static_cast<int>(std::floor(thing.y - marginY)),
                             static_cast<int>(std::ceil(thing.x + thing.width + marginX)),
                             static_cast<int>(std::ceil(thing.y + thing.height + marginY))});
        }
        learnedOn.emplace_back();
        learnedOn.back().learn(*grey, avoid);
        learnedMs.push_back(timeMs);
        learnedPitch.push_back(pitch);
    }
    std::printf("# %d pairs, %d fitted\n", pairs, fitted);
    return 0;
}

} // namespace observex
