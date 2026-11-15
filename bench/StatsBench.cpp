#include "StatsBench.h"

#include "heuristics/KeyLabels.h"
#include "heuristics/StatsReader.h"
#include "core/StateTree.h"
#include "core/UiLayout.h"
#include "engine/ImageFile.h"
#include "heuristics/Snapper.h"
#include "heuristics/TextOcr.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <string>
#include <vector>

namespace observex {

namespace {

// --stats --keys [--layout FOLDER] read|teach ...: the keys written on hotbar slots (heuristics/KeyLabels, by FOLDER's
// keys.txt, models/ui by default), as the stats reader reads them and as the Skills tab teaches them:
//   read BRAIN                 every skill's picture in a profile (.brain)
//   read IMAGE X Y SIDE        the slot at X Y on a screen image
//   teach BRAIN SKILL TEXT     learns that the key on SKILL's picture is TEXT, and writes keys.txt
//   teach IMAGE X Y SIDE TEXT
int keysBench(int argc, char** argv) {
    namespace fs = std::filesystem;
    fs::path folder = "models/ui";
    int at = 0;
    if (argc >= 2 && std::strcmp(argv[0], "--layout") == 0) {
        folder = argv[1];
        at = 2;
    }
    const auto usage = [] {
        std::printf("usage: observex_bench --stats --keys [--layout FOLDER] read BRAIN | read IMAGE X Y SIDE |\n"
                    "         teach BRAIN SKILL TEXT | teach IMAGE X Y SIDE TEXT\n");
        return 2;
    };
    if (argc - at < 2 || (std::strcmp(argv[at], "read") != 0 && std::strcmp(argv[at], "teach") != 0)) {
        return usage();
    }
    const bool teach = std::strcmp(argv[at], "teach") == 0;
    const fs::path source = argv[at + 1];
    int next = at + 2;
    KeyFont font;
    std::string error;
    if (!font.load(folder / kKeysFile, &error)) {
        std::printf("%s\n", error.c_str());
        return 1;
    }
    // The pictures: a profile's skills (3 bytes a pixel), or one slot of an image (4).
    struct Slot {
        std::string name;
        std::vector<std::uint8_t> pixels;
        int side = 0;
        int step = 3;
    };
    std::vector<Slot> slots;
    if (source.extension() == statetree::kTreeExtension) {
        std::ifstream in(source, std::ios::binary);
        const std::string text{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
        const std::optional<statetree::Tree> tree = statetree::readTree(text, &error);
        if (!tree) {
            std::printf("%s: %s\n", source.string().c_str(), error.c_str());
            return 1;
        }
        const char* only = teach && next < argc ? argv[next++] : nullptr;
        for (const statetree::Skill& skill : tree->skills()) {
            if (skill.side > 0 && (only == nullptr || skill.name == only)) {
                slots.push_back({skill.name + " (profile: " + (skill.key.empty() ? "no key" : skill.key) + ")",
                                 skill.bgr, skill.side, 3});
            }
        }
        if (teach && slots.size() != 1) {
            std::printf("no skill %s in %s\n", only != nullptr ? only : "(none given)", source.string().c_str());
            return 1;
        }
    } else {
        if (argc - next < 3) {
            return usage();
        }
        const int x = std::atoi(argv[next]);
        const int y = std::atoi(argv[next + 1]);
        const int side = std::atoi(argv[next + 2]);
        next += 3;
        ImageData image;
        try {
            image = readImageFile(source);
        } catch (const std::exception& e) {
            std::printf("%s: %s\n", source.string().c_str(), e.what());
            return 1;
        }
        if (side < 8 || side > 64 || x < 0 || y < 0 || x + side > static_cast<int>(image.width) ||
            y + side > static_cast<int>(image.height)) {
            std::printf("the slot is not on the image\n");
            return 1;
        }
        Slot slot{source.filename().string() + " " + std::to_string(x) + "," + std::to_string(y), {}, side, 4};
        for (int row = 0; row < side; ++row) {
            const std::uint8_t* from = image.bgra.data() + (static_cast<std::size_t>(y + row) * image.width + x) * 4;
            slot.pixels.insert(slot.pixels.end(), from, from + static_cast<std::size_t>(side) * 4);
        }
        slots.push_back(std::move(slot));
    }
    const auto pictureOf = [](const Slot& slot) {
        return KeyPicture{slot.pixels.data(), static_cast<std::size_t>(slot.side * slot.step), slot.step, slot.side};
    };
    const auto say = [&](const Slot& slot) {
        std::vector<KeyPart> parts;
        const std::string key = font.read(pictureOf(slot), &parts);
        std::string line = slot.name + ": " + (key.empty() ? "- (no letters)" : key) + "  [";
        for (const KeyPart& part : parts) {
            char one[64];
            std::snprintf(one, sizeof(one), "%s%s %d-%d", line.back() == '[' ? "" : ", ",
                          part.known ? ("'" + part.text + "' " + std::to_string(part.score).substr(0, 4)).c_str() : "?",
                          part.from, part.to);
            line += one;
        }
        std::printf("%s]\n", line.c_str());
        return key;
    };
    if (!teach) {
        for (const Slot& slot : slots) {
            say(slot);
        }
        return 0;
    }
    if (next >= argc) {
        return usage();
    }
    const std::string typed = argv[next];
    std::printf("before: ");
    say(slots[0]);
    const int learned = font.teach(pictureOf(slots[0]), typed);
    std::printf("learned %d glyphs for '%s' (%d in all); after: ", learned, typed.c_str(), font.glyphs());
    say(slots[0]);
    if (learned > 0 && !font.save(folder / kKeysFile, &error)) {
        std::printf("%s\n", error.c_str());
        return 1;
    }
    return 0;
}

} // namespace

// --stats: on each image (PATH may be an image or a folder of them, e.g. a recording), finds the interface as the UI
// finder does (core/UiLayout on its grey image) and reads stats.txt's bars on it (core/GameStats), then says per bar
// how often it was seen and its number read, kept or doubted (read, but not agreeing with the bar), with examples, and
// how long reading took. --list prints every image's reads, and why what was not seen was not. --skills BRAIN: the skills
// to look for, a profile's (.brain, as the app gives them), or a folder of grey NAME.pgm pictures (all with cooldowns).
// --plate X0 Y0 X1 Y1: also the name plate over that box on the image (as Select Target asks for its monster's), a
// line per image: hovered or selected, where, the name read.
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
    if (argc >= 1 && std::strcmp(argv[0], "--keys") == 0) {
        return keysBench(argc - 1, argv + 1);
    }
    std::vector<fs::path> paths;
    fs::path layoutFolder = "models/ui";
    bool list = false;
    bool listPieces = false; // --pieces: each image's found pieces (for scratch stress tests that move them).
    fs::path skillsFrom;
    PlateLook look{};
    for (int i = 0; i < argc; ++i) {
        if (std::strcmp(argv[i], "--plate") == 0 && i + 4 < argc) {
            look = {1, {std::atoi(argv[i + 1]), std::atoi(argv[i + 2]), std::atoi(argv[i + 3]), std::atoi(argv[i + 4])}};
            i += 4;
        } else if (std::strcmp(argv[i], "--layout") == 0 && i + 1 < argc) {
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
                    "  [--plate X0 Y0 X1 Y1]\n"
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
    reader.setLook(look);

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
        StatsArea areas[12];
        const int parts = reader.areas(on, width, height, areas, 12);
        std::vector<ColourPart> colour;
        for (int i = 0; i < parts; ++i) {
            colour.push_back({areas[i],
                              picture.bgra.data() + (static_cast<std::size_t>(areas[i].y0) * width + areas[i].x0) * 4,
                              static_cast<std::size_t>(width) * 4});
        }
        GameStats stats;
        reader.read(on, colour, stats);
        readMs.push_back(std::chrono::duration<double, std::milli>(Clock::now() - start).count());
        if (look.id != 0) {
            const NamePlate& plate = stats.plate;
            std::printf("plate %s: %s at %d,%d-%d,%d, %d orbs round it, name '%s'\n", path.filename().string().c_str(),
                        !plate.seen ? "none" : plate.selected ? "SELECTED" : "hovered", plate.at.x0, plate.at.y0,
                        plate.at.x1, plate.at.y1, plate.orbs, plate.name.data());
        }

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
            char said[128];
            std::snprintf(said, sizeof(said), " %s %s %s%s%s%s%s", gauge.name.data(), full.c_str(),
                          gauge.pressed ? "pressed" : gauge.text[0] != '\0' ? gauge.text.data() : "-",
                          gauge.doubted[0] != '\0' ? " doubted " : "", gauge.doubted.data(),
                          gauge.key[0] != '\0' ? " key " : "", gauge.key.data());
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
