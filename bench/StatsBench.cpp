#include "StatsBench.h"

#include "heuristics/StatsReader.h"
#include "core/StateTree.h"
#include "core/UiLayout.h"
#include "engine/ImageFile.h"
#include "heuristics/Snapper.h"
#include "heuristics/TextOcr.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <string>
#include <vector>

namespace observex {

// --stats: on each image (PATH may be an image or a folder of them, e.g. a recording), finds the interface as the UI
// finder does (core/UiLayout on its grey image) and reads stats.txt's bars on it (core/GameStats), then says per bar
// how often it was seen and its number read, kept or doubted (read, but not agreeing with the bar), with examples, and
// how long reading took. --list prints every image's reads, and why what was not seen was not. --skills BRAIN: the skills
// to look for, a profile's (.brain, as the app gives them), or a folder of grey NAME.pgm pictures (all with cooldowns).
int statsBench(int argc, char** argv) {
    namespace fs = std::filesystem;
    using Clock = std::chrono::steady_clock;
    // --ocr PGM...: what Windows' OCR reads on each grey picture (binary PGM), for trying how the reader draws names.
    if (argc >= 1 && std::strcmp(argv[0], "--ocr") == 0) {
        TextOcr ocr;
        for (int i = 1; i < argc; ++i) {
            std::ifstream in(argv[i], std::ios::binary);
            std::string magic;
            int w = 0;
            int h = 0;
            int most = 0;
            in >> magic >> w >> h >> most;
            in.get();
            std::vector<std::uint8_t> grey(static_cast<std::size_t>(w) * static_cast<std::size_t>(h));
            in.read(reinterpret_cast<char*>(grey.data()), static_cast<std::streamsize>(grey.size()));
            std::string error;
            const std::string said = magic == "P5" && in ? ocr.read(grey.data(), w, h, w, &error) : "(not a P5 PGM)";
            std::printf("%s\t%s%s\n", fs::path(argv[i]).filename().string().c_str(), said.c_str(), error.c_str());
        }
        return 0;
    }
    std::vector<fs::path> paths;
    fs::path layoutFolder = "models/ui";
    bool list = false;
    bool listPieces = false; // --pieces: each image's found pieces (for scratch stress tests that move them).
    fs::path skillsFrom;
    for (int i = 0; i < argc; ++i) {
        if (std::strcmp(argv[i], "--layout") == 0 && i + 1 < argc) {
            layoutFolder = argv[++i];
        } else if (std::strcmp(argv[i], "--skills") == 0 && i + 1 < argc) {
            skillsFrom = argv[++i];
        } else if (std::strcmp(argv[i], "--list") == 0) {
            list = true;
        } else if (std::strcmp(argv[i], "--pieces") == 0) {
            listPieces = true;
        } else {
            paths.emplace_back(argv[i]);
        }
    }
    if (paths.empty()) {
        std::printf("usage: observex_bench --stats PATH... [--layout FOLDER] [--skills BRAIN|FOLDER] [--list]\n"
                    "  PATH: an image, or a folder of .jpg/.png (a recording)\n");
        return 2;
    }
    UiLayout layout;
    StatsReader reader;
    std::string error;
    if (!layout.load(layoutFolder, &error) || !reader.load(layoutFolder, &error)) {
        std::printf("%s\n", error.c_str());
        return 1;
    }
    // The skills, grey as the app gives them (app/App's skillsToFind).
    std::vector<std::string> skillNames;
    std::vector<std::vector<std::uint8_t>> greys;
    std::vector<SkillPicture> pictures;
    if (!skillsFrom.empty()) {
        std::vector<std::pair<SkillPicture, std::vector<std::uint8_t>>> found;
        if (fs::is_directory(skillsFrom)) {
            for (const fs::directory_entry& entry : fs::directory_iterator(skillsFrom)) {
                std::vector<std::uint8_t> pixels;
                int width = 0;
                int height = 0;
                if (entry.path().extension() == ".pgm" && readPgm(entry.path(), pixels, width, height) &&
                    width == height) {
                    SkillPicture picture;
                    std::snprintf(picture.name.data(), picture.name.size(), "%s",
                                  entry.path().stem().string().c_str());
                    picture.side = width;
                    found.emplace_back(picture, std::move(pixels));
                }
            }
        } else {
            std::ifstream in(skillsFrom, std::ios::binary);
            const std::string text{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
            const std::optional<statetree::Tree> tree = statetree::readTree(text, &error);
            if (!tree) {
                std::printf("%s: %s\n", skillsFrom.string().c_str(), error.c_str());
                return 1;
            }
            for (const statetree::Skill& skill : tree->skills()) {
                SkillPicture picture;
                std::snprintf(picture.name.data(), picture.name.size(), "%s", skill.name.c_str());
                picture.cooldown = skill.cooldown;
                picture.side = skill.side;
                std::vector<std::uint8_t> grey(skill.bgr.size() / 3);
                for (std::size_t p = 0; p < grey.size(); ++p) {
                    grey[p] = static_cast<std::uint8_t>(
                        (skill.bgr[p * 3] * 29 + skill.bgr[p * 3 + 1] * 150 + skill.bgr[p * 3 + 2] * 77) >> 8);
                }
                found.emplace_back(picture, std::move(grey));
            }
        }
        for (auto& [picture, grey] : found) {
            greys.push_back(std::move(grey));
            pictures.push_back(picture);
        }
        for (std::size_t i = 0; i < pictures.size(); ++i) {
            pictures[i].grey = greys[i].data();
        }
        reader.setSkills(pictures);
        std::printf("%d skills from %s\n", reader.skills(), skillsFrom.string().c_str());
    }

    std::vector<fs::path> images;
    for (const fs::path& path : paths) {
        if (fs::is_directory(path)) {
            std::vector<fs::path> inside;
            for (const fs::directory_entry& entry : fs::directory_iterator(path)) {
                const fs::path ext = entry.path().extension();
                if (ext == ".jpg" || ext == ".png") {
                    inside.push_back(entry.path());
                }
            }
            std::sort(inside.begin(), inside.end());
            images.insert(images.end(), inside.begin(), inside.end());
        } else {
            images.push_back(path);
        }
    }

    struct Tally {
        int seen = 0;
        int kept = 0;
        int doubted = 0;
        int unread = 0; // Seen, with a number expected, none read.
        int pieces = 0; // Its piece found.
        int hidden = 0; // Its piece found, how full it is not seen (a window over it).
        std::vector<std::string> doubts;
        std::vector<std::string> unreads;
        std::vector<std::string> hides;
    };
    std::map<std::string, Tally> tallies;
    std::vector<std::string> order;
    std::vector<double> readMs;
    std::vector<UiPiece> pieces(UiPieces::kMax);
    for (const fs::path& path : images) {
        ImageData picture;
        try {
            picture = readImageFile(path);
        } catch (const std::exception& e) {
            std::printf("%s: %s\n", path.string().c_str(), e.what());
            continue;
        }
        const int width = static_cast<int>(picture.width);
        const int height = static_cast<int>(picture.height);
        GreyImage grey;
        grey.fromBgra(picture.bgra.data(), width, height, static_cast<std::ptrdiff_t>(width) * 4);
        const int count = layout.find(grey.view().levels[0], pieces.data(), static_cast<int>(pieces.size()));
        const std::span<const UiPiece> on(pieces.data(), static_cast<std::size_t>(count));
        if (listPieces) {
            for (const UiPiece& piece : on) {
                std::printf("piece %s %s %d %d %d %d\n", path.filename().string().c_str(), piece.name.data(), piece.x0,
                            piece.y0, piece.x1, piece.y1);
            }
        }

        const Clock::time_point start = Clock::now();
        StatsArea areas[8];
        const int parts = reader.areas(on, width, height, areas, 8);
        std::vector<ColourPart> colour;
        for (int i = 0; i < parts; ++i) {
            colour.push_back({areas[i],
                              picture.bgra.data() + (static_cast<std::size_t>(areas[i].y0) * width + areas[i].x0) * 4,
                              static_cast<std::size_t>(width) * 4});
        }
        GameStats stats;
        reader.read(on, colour, stats);
        readMs.push_back(std::chrono::duration<double, std::milli>(Clock::now() - start).count());

        std::string line = path.filename().string() + ":";
        for (int i = 0; i < stats.count; ++i) {
            const GameGauge& gauge = stats.gauges[static_cast<std::size_t>(i)];
            const std::string name = std::string(gauge.piece.data()) + " " + gauge.name.data();
            if (!tallies.contains(name)) {
                order.push_back(name);
            }
            Tally& tally = tallies[name];
            const std::string full = gauge.full >= 0.0f ? std::to_string(gauge.full).substr(0, 6) : "unseen";
            const std::string where = path.parent_path().filename().string() + "/" + path.filename().string();
            char said[96];
            std::snprintf(said, sizeof(said), " %s %s %s%s%s", gauge.name.data(), full.c_str(),
                          gauge.pressed ? "pressed" : gauge.text[0] != '\0' ? gauge.text.data() : "-",
                          gauge.doubted[0] != '\0' ? " doubted " : "", gauge.doubted.data());
            line += said;
            if (std::any_of(on.begin(), on.end(),
                            [&](const UiPiece& p) { return std::strcmp(p.name.data(), gauge.piece.data()) == 0; })) {
                ++tally.pieces;
                if (gauge.full < 0.0f) {
                    ++tally.hidden;
                    if (tally.hides.size() < 4) {
                        tally.hides.push_back(where + ": bar hidden" +
                                              (gauge.doubted[0] != '\0' ? std::string(", read ") + gauge.doubted.data()
                                                                        : std::string()));
                    }
                }
            }
            if (!gauge.seen) {
                continue;
            }
            ++tally.seen;
            if (gauge.text[0] != '\0') {
                ++tally.kept;
            } else if (gauge.doubted[0] != '\0') {
                ++tally.doubted;
                if (tally.doubts.size() < 8 && gauge.full >= 0.0f) {
                    tally.doubts.push_back(where + ": read " + gauge.doubted.data() + ", bar " + full);
                }
            } else {
                ++tally.unread;
                if (tally.unreads.size() < 4) {
                    tally.unreads.push_back(where + ": nothing read, bar " + full);
                }
            }
        }
        if (list) {
            int area = 0;
            for (int i = 0; i < parts; ++i) {
                area += (areas[i].x1 - areas[i].x0) * (areas[i].y1 - areas[i].y0);
            }
            std::printf("%s | target %s '%s'%s%s | %d parts %d px %.3f ms%s%s\n", line.c_str(),
                        stats.hasTarget ? "yes" : "no", stats.targetName.data(),
                        stats.targetChanged ? " | Target Changed: " : "",
                        stats.targetChanged ? (stats.target[0] != '\0' ? stats.target.data() : "None") : "", parts,
                        area, readMs.back(), stats.why[0] != '\0' ? " | why: " : "", stats.why.data());
        }
    }

    std::printf("%zu images (%s)\n", images.size(), layoutFolder.string().c_str());
    for (const std::string& name : order) {
        const Tally& tally = tallies[name];
        // A bar no number was ever read on has none (e.g. the target's).
        const bool numbered = tally.kept + tally.doubted > 0;
        std::printf("  %-16s piece on %d, bar hidden on %d; seen on %d", name.c_str(), tally.pieces, tally.hidden,
                    tally.seen);
        if (numbered) {
            std::printf("; number kept %d, doubted %d, none read %d", tally.kept, tally.doubted, tally.unread);
        }
        std::printf("\n");
        for (const std::vector<std::string>* examples : {&tally.doubts, &tally.unreads, &tally.hides}) {
            if (examples == &tally.unreads && !numbered) {
                continue;
            }
            for (const std::string& example : *examples) {
                std::printf("      %s\n", example.c_str());
            }
        }
    }
    if (!readMs.empty()) {
        std::sort(readMs.begin(), readMs.end());
        double sum = 0.0;
        for (const double ms : readMs) {
            sum += ms;
        }
        std::printf("Reading: %.3f ms average, %.3f median, %.3f slowest (CPU, the parts already in memory)\n",
                    sum / static_cast<double>(readMs.size()), readMs[readMs.size() / 2], readMs.back());
    }
    return 0;
}

} // namespace observex
