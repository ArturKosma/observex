// observex_bench --distances CLIP: the distances the live view writes on characters and drops (the user asked,
// 2026-11-16: "check how stable and accurate that is over some clips from earlier"). The camera is followed over the
// clip as the clip screen's Today does (engine/ClipGround, today's ground following), and on every video frame each
// thing the app showed live (Recorded: clip.txt's shown and snapped lines) and each drop it drew (drops lines) gets its
// distance (core/Distances) on the flat ground and, where the follow shaped the terrain from the clip's depth maps,
// on it. Prints how many got one, how steady they are (a frame's distance against the middle of the frames either
// side, one frame and 0.1 s away: a thing walking steadily changes nothing there), and against the geodata's
// distances where the clip folder has them (geodata-distances.csv, the check of 2026-11-15 on clip
// 2026-11-15_14-24-16). --out FILE writes every distance as CSV.
#include "DistanceBench.h"

#include "core/ClipGuesses.h"
#include "core/Distances.h"
#include "core/Ground.h"
#include "core/Recordings.h"
#include "core/UiLayout.h"
#include "engine/ClipGround.h"
#include "engine/Gpu.h"
#include "engine/HeuristicsLibrary.h"
#include "heuristics/GroundMotion.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace observex {

namespace {

namespace fs = std::filesystem;

constexpr float kNone = -1.0f; // No distance.

// Distance bands, in the game's units.
struct Band {
    const char* name;
    float upTo;
};
constexpr Band kBands[] = {{"   0-300", 300.0f},   {" 300-600", 600.0f},   {" 600-1000", 1000.0f},
                           {"1000-2000", 2000.0f}, {"2000-4000", 4000.0f}, {"4000+", 1e30f}};
constexpr int kBandCount = static_cast<int>(std::size(kBands));

int bandOf(float units) {
    for (int i = 0; i < kBandCount; ++i) {
        if (units < kBands[i].upTo) {
            return i;
        }
    }
    return kBandCount - 1;
}

// The `share`-th of `values` (sorts them); 0 for none.
float quantile(std::vector<float>& values, float share) {
    if (values.empty()) {
        return 0.0f;
    }
    std::sort(values.begin(), values.end());
    const auto at = static_cast<std::size_t>(share * static_cast<float>(values.size() - 1) + 0.5f);
    return values[std::min(at, values.size() - 1)];
}

// One thing's distance on one video frame.
struct Sample {
    int frame = 0; // 0-based video frame.
    double ms = 0.0;
    unsigned id = 0;
    std::string kind;
    bool held = false;
    float footX = 0.0f;
    float footY = 0.0f;
    float tall = 0.0f;
    float flat = kNone;    // Game units, on the flat ground,
    float terrain = kNone; // on the terrain (kNone: none shaped yet).
    float flatSize = kNone;    // How tall it is by each distance (game units): its box's height at that depth.
    float terrainSize = kNone;
};

// How steady a series is: each frame's distance against the middle of the frames `lag` either side, by band.
struct Steadiness {
    std::vector<float> off[kBandCount];      // Game units,
    std::vector<float> offShare[kBandCount]; // of the distance.
    void add(float before, float now, float after) {
        const float wobble = std::fabs(now - 0.5f * (before + after));
        const int band = bandOf(now);
        off[band].push_back(wobble);
        offShare[band].push_back(wobble / std::max(now, 1.0f));
    }
};

// Each series (per thing, in frame order) into `steady` at lags 1 and kLongLag frames.
constexpr int kLongLag = 12; // 0.1 s at 120 frames a second.
void steadiness(const std::map<unsigned, std::vector<std::pair<int, float>>>& series, Steadiness& oneFrame,
                Steadiness& longer) {
    for (const auto& [id, values] : series) {
        std::map<int, float> byFrame(values.begin(), values.end());
        for (const auto& [frame, value] : byFrame) {
            for (const auto& [lag, steady] : {std::pair<int, Steadiness*>{1, &oneFrame}, {kLongLag, &longer}}) {
                const auto before = byFrame.find(frame - lag);
                const auto after = byFrame.find(frame + lag);
                if (before != byFrame.end() && after != byFrame.end()) {
                    steady->add(before->second, value, after->second);
                }
            }
        }
    }
}

void printSteadiness(const char* what, Steadiness& oneFrame, Steadiness& longer) {
    std::printf("  %s, how steady (off the middle of the frames either side; median / 90%%):\n", what);
    for (int band = 0; band < kBandCount; ++band) {
        if (oneFrame.off[band].empty() && longer.off[band].empty()) {
            continue;
        }
        std::printf("    %-9s  frame to frame %6zu: %5.1f / %5.1f units (%4.1f%% / %4.1f%%)   over 0.1 s %6zu: %5.1f / "
                    "%5.1f units (%4.1f%% / %4.1f%%)\n",
                    kBands[band].name, oneFrame.off[band].size(), quantile(oneFrame.off[band], 0.5f),
                    quantile(oneFrame.off[band], 0.9f), 100.0f * quantile(oneFrame.offShare[band], 0.5f),
                    100.0f * quantile(oneFrame.offShare[band], 0.9f), longer.off[band].size(),
                    quantile(longer.off[band], 0.5f), quantile(longer.off[band], 0.9f),
                    100.0f * quantile(longer.offShare[band], 0.5f), 100.0f * quantile(longer.offShare[band], 0.9f));
    }
}

// The geodata's distances (geodata-distances.csv of the 2026-11-15 check): by 0-based video frame and thing id.
std::map<std::pair<int, unsigned>, float> readTruth(const fs::path& file) {
    std::map<std::pair<int, unsigned>, float> truth;
    std::ifstream in(file);
    std::string line;
    std::getline(in, line); // The header.
    while (std::getline(in, line)) {
        std::stringstream fields(line);
        std::vector<std::string> cells;
        for (std::string cell; std::getline(fields, cell, ',');) {
            cells.push_back(cell);
        }
        // frame, seconds, id, kind, box_bottom_x, box_bottom_y, world_x, world_y, world_z, distance, ...,
        // held_box (13), under_ui_or_edge (14).
        if (cells.size() < 15 || cells[13] != "0" || cells[14] != "0") {
            continue; // Held through a gap or under the interface: its box is not where it stands.
        }
        truth[{std::atoi(cells[0].c_str()), static_cast<unsigned>(std::strtoul(cells[2].c_str(), nullptr, 10))}] =
            std::strtof(cells[9].c_str(), nullptr);
    }
    return truth;
}

void printTruth(const char* what, const std::vector<Sample>& samples,
                const std::map<std::pair<int, unsigned>, float>& truth, bool terrain) {
    std::vector<float> off[kBandCount];
    std::vector<float> offAbs[kBandCount];
    std::vector<float> share[kBandCount];
    int missing = 0;
    for (const Sample& sample : samples) {
        const auto found = truth.find({sample.frame, sample.id});
        if (found == truth.end()) {
            continue;
        }
        const float ours = terrain ? sample.terrain : sample.flat;
        if (ours < 0.0f) {
            ++missing;
            continue;
        }
        const int band = bandOf(found->second);
        off[band].push_back(ours - found->second);
        offAbs[band].push_back(std::fabs(ours - found->second));
        share[band].push_back(std::fabs(ours - found->second) / found->second);
    }
    std::printf("  %s against the geodata (by its distance; ours - geodata: median, and how far off: median / 90%%; "
                "%d without one):\n",
                what, missing);
    for (int band = 0; band < kBandCount; ++band) {
        if (off[band].empty()) {
            continue;
        }
        std::printf("    %-9s %5zu: %+6.0f units   off %5.0f / %5.0f units (%4.1f%% / %4.1f%%)\n", kBands[band].name,
                    off[band].size(), quantile(off[band], 0.5f), quantile(offAbs[band], 0.5f),
                    quantile(offAbs[band], 0.9f), 100.0f * quantile(share[band], 0.5f),
                    100.0f * quantile(share[band], 0.9f));
    }
}

// How tall each character is by its distances (Sample::flatSize, terrainSize), for those seen over a range of
// distances: a thing's size is the same at every distance, so with right distances it does not grow or shrink with
// them. Per thing seen on kLeastFrames frames whose farthest is kLeastRange times its nearest: how its size goes with
// its distance (the slope of log size on log distance: 0 right, + too far the farther it is, - too near) and how far
// its sizes are off their own line (median, of the size).
void printSizes(const char* what, const std::vector<Sample>& samples, bool terrain) {
    constexpr std::size_t kLeastFrames = 60;
    constexpr float kLeastRange = 1.25f;
    std::map<unsigned, std::vector<std::pair<float, float>>> byThing; // log distance, log size.
    std::map<unsigned, std::string> kinds;
    for (const Sample& sample : samples) {
        const float distance = terrain ? sample.terrain : sample.flat;
        const float size = terrain ? sample.terrainSize : sample.flatSize;
        if (!sample.held && distance > 0.0f && size > 0.0f) {
            byThing[sample.id].emplace_back(std::log(distance), std::log(size));
            kinds[sample.id] = sample.kind;
        }
    }
    std::vector<float> slopes;
    std::vector<float> spreads;
    std::printf("  %s, sizes by the distances (things seen over a range of distances):\n", what);
    for (const auto& [id, points] : byThing) {
        if (points.size() < kLeastFrames) {
            continue;
        }
        float nearest = 1e30f;
        float farthest = -1e30f;
        double meanX = 0.0;
        double meanY = 0.0;
        for (const auto& [x, y] : points) {
            nearest = std::min(nearest, x);
            farthest = std::max(farthest, x);
            meanX += x;
            meanY += y;
        }
        if (farthest - nearest < std::log(kLeastRange)) {
            continue;
        }
        meanX /= static_cast<double>(points.size());
        meanY /= static_cast<double>(points.size());
        double xx = 0.0;
        double xy = 0.0;
        for (const auto& [x, y] : points) {
            xx += (x - meanX) * (x - meanX);
            xy += (x - meanX) * (y - meanY);
        }
        const auto slope = static_cast<float>(xy / xx);
        std::vector<float> off;
        for (const auto& [x, y] : points) {
            off.push_back(std::fabs(std::exp(y - static_cast<float>(meanY) - slope * (x - static_cast<float>(meanX))) -
                                    1.0f));
        }
        const float spread = quantile(off, 0.5f);
        slopes.push_back(slope);
        spreads.push_back(spread);
        std::printf("    %-8s %4u: %4zu frames, %5.0f to %5.0f units, %3.0f units tall at the middle, slope %+.2f, off "
                    "its line %4.1f%%\n",
                    kinds[id].c_str(), id, points.size(), std::exp(nearest), std::exp(farthest),
                    std::exp(static_cast<float>(meanY)), slope, 100.0f * spread);
    }
    if (slopes.empty()) {
        std::printf("    none seen over a range\n");
        return;
    }
    std::printf("    %zu things: slope median %+.2f (25%% %+.2f, 75%% %+.2f), off their lines %.1f%% (median)\n",
                slopes.size(), quantile(slopes, 0.5f), quantile(slopes, 0.25f), quantile(slopes, 0.75f),
                100.0f * quantile(spreads, 0.5f));
}

} // namespace

