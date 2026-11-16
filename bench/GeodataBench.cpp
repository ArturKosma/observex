// observex_bench --geodata CLIP: the Show flag Geodata over a saved clip (the user asked, 2026-11-15): the minimap
// followed (heuristics/MinimapScroll) and the /loc answers read from the Messages window (heuristics/LocationReader) on
// ~10 frames a second, as the stats reader does live, where that puts the character (core/WorldPosition, checked
// against the geodata's floor as app/GeodataView does), and with --out frames with the geodata's grid drawn as the flag
// draws it, on the camera the ground finder follows over the clip (engine/ClipGround), its pitch set right on the
// geodata (core/Geodata's geoCamera; --all prints both). See bench/Bench.cpp's header for the options.
#include "GeodataBench.h"

#include "core/ClipGuesses.h"
#include "core/ClipLines.h"
#include "core/Geodata.h"
#include "core/Ground.h"
#include "core/Recordings.h"
#include "core/UiLayout.h"
#include "core/WorldPosition.h"
#include "engine/ClipGround.h"
#include "engine/ClipPlayer.h"
#include "engine/Gpu.h"
#include "engine/ImageFile.h"
#include "engine/ScreenCapture.h"
#include "heuristics/GroundMotion.h"
#include "heuristics/LocationReader.h"
#include "heuristics/MessageLines.h"
#include "heuristics/MinimapScroll.h"
#include "heuristics/PickupReader.h"
#include "heuristics/Snapper.h"
#include "heuristics/TextOcr.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
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
using Clock = std::chrono::steady_clock;

constexpr double kSampleMs = 100.0; // The UI finder's stats reading live: 10 a second.

const char* doubtName(WorldPosition::Doubt doubt) {
    switch (doubt) {
    case WorldPosition::Doubt::None:
        return "sure";
    case WorldPosition::Doubt::NoLocation:
        return "no /loc yet";
    case WorldPosition::Doubt::Moved:
        return "moved, no scale";
    case WorldPosition::Doubt::MinimapLost:
        return "minimap lost";
    case WorldPosition::Doubt::MinimapHidden:
        return "minimap hidden";
    }
    return "?";
}

const UiPiece* pieceNamed(const std::vector<UiPiece>& pieces, int count, const char* name) {
    for (int i = 0; i < count; ++i) {
        if (std::strcmp(pieces[static_cast<std::size_t>(i)].name.data(), name) == 0) {
            return &pieces[static_cast<std::size_t>(i)];
        }
    }
    return nullptr;
}

// A BGRA image to draw the grid on, as app/GeodataView draws it.
struct Canvas {
    std::uint8_t* pixels = nullptr;
    int width = 0;
    int height = 0;

    void blend(int x, int y, const float bgr[3], float alpha) const {
        if (x < 0 || y < 0 || x >= width || y >= height || alpha <= 0.0f) {
            return;
        }
        std::uint8_t* p = pixels + (static_cast<std::size_t>(y) * static_cast<std::size_t>(width) +
                                    static_cast<std::size_t>(x)) *
                                       4;
        for (int c = 0; c < 3; ++c) {
            p[c] = static_cast<std::uint8_t>(
                std::lround(static_cast<float>(p[c]) + (bgr[c] - static_cast<float>(p[c])) * std::min(alpha, 1.0f)));
        }
    }
    void line(float x0, float y0, float x1, float y1, const float bgr[3], float alpha, bool thick) const {
        const float length = std::hypot(x1 - x0, y1 - y0);
        if (length > 20000.0f) {
            return;
        }
        const int steps = std::max(1, static_cast<int>(length));
        for (int i = 0; i <= steps; ++i) {
            const float t = static_cast<float>(i) / static_cast<float>(steps);
            const int x = static_cast<int>(std::lround(x0 + (x1 - x0) * t));
            const int y = static_cast<int>(std::lround(y0 + (y1 - y0) * t));
            blend(x, y, bgr, alpha);
            if (thick) {
                blend(x + 1, y, bgr, alpha);
                blend(x, y + 1, bgr, alpha);
            }
        }
    }
    void quad(const GeoGrid::Cell& cell, const float bgr[3], float alpha) const {
        const float minX = std::min({cell.x[0], cell.x[1], cell.x[2], cell.x[3]});
        const float maxX = std::max({cell.x[0], cell.x[1], cell.x[2], cell.x[3]});
        const float minY = std::min({cell.y[0], cell.y[1], cell.y[2], cell.y[3]});
        const float maxY = std::max({cell.y[0], cell.y[1], cell.y[2], cell.y[3]});
        if (maxX - minX > 400.0f || maxY - minY > 400.0f) {
            return;
        }
        for (int y = static_cast<int>(std::floor(minY)); y <= static_cast<int>(std::ceil(maxY)); ++y) {
            for (int x = static_cast<int>(std::floor(minX)); x <= static_cast<int>(std::ceil(maxX)); ++x) {
                // Inside: on one side of all four edges.
                int positive = 0;
                int negative = 0;
                for (int k = 0; k < 4; ++k) {
                    const int n = (k + 1) % 4;
                    const float cross = (cell.x[n] - cell.x[k]) * (static_cast<float>(y) - cell.y[k]) -
                                        (cell.y[n] - cell.y[k]) * (static_cast<float>(x) - cell.x[k]);
                    positive += cross >= 0.0f ? 1 : 0;
                    negative += cross <= 0.0f ? 1 : 0;
                }
                if (positive == 4 || negative == 4) {
                    blend(x, y, bgr, alpha);
                }
            }
        }
    }
};

