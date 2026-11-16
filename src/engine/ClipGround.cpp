#include "engine/ClipGround.h"

#include "core/MouseCamera.h"
#include "engine/ClipPlayer.h"
#include "engine/DepthStore.h"
#include "engine/GreyMaker.h"
#include "engine/GroundFinder.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <exception>
#include <filesystem>
#include <unordered_map>
#include <utility>
#include <vector>

namespace observex {

ClipGround::ClipGround(IDXGIAdapter1* gpu, std::unique_ptr<Grounding> ground, std::filesystem::path video,
                       std::shared_ptr<const ClipText> clip, Guesses shown, std::filesystem::path uiFolder)
    : ground_(std::move(ground)), video_(std::move(video)), clip_(std::move(clip)), shown_(std::move(shown)),
      uiFolder_(std::move(uiFolder)) {
    thread_ = std::jthread([this, adapter = ComPtr<IDXGIAdapter1>(gpu)](std::stop_token stop) { run(stop, adapter); });
}

ClipGround::~ClipGround() {
    if (thread_.joinable()) {
        thread_.request_stop();
        thread_.join();
    }
}

const GroundHeights* heightsAmong(std::span<const ClipHeights> heights, double timeMs) {
    const auto after = std::upper_bound(heights.begin(), heights.end(), timeMs,
                                        [](double ms, const ClipHeights& line) { return ms < line.timeMs; });
    return after == heights.begin() ? nullptr : (after - 1)->heights.get();
}

std::vector<GroundCamera> ClipGround::follow(ID3D11Device* device, Grounding& follower,
                                             const std::filesystem::path& video, const ClipText& clip,
                                             const Guesses& shown, UiLayout* layout, std::stop_token stop,
                                             std::atomic<float>* progress, Stats* stats, bool notes,
                                             std::vector<ClipCameraLine>* mouseCameras,
                                             std::vector<ClipHeights>* heights) {
    follower.clear();
    // The terrain on each frame it changed on, as the ground finder hands it out live.
    if (heights != nullptr) {
        heights->clear();
    }
    std::uint64_t heightsVersion = ~std::uint64_t{0};
    auto spare = std::make_shared<GroundHeights>();
    const auto noteHeights = [&](double timeMs) {
        if (heights != nullptr && follower.heights(heightsVersion, *spare)) {
            heightsVersion = spare->version;
            heights->push_back({timeMs, spare->known ? std::shared_ptr<const GroundHeights>(std::move(spare)) : nullptr});
            spare = std::make_shared<GroundHeights>();
        }
    };
    ClipReader reader(device, video);
    GreyMaker greys(device, reader.width(), reader.height(), 1);
    ComPtr<ID3D11DeviceContext> context;
    device->GetImmediateContext(&context);
    const auto width = static_cast<int>(reader.width());
    const auto height = static_cast<int>(reader.height());
    const auto own = std::find(clip.kinds.begin(), clip.kinds.end(), "own");
    const int ownKind = own != clip.kinds.end() ? static_cast<int>(own - clip.kinds.begin()) : -1;

    // As live: the images it followed (one the video lacks by the frame after it), or on a clip saved before those
    // were kept kImagesPerSecond of the frames; the interface found again once a second (it moves only when the user
    // moves it; live the UI finder looks 10 times a second).
    const std::vector<unsigned long long> followed = followedImages(clip);
    std::size_t nextFollowed = 0;
    constexpr double kGapMs = 1000.0 / GroundFinder::kImagesPerSecond;
    constexpr double kUiGapMs = 1000.0;
    const GreyArea whole{0, 0, width, height};
    std::vector<UiPiece> pieces(UiPieces::kMax);
    int pieceCount = 0;
    std::vector<GreyArea> avoid;
    std::vector<GreyArea> windows;
    std::array<GreyArea, 8> full{};
    const int firstLevel = follower.firstLevel();
    std::vector<GroundCamera> cameras;
    Stats counted;
    double nextMs = -1e300;
    // The depth maps made live, in image order, each with its image's time; given the follower before the frame after
    // its image (none on a clip saved without them).
    std::vector<DepthRecord> depths;
    if (std::error_code error; std::filesystem::exists(video.parent_path() / kClipDepth, error)) {
        depths = readDepthFile(video.parent_path() / kClipDepth);
    }
    std::sort(depths.begin(), depths.end(), [](const DepthRecord& a, const DepthRecord& b) { return a.image < b.image; });
    std::unordered_map<unsigned long long, double> imageMs;
    for (std::size_t i = 0; i < clip.frameImage.size() && i < clip.frameMs.size(); ++i) {
        imageMs.try_emplace(clip.frameImage[i], clip.frameMs[i]);
    }
    std::size_t nextDepth = 0;
    DepthPacker packer;
    std::vector<std::uint16_t> unpacked;
    const auto giveDepths = [&](unsigned long long image) {
        for (; nextDepth < depths.size() && depths[nextDepth].image < image; ++nextDepth) {
            const DepthRecord& record = depths[nextDepth];
            const auto at = imageMs.find(record.image);
            if (at == imageMs.end() || !packer.unpackMap(record, unpacked)) {
                continue;
            }
            DepthInput input;
            input.map = unpacked.data();
            input.width = record.width;
            input.height = record.height;
            input.screenWidth = record.screenWidth;
            input.screenHeight = record.screenHeight;
            input.ignore = record.ignore.data();
            input.ignoreCount = static_cast<int>(record.ignore.size());
            const GroundDepthNote note = follower.depth(input, at->second);
            ++counted.depths;
            counted.depthsMeasured += note.measured ? 1 : 0;
            if (notes) {
                counted.depthNotes.push_back({at->second, note});
            }
        }
    };


    // The camera by the mouse's counts (clips since the evening of 2026-11-15), given the reports before each frame as
    // live; the game's window filled the screen recorded (its middle the clip's).
    MouseCamera::Settings mouseSettings;
    mouseSettings.keepMs = 1e12;
    MouseCamera mouse(mouseSettings);
    mouse.middle(width / 2, height / 2);
    std::size_t reported = 0;
    if (mouseCameras != nullptr) {
        mouseCameras->clear();
    }
    // The Camera card on the frame of `image` shown at `timeMs`, as live (core/MouseCamera's cameraOf).
    const auto noteCamera = [&](unsigned long long image, double timeMs) {
        if (mouseCameras != nullptr && !clip.mice.empty()) {
            mouseCameras->push_back({image, timeMs, cameraOf(mouse.at(timeMs), mouse.yawOff(), mouse.pitchOff())});
        }
    };

    double nextUiMs = -1e300;
    const std::size_t frames = reader.times().size();
    ClipReader::Frame frame;
    while (!stop.stop_requested() && reader.next(frame)) {
        if (progress != nullptr) {
            progress->store(static_cast<float>(frame.number) / static_cast<float>(frames), std::memory_order_relaxed);
        }
        const std::size_t index = static_cast<std::size_t>(frame.number) - 1;
        const double timeMs = index < clip.frameMs.size() ? clip.frameMs[index] : frame.seconds * 1000.0;
        if (index < clip.frameImage.size()) {
            giveDepths(clip.frameImage[index]);
        }
        for (; reported < clip.mice.size() && clip.mice[reported].timeMs <= timeMs; ++reported) {
            mouse.add(clip.mice[reported]);
        }
        const MouseCamera::View byCounts = mouse.at(timeMs);
        const unsigned long long frameImage = index < clip.frameImage.size() ? clip.frameImage[index] : 0;
        bool due = false;
        if (!followed.empty()) {
            const unsigned long long image = frameImage;
            while (nextFollowed < followed.size() && followed[nextFollowed] <= image) {
                ++nextFollowed;
                due = true;
            }
        } else if (timeMs >= nextMs - 2.0) { // A frame shown a moment before the one due stands for it (times jitter).
            due = true;
            nextMs = std::max(nextMs + kGapMs, timeMs);
        }
        if (!due) {
            // Between follows: only the eighth size and the minimap's cone, as live (engine/GroundFinder).
            const auto headStart = std::chrono::steady_clock::now();
            GroundHints hints;
            hints.windows = windows.data();
            hints.windowCount = static_cast<int>(windows.size());
            mouseHints(byCounts, hints);
            const int count = follower.headAreas(timeMs, hints, full.data(), static_cast<int>(full.size()));
            context->CopySubresourceRegion(greys.input(), 0, 0, 0, 0, frame.texture, frame.subresource, nullptr);
            greys.make(context.Get(), frame.number);
            if (count > 0) {
                greys.read(context.Get(), frame.number, {full.data(), static_cast<std::size_t>(count)}, 1);
            }
            greys.read(context.Get(), frame.number, {&whole, 1}, GreyView::kLevels, follower.headFirstLevel());
            greys.wait(context.Get());
            if (const GreyView* grey = greys.find(frame.number)) {
                ++counted.heads;
                counted.headsTurned += follower.head(*grey) ? 1 : 0;
                GroundCamera camera = follower.camera();
                camera.timeMs = timeMs;
                cameras.push_back(camera);
                noteHeights(timeMs);
            }
            counted.headMs +=
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - headStart).count();
            noteCamera(frameImage, timeMs);
            continue;
        }
        const auto start = std::chrono::steady_clock::now();
        context->CopySubresourceRegion(greys.input(), 0, 0, 0, 0, frame.texture, frame.subresource, nullptr);
        greys.make(context.Get(), frame.number);
        // When the interface is looked for, all the levels first (it reads the full size whole); else the levels
        // the follower reads once it says which parts of the full size it reads too (the minimap's cone).
        const bool findUi = layout != nullptr && timeMs >= nextUiMs;
        const GreyView* grey = nullptr;
        if (findUi) {
            greys.read(context.Get(), frame.number, {&whole, 1});
            greys.wait(context.Get());
            grey = greys.find(frame.number);
            if (grey != nullptr) {
                pieceCount = layout->find(grey->levels[0], pieces.data(), UiPieces::kMax);
                windows.clear();
                for (int i = 0; i < pieceCount; ++i) {
                    const UiPiece& piece = pieces[static_cast<std::size_t>(i)];
                    windows.push_back({piece.x0, piece.y0, piece.x1, piece.y1});
                }
            }
            nextUiMs = timeMs + kUiGapMs;
        }

        // What the guesser shows then: the own character, and the characters and the interface to leave out (a
        // little wider, as live).
        const std::span<const Tracked> things = shown.at(timeMs);
        avoid.clear();
        for (const Tracked& thing : things) {
            const float marginX = thing.width * 0.1f;
            const float marginY = thing.height * 0.1f;
            avoid.push_back({static_cast<int>(std::floor(thing.x - marginX)),
                             static_cast<int>(std::floor(thing.y - marginY)),
                             static_cast<int>(std::ceil(thing.x + thing.width + marginX)),
                             static_cast<int>(std::ceil(thing.y + thing.height + marginY))});
        }
        avoid.insert(avoid.end(), windows.begin(), windows.end()); // The interface's pieces.

        GroundHints hints;
        if (const Tracked* ownThing = ownCharacter(things, ownKind, static_cast<float>(width))) {
            hints.hasOwn = true;
            hints.ownTop = ownThing->y;
            hints.ownBottom = ownThing->y + ownThing->height;
        }
        hints.avoid = avoid.data();
        hints.avoidCount = static_cast<int>(avoid.size());
        hints.windows = windows.data();
        hints.windowCount = static_cast<int>(windows.size());
        mouseHints(byCounts, hints);

        follower.begin(timeMs, width, height, hints);
        if (!findUi) {
            if (const int count = follower.fullAreas(full.data(), static_cast<int>(full.size())); count > 0) {
                greys.read(context.Get(), frame.number, {full.data(), static_cast<std::size_t>(count)}, 1);
            }
            greys.read(context.Get(), frame.number, {&whole, 1}, GreyView::kLevels, firstLevel);
            greys.wait(context.Get());
            grey = greys.find(frame.number);
        }
        const bool fitted = grey != nullptr && follower.end(*grey);
        GroundCamera camera = follower.camera();
        camera.timeMs = timeMs;
        cameras.push_back(camera);
        noteHeights(timeMs);

        const GroundNote note = follower.note();
        anchorMouse(mouse, timeMs, note, camera);
        noteCamera(frameImage, timeMs);
        if (notes) {
            counted.frames.push_back({timeMs, note.learned, note.found, note.moved, note.motion, note.agreeing,
                                      note.searchTurn, note.searchMatch, note.headed ? note.heading : -1.0f,
                                      camera.yaw, camera.pitch, camera.distance});
            counted.frames.back().note = note;
        }
        counted.turnsFitted += note.turnFitted ? 1 : 0;
        counted.turnsTaken += note.turnTaken ? 1 : 0;
        counted.turnsHeld += note.turnHolds ? 1 : 0;
        counted.turnMs += note.turnMs;
        ++counted.images;
        counted.headed += note.headed ? 1 : 0;
        if (note.followed) {
            ++counted.motions[static_cast<std::size_t>(note.motion)];
        }
        counted.matches += note.found;
        if (fitted) {
            ++counted.fitted;
            counted.agreeing += note.agreeing;
            counted.missPixels += note.miss;
        }
        counted.workMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    }
    if (stats != nullptr) {
        *stats = counted;
        if (counted.images > 0) {
            stats->matches /= counted.images;
            stats->workMs /= counted.images;
            stats->turnMs /= counted.images;
        }
        if (counted.heads > 0) {
            stats->headMs /= counted.heads;
        }
        if (counted.fitted > 0) {
            stats->agreeing /= counted.fitted;
            stats->missPixels /= counted.fitted;
        }
    }
    return cameras;
}

void ClipGround::run(std::stop_token stop, ComPtr<IDXGIAdapter1> gpu) {
    SetThreadDescription(GetCurrentThread(), L"observex clip ground");
    // Below the UI and the player: it only has to be done in a few seconds.
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    const bool com = SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED)); // Media Foundation needs it.
    try {
        UiLayout layout;
        layout.load(uiFolder_, nullptr); // Only framed windows if it cannot be read, as live.
        const GpuDevice device = createGpuDevice(gpu.Get(), true);
        cameras_ = follow(device.device.Get(), *ground_, video_, *clip_, shown_, &layout, stop, &progress_, nullptr,
                          false, &mouseCameras_, &heights_);
        state_.store(State::Done, std::memory_order_release);
    } catch (const std::exception& e) {
        error_ = e.what();
        state_.store(State::Failed, std::memory_order_release);
    }
    if (com) {
        CoUninitialize();
    }
}

} // namespace observex