int distanceBench(int argc, char** argv) {
    if (argc < 1) {
        std::printf("usage: observex_bench --distances CLIP [--heuristics DLL] [--layout FOLDER] [--out FILE]\n");
        return 2;
    }
    fs::path folder(argv[0]);
    fs::path layoutFolder = "models/ui";
    const char* heuristics = nullptr;
    const char* out = nullptr;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--layout") == 0 && i + 1 < argc) {
            layoutFolder = argv[++i];
        } else if (std::strcmp(argv[i], "--heuristics") == 0 && i + 1 < argc) {
            heuristics = argv[++i];
        } else if (std::strcmp(argv[i], "--out") == 0 && i + 1 < argc) {
            out = argv[++i];
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
    const Guesses shown = recordedGuesses(clip);
    UiLayout layout;
    if (std::string error; !layout.load(layoutFolder, &error)) {
        std::printf("Only framed windows are left out: %s\n", error.c_str());
    }
    std::unique_ptr<Grounding> ground;
    if (heuristics != nullptr) {
        ground = HeuristicsLibrary::load(fs::absolute(heuristics))->makeGround();
    } else {
        ground = std::make_unique<GroundFollower>();
    }
    const ComPtr<IDXGIAdapter1> adapter = findPrimaryGpu();
    const GpuDevice gpu = createGpuDevice(adapter.Get(), true); // Video support, for the GPU's video decoder.
    const auto start = std::chrono::steady_clock::now();
    std::vector<ClipHeights> terrains;
    const std::vector<GroundCamera> cameras = ClipGround::follow(gpu.device.Get(), *ground, video, clip, shown, &layout,
                                                                 {}, nullptr, nullptr, false, nullptr, &terrains);
    std::printf("%s: camera followed on %zu frames in %.0f ms, the terrain changed on %zu (depth.bin %s)\n",
                toUtf8(video.c_str()).c_str(), cameras.size(),
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count(),
                terrains.size(), fs::exists(folder / "depth.bin") ? "there" : "none");

    const auto own = std::find(clip.kinds.begin(), clip.kinds.end(), "own");
    const int ownKind = own != clip.kinds.end() ? static_cast<int>(own - clip.kinds.begin()) : -1;
    std::vector<Sample> samples;
    std::vector<Sample> drops;
    int framesKnown = 0;
    float pitchLeast = 1e9f;
    float pitchMost = -1e9f;
    for (std::size_t n = 0; n < clip.frameMs.size(); ++n) {
        const double ms = clip.frameMs[n];
        const GroundCamera camera = groundAmong(cameras, ms);
        if (camera.known) {
            ++framesKnown;
            pitchLeast = std::min(pitchLeast, camera.pitch);
            pitchMost = std::max(pitchMost, camera.pitch);
        }
        const GroundHeights* heights = heightsAmong(terrains, ms); // The terrain (heuristics/DepthTerrain).
        for (const Tracked& thing : shown.at(ms)) {
            if (static_cast<int>(thing.kind) == ownKind && ownKind >= 0) {
                continue; // The own character: no distance from itself.
            }
            Sample sample;
            sample.frame = static_cast<int>(n);
            sample.ms = ms - clip.frameMs.front();
            sample.id = thing.id;
            sample.kind = thing.kind < clip.kinds.size() ? clip.kinds[thing.kind] : "character";
            sample.held = thing.held;
            characterFoot(thing, sample.footX, sample.footY);
            sample.tall = thing.height;
            // Also how tall it is by that distance (its box's height at how far ahead of the eye its feet are): the
            // same thing's size stays the same at any distance only if the distances are right.
            const auto sizeAt = [&](const GroundSpot& spot) {
                const float ahead = aheadOfCamera(camera, spot.right, spot.away, spot.up);
                return ahead > 0.0f ? thing.height * ahead / camera.focal * kGameUnits : kNone;
            };
            // As characterDistance (none farther than kFarthestDistance), keeping the spot for the size.
            const std::optional<GroundSpot> flat = groundUnder(camera, nullptr, sample.footX, sample.footY);
            if (flat && alongGround(*flat) <= kFarthestDistance) {
                sample.flat = alongGround(*flat) * kGameUnits;
                sample.flatSize = sizeAt(*flat);
            }
            if (heights != nullptr && heights->known) {
                const std::optional<GroundSpot> onTerrain = groundUnder(camera, heights, sample.footX, sample.footY);
                if (onTerrain && alongGround(*onTerrain) <= kFarthestDistance) {
                    sample.terrain = alongGround(*onTerrain) * kGameUnits;
                    sample.terrainSize = sizeAt(*onTerrain);
                }
            }
            samples.push_back(std::move(sample));
        }
        if (const ClipDropsLine* line = recordedDrops(clip.drops, ms)) {
            for (const Detection& drop : line->drops) {
                Sample sample;
                sample.frame = static_cast<int>(n);
                sample.ms = ms - clip.frameMs.front();
                sample.kind = "drop";
                dropFoot(drop, sample.footX, sample.footY);
                sample.tall = drop.height;
                if (const std::optional<float> flat = dropDistance(camera, nullptr, drop)) {
                    sample.flat = *flat * kGameUnits;
                }
                if (heights != nullptr && heights->known) {
                    if (const std::optional<float> onTerrain = dropDistance(camera, heights, drop)) {
                        sample.terrain = *onTerrain * kGameUnits;
                    }
                }
                drops.push_back(std::move(sample));
            }
        }
    }
    // Drops have no ids: one on the next frame within 30 pixels of one is the same.
    unsigned nextDrop = 1;
    for (std::size_t i = 0; i < drops.size(); ++i) {
        if (drops[i].id == 0) {
            drops[i].id = nextDrop++;
        }
        for (std::size_t j = i + 1; j < drops.size() && drops[j].frame <= drops[i].frame + 1; ++j) {
            if (drops[j].frame == drops[i].frame + 1 && drops[j].id == 0 &&
                std::hypot(drops[j].footX - drops[i].footX, drops[j].footY - drops[i].footY) < 30.0f) {
                drops[j].id = drops[i].id;
            }
        }
    }

    constexpr float kDegrees = 57.29578f;
    std::printf("  %zu frames, the camera known on %d (pitch %.1f to %.1f degrees)\n", clip.frameMs.size(),
                framesKnown, framesKnown > 0 ? pitchLeast * kDegrees : 0.0f,
                framesKnown > 0 ? pitchMost * kDegrees : 0.0f);
    for (const auto& [what, list] : {std::pair<const char*, const std::vector<Sample>*>{"Characters", &samples},
                                     {"Drops", &drops}}) {
        if (list->empty()) {
            std::printf("  %s: none shown\n", what);
            continue;
        }
        int flat = 0;
        int terrain = 0;
        std::map<unsigned, std::vector<std::pair<int, float>>> flatSeries;
        std::map<unsigned, std::vector<std::pair<int, float>>> terrainSeries;
        std::vector<float> all;
        for (const Sample& sample : *list) {
            if (sample.flat >= 0.0f && !sample.held) {
                ++flat;
                flatSeries[sample.id].emplace_back(sample.frame, sample.flat);
                all.push_back(sample.flat);
            }
            if (sample.terrain >= 0.0f && !sample.held) {
                ++terrain;
                terrainSeries[sample.id].emplace_back(sample.frame, sample.terrain);
            }
        }
        std::printf("  %s: %zu shown on frames (%d held through a gap), a distance on the flat ground for %d, on the "
                    "terrain for %d; distances %.0f to %.0f units (median %.0f)\n",
                    what, list->size(),
                    static_cast<int>(std::count_if(list->begin(), list->end(), [](const Sample& s) { return s.held; })),
                    flat, terrain, quantile(all, 0.0f), quantile(all, 1.0f), quantile(all, 0.5f));
        Steadiness oneFrame;
        Steadiness longer;
        steadiness(flatSeries, oneFrame, longer);
        printSteadiness("flat ground", oneFrame, longer);
        if (terrain > 0) {
            Steadiness terrainOne;
            Steadiness terrainLonger;
            steadiness(terrainSeries, terrainOne, terrainLonger);
            printSteadiness("terrain", terrainOne, terrainLonger);
        }
    }
    if (const fs::path truthFile = folder / "geodata-distances.csv"; fs::exists(truthFile)) {
        const auto truth = readTruth(truthFile);
        std::printf("  geodata-distances.csv: %zu distances\n", truth.size());
        printTruth("Flat ground", samples, truth, false);
        printTruth("Terrain", samples, truth, true);
    }
    printSizes("Flat ground", samples, false);
    printSizes("Terrain", samples, true);
    if (out != nullptr) {
        std::ofstream csv(out);
        csv << "frame,ms,id,kind,held,foot_x,foot_y,box_height,flat,terrain,flat_size,terrain_size\n";
        for (const auto* list : {&samples, &drops}) {
            for (const Sample& s : *list) {
                char line[256];
                std::snprintf(line, sizeof(line), "%d,%.2f,%u,%s,%d,%.1f,%.1f,%.1f,%.1f,%.1f,%.1f,%.1f\n", s.frame, s.ms,
                              s.id, s.kind.c_str(), s.held ? 1 : 0, static_cast<double>(s.footX),
                              static_cast<double>(s.footY), static_cast<double>(s.tall),
                              static_cast<double>(s.flat), static_cast<double>(s.terrain),
                              static_cast<double>(s.flatSize), static_cast<double>(s.terrainSize));
                csv << line;
            }
        }
        std::printf("  Wrote %s\n", out);
    }
    return 0;
}

} // namespace observex