// app/GeodataView's colour by height against the feet, as BGR.
void heightColour(float rise, float out[3]) {
    constexpr float kLevel[3] = {53.0f, 230.0f, 163.0f}; // theme::kGround.
    constexpr float kLow[3] = {255.0f, 200.0f, 40.0f};   // theme::kGeoLow.
    constexpr float kHigh[3] = {30.0f, 140.0f, 255.0f};  // theme::kGeoHigh.
    const float t = std::clamp(rise / 120.0f, -1.0f, 1.0f);
    const float* to = t < 0.0f ? kLow : kHigh;
    for (int c = 0; c < 3; ++c) {
        out[c] = kLevel[c] + (to[c] - kLevel[c]) * std::fabs(t);
    }
}

} // namespace

int geodataBench(int argc, char** argv) {
    if (argc < 1) {
        std::printf("usage: observex_bench --geodata CLIP [--layout FOLDER] [--geo FOLDER] [--scale UNITS] "
                    "[--first-lines] [--all] [--out FOLDER] [--every N]\n");
        return 2;
    }
    fs::path folder(argv[0]);
    fs::path layoutFolder = "models/ui";
    fs::path geoFolder = "models/geodata";
    float scale = 0.0f;
    bool firstLines = false;
    bool all = false; // --all: a line for every image read.
    const char* out = nullptr;
    int every = 10;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--layout") == 0 && i + 1 < argc) {
            layoutFolder = argv[++i];
        } else if (std::strcmp(argv[i], "--geo") == 0 && i + 1 < argc) {
            geoFolder = argv[++i];
        } else if (std::strcmp(argv[i], "--scale") == 0 && i + 1 < argc) {
            scale = std::strtof(argv[++i], nullptr);
        } else if (std::strcmp(argv[i], "--first-lines") == 0) {
            firstLines = true;
        } else if (std::strcmp(argv[i], "--all") == 0) {
            all = true;
        } else if (std::strcmp(argv[i], "--out") == 0 && i + 1 < argc) {
            out = argv[++i];
        } else if (std::strcmp(argv[i], "--every") == 0 && i + 1 < argc) {
            every = std::max(1, std::atoi(argv[++i]));
        } else {
            std::printf("Unknown option %s\n", argv[i]);
            return 2;
        }
    }
    fs::path video = folder;
    if (fs::is_directory(folder)) {
        video /= kClipVideo;
    } else {
        folder = folder.parent_path();
    }
    std::ifstream file(folder / kClipText, std::ios::binary);
    if (!file) {
        std::printf("No %s in %s\n", kClipText, toUtf8(folder.c_str()).c_str());
        return 1;
    }
    const std::string text{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
    const ClipText clip = readClipText(text);
    UiLayout layout;
    std::string error;
    if (!layout.load(layoutFolder, &error)) {
        std::printf("%s\n", error.c_str());
        return 1;
    }

    try {
        const ComPtr<IDXGIAdapter1> adapter = findPrimaryGpu();
        const GpuDevice gpu = createGpuDevice(adapter.Get(), true); // Video support, for the GPU's video decoder.
        // The camera, as the ground finder follows it over the clip.
        GroundFollower follower;
        UiLayout groundLayout;
        groundLayout.load(layoutFolder, nullptr);
        const Clock::time_point followStart = Clock::now();
        const std::vector<GroundCamera> cameras =
            ClipGround::follow(gpu.device.Get(), follower, video, clip, recordedGuesses(clip), &groundLayout);
        std::printf("Camera followed in %.0f ms (%zu frames)\n",
                    std::chrono::duration<double, std::milli>(Clock::now() - followStart).count(), cameras.size());

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
        std::vector<std::uint8_t> screen(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4);
        const std::size_t pitch = static_cast<std::size_t>(width) * 4;
        if (out != nullptr) {
            fs::create_directories(out);
        }

        GreyImage grey;
        std::vector<UiPiece> pieces(UiPieces::kMax);
        MinimapScroll minimap;
        MessageLines messages;
        LocationReader location;
        PickupReader pickups;
        if (firstLines) {
            messages.takeFirstLines();
        }
        TextOcr ocr;
        LocationRead located{};
        PickupRead picked{};
        std::uint32_t lastCount = 0;
        std::uint32_t lastPickups = 0;
        WorldPosition position;
        if (scale > 0.0f) {
            position.setScale(scale);
        }
        Geodata geodata(geoFolder);
        std::shared_ptr<const GeoField> field;
        GeoGrid grid;
        WorldPosition::Doubt shownDoubt = WorldPosition::Doubt::NoLocation;


        double nextMs = -1e300;
        double firstMs = -1.0;
        int samples = 0;
        int seen = 0;
        int sure = 0;
        int written = 0;
        double minimapMs = 0.0;
        double locationMs = 0.0;
        ClipReader::Frame frame;
        while (reader.next(frame)) {
            const std::size_t index = static_cast<std::size_t>(frame.number) - 1;
            const double timeMs = index < clip.frameMs.size() ? clip.frameMs[index] : frame.seconds * 1000.0;
            if (timeMs < nextMs) {
                continue;
            }
            nextMs = timeMs + kSampleMs;
            if (firstMs < 0.0) {
                firstMs = timeMs;
            }
            gpu.context->CopySubresourceRegion(staging.Get(), 0, 0, 0, 0, frame.texture, frame.subresource, nullptr);
            D3D11_MAPPED_SUBRESOURCE mapped{};
            check(gpu.context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped), "Map (clip frame)");
            for (int y = 0; y < height; ++y) {
                std::memcpy(screen.data() + static_cast<std::size_t>(y) * pitch,
                            static_cast<const std::uint8_t*>(mapped.pData) +
                                static_cast<std::size_t>(mapped.RowPitch) * static_cast<std::size_t>(y),
                            pitch);
            }
            gpu.context->Unmap(staging.Get(), 0);
            grey.fromBgra(screen.data(), width, height, static_cast<std::ptrdiff_t>(pitch));
            const GreyView view = grey.view(frame.number);
            const int count = layout.find(view.levels[0], pieces.data(), static_cast<int>(pieces.size()));
            const auto partOf = [&](const StatsArea& area) {
                return ColourPart{area, screen.data() + static_cast<std::size_t>(area.y0) * pitch +
                                            static_cast<std::size_t>(area.x0) * 4,
                                  pitch};
            };

            // The stats reader's two: the minimap, then the messages.
            const UiPiece* mapPiece = pieceNamed(pieces, count, kMinimapPiece);
            const UiPiece* messagesPiece = pieceNamed(pieces, count, kMessagesPiece);
            Clock::time_point start = Clock::now();
            MinimapRead map{};
            if (mapPiece != nullptr) {
                const ColourPart part = partOf(MinimapScroll::area(*mapPiece, width, height));
                map = minimap.follow(mapPiece, &part);
            } else {
                map = minimap.follow(nullptr, nullptr);
            }
            minimapMs += std::chrono::duration<double, std::milli>(Clock::now() - start).count();
            start = Clock::now();
            std::string why;
            ColourPart messagesPart{};
            if (messagesPiece != nullptr) {
                messagesPart = partOf(MessageLines::area(*messagesPiece, width, height));
            }
            messages.look(messagesPiece != nullptr ? &messagesPart : nullptr);
            location.read(messages, map, ocr, located, why);
            pickups.read(messages, ocr, picked, why);
            const double readMs = std::chrono::duration<double, std::milli>(Clock::now() - start).count();
            locationMs += readMs;
            if (all && readMs > 1.0) {
                std::printf("%8.0f ms  messages read in %.1f ms\n", timeMs - firstMs, readMs);
            }
            // Items and adena picked up, as the Log says them.
            for (; lastPickups < picked.count; ++lastPickups) {
                const Pickup& pickup = picked.items[lastPickups % PickupRead::kMax];
                std::printf("%8.0f ms  Picked up %s\n", timeMs - firstMs, pickupText(pickup).c_str());
            }
            if (all) {
                for (const MessageLines::Line& line : messages.lines()) {
                    if (line.fresh) {
                        std::printf("%8.0f ms  new %s line at rows %d-%d\n", timeMs - firstMs,
                                    line.white ? "white" : "yellow", line.top, line.bottom);
                    }
                }
            }
            ++samples;
            seen += map.seen ? 1 : 0;

            // Where that puts the character, as app/GeodataView reads it.
            const WorldPosition::Doubt before = position.spot().doubt;
            position.minimap(map);
            if (located.count != 0 && located.count != lastCount) {
                lastCount = located.count;
                const std::optional<GeoFloor> floor = geodata.floorNear(located.x, located.y, located.z);
                if (floor && std::abs(floor->z - located.z) > 100) {
                    std::printf("%8.0f ms  Location read as %d, %d, %d, but the floor there is at %d: not taken\n",
                                timeMs - firstMs, located.x, located.y, located.z, floor->z);
                } else {
                    std::printf("%8.0f ms  %s  [read: %s]%s\n", timeMs - firstMs, position.located(located).c_str(),
                                located.text.data(),
                                floor ? "" : (" (no geodata: " + Geodata::regionName(located.x, located.y) + ")").c_str());
                }
            }
            const WorldPosition::Spot& spot = position.spot();
            if (spot.known && position.sure()) {
                if (const std::optional<GeoFloor> floor = geodata.floorNear(spot.x, spot.y, spot.z)) {
                    position.settle(floor->z);
                }
            }
            if (!why.empty()) {
                std::printf("%8.0f ms  %s\n", timeMs - firstMs, why.c_str());
            }
            if (all && mapPiece != nullptr) {
                std::printf("%8.0f ms  minimap window %d %d %d %d\n", timeMs - firstMs, mapPiece->x0, mapPiece->y0,
                            mapPiece->x1, mapPiece->y1);
            }
            if (all && mapPiece == nullptr) {
                std::printf("%8.0f ms  no minimap: %s; pieces:", timeMs - firstMs, layout.whyNot(kMinimapPiece).c_str());
                for (int i = 0; i < count; ++i) {
                    const UiPiece& piece = pieces[static_cast<std::size_t>(i)];
                    std::printf(" %s %d %d %d %d,", piece.name.data(), piece.x0, piece.y0, piece.x1, piece.y1);
                }
                std::printf("\n");
            }
            if (all || spot.doubt != shownDoubt || samples % 10 == 1) {
                std::printf("%8.0f ms  minimap %s run %u slid %7.1f %7.1f alike %.2f (%d key points)  %s  at %.0f %.0f "
                            "%.0f\n",
                            timeMs - firstMs, mapPiece == nullptr ? "no-piece" : map.seen ? "seen" : "unseen",
                            map.run, static_cast<double>(map.x), static_cast<double>(map.y),
                            static_cast<double>(minimap.lastMatch()), minimap.points(), doubtName(spot.doubt), spot.x,
                            spot.y, spot.z);
                shownDoubt = spot.doubt;
            }
            (void)before;
            sure += position.sure() ? 1 : 0;

            // The grid, as the flag draws it.
            const GroundCamera camera = groundAmong(cameras, timeMs);
            if (out == nullptr || (samples - 1) % every != 0) {
                continue;
            }
            if (spot.known && (!field || !field->covers(spot.x, spot.y, 4200.0))) {
                const Clock::time_point buildStart = Clock::now();
                field = GeoField::build(geodata, spot.x, spot.y, spot.z);
                std::printf("%8.0f ms  geodata field built round %.0f %.0f in %.0f ms (%d cells without geodata)\n",
                            timeMs - firstMs, spot.x, spot.y,
                            std::chrono::duration<double, std::milli>(Clock::now() - buildStart).count(),
                            field->missing());
            }
            if (spot.known && field && camera.known) {
                const double feet[3] = {spot.x, spot.y, spot.z};
                const GroundCamera fixed =
                    geoCamera(*field, camera, feet, static_cast<float>(width), static_cast<float>(height));
                if (all) {
                    std::printf("%8.0f ms  pitch %.2f deg, on the geodata %.2f\n", timeMs - firstMs,
                                static_cast<double>(camera.pitch) * 57.29578,
                                static_cast<double>(fixed.pitch) * 57.29578);
                }
                geoGrid(*field, fixed, feet, static_cast<float>(width), static_cast<float>(height),
                        {pieces.data(), static_cast<std::size_t>(count)}, grid);
                const Canvas canvas{screen.data(), width, height};
                const float strength = position.sure() ? 1.0f : 0.4f;
                constexpr float kBlocked[3] = {40.0f, 40.0f, 220.0f};
                for (const GeoGrid::Cell& cell : grid.blocked) {
                    canvas.quad(cell, kBlocked, 0.35f * cell.strength * strength);
                }
                for (const GeoGrid::Line& line : grid.lines) {
                    float colour[3];
                    heightColour(line.rise, colour);
                    canvas.line(line.x0, line.y0, line.x1, line.y1, colour,
                                (line.fine ? 0.8f : 0.6f) * line.strength * strength, line.fine);
                }
                constexpr float kOwn[3] = {200.0f, 112.0f, 240.0f};
                for (std::size_t k = 0; k + 3 < grid.feet.size(); k += 2) {
                    canvas.line(grid.feet[k], grid.feet[k + 1], grid.feet[k + 2], grid.feet[k + 3], kOwn, strength,
                                true);
                }
            }
            char name[40];
            std::snprintf(name, sizeof(name), "geodata-%06llu.jpg", static_cast<unsigned long long>(frame.number));
            writeImageFile(fs::path(out) / name, ImageFormat::Jpeg, screen.data(), reader.width(), reader.height(),
                           reader.width() * 4);
            ++written;
        }
        std::printf("%d images read: minimap followed on %d, sure where the character is on %d; %.2f ms a minimap, "
                    "%.2f ms a messages read on average\n",
                    samples, seen, sure, samples > 0 ? minimapMs / samples : 0.0,
                    samples > 0 ? locationMs / samples : 0.0);
        if (position.scale() > 0.0f) {
            std::printf("Minimap scale from /loc: %.2f units a pixel\n", static_cast<double>(position.scale()));
        }
        if (out != nullptr) {
            std::printf("Wrote %d JPEGs into %s\n", written, out);
        }
    } catch (const std::exception& e) {
        std::printf("%s\n", e.what());
        return 1;
    }
    return 0;
}

int pickupsBench(int argc, char** argv) {
    if (argc < 1) {
        std::printf("usage: observex_bench --pickups IMAGE... [--layout FOLDER]\n");
        return 2;
    }
    fs::path layoutFolder = "models/ui";
    std::vector<fs::path> images;
    for (int i = 0; i < argc; ++i) {
        if (std::strcmp(argv[i], "--layout") == 0 && i + 1 < argc) {
            layoutFolder = argv[++i];
        } else {
            images.emplace_back(argv[i]);
        }
    }
    UiLayout layout;
    std::string error;
    if (!layout.load(layoutFolder, &error)) {
        std::printf("%s\n", error.c_str());
        return 1;
    }
    TextOcr ocr;
    GreyImage grey;
    std::vector<UiPiece> pieces(UiPieces::kMax);
    int found = 0;
    for (const fs::path& path : images) {
        ImageData image;
        try {
            image = readImageFile(path);
        } catch (const std::exception& e) {
            std::printf("%s: %s\n", toUtf8(path.c_str()).c_str(), e.what());
            continue;
        }
        const int width = static_cast<int>(image.width);
        const int height = static_cast<int>(image.height);
        const std::size_t pitch = static_cast<std::size_t>(width) * 4;
        grey.fromBgra(image.bgra.data(), width, height, static_cast<std::ptrdiff_t>(pitch));
        const GreyView view = grey.view(1);
        const int count = layout.find(view.levels[0], pieces.data(), static_cast<int>(pieces.size()));
        const UiPiece* messagesPiece = pieceNamed(pieces, count, kMessagesPiece);
        std::printf("%s\n", toUtf8(path.filename().c_str()).c_str());
        if (messagesPiece == nullptr) {
            std::printf("  no Messages window\n");
            continue;
        }
        const StatsArea area = MessageLines::area(*messagesPiece, width, height);
        const ColourPart part{area, image.bgra.data() + static_cast<std::size_t>(area.y0) * pitch +
                                        static_cast<std::size_t>(area.x0) * 4,
                              pitch};
        MessageLines lines;
        lines.takeFirstLines(); // Every line shown is read.
        lines.look(&part);
        PickupReader reader;
        PickupRead picked{};
        std::string why;
        const Clock::time_point start = Clock::now();
        reader.read(lines, ocr, picked, why);
        const double ms = std::chrono::duration<double, std::milli>(Clock::now() - start).count();
        for (std::uint32_t i = 0; i < picked.count && i < static_cast<std::uint32_t>(PickupRead::kMax); ++i) {
            std::printf("  Picked up %s\n", pickupText(picked.items[i]).c_str());
            ++found;
        }
        std::printf("  (%zu lines, %.1f ms) %s\n", lines.lines().size(), ms, why.c_str());
    }
    std::printf("%d pickups in %zu images\n", found, images.size());
    return 0;
}

} // namespace observex
