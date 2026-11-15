#include "core/Geodata.h"
#include "core/Ground.h"
#include "core/WorldPosition.h"
#include "heuristics/LocationReader.h"
#include "heuristics/MinimapScroll.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <string>
#include <vector>

using namespace observex;

namespace {

// A complex or multilayer cell's floor as the files keep it (`z` a multiple of 8).
std::uint16_t floorValue(int z, int nswe = 15) {
    return static_cast<std::uint16_t>((static_cast<std::uint16_t>(z * 2) & 0xFFF0) | nswe);
}

void putWord(std::vector<std::uint8_t>& bytes, std::uint16_t value) {
    bytes.push_back(static_cast<std::uint8_t>(value & 0xFF));
    bytes.push_back(static_cast<std::uint8_t>(value >> 8));
}

// A region file: every block flat at `flat`, except those `special` writes (it returns false for the ones it leaves
// flat).
std::vector<std::uint8_t> region(int flat, const std::function<bool(int, int, std::vector<std::uint8_t>&)>& special) {
    std::vector<std::uint8_t> bytes;
    for (int bx = 0; bx < 256; ++bx) {
        for (int by = 0; by < 256; ++by) {
            if (!special || !special(bx, by, bytes)) {
                bytes.push_back(0);
                putWord(bytes, static_cast<std::uint16_t>(static_cast<std::int16_t>(flat)));
            }
        }
    }
    return bytes;
}

std::filesystem::path writeRegion(const std::string& folderName, const std::string& file,
                                  const std::vector<std::uint8_t>& bytes) {
    const std::filesystem::path folder = std::filesystem::temp_directory_path() / folderName;
    std::filesystem::create_directories(folder);
    std::ofstream out(folder / file, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    return folder;
}

// The test's region: block (0, 0) complex (each cell its own height), block (0, 1) multilayer (two floors a cell: the
// ground and a bridge), the rest flat at 500.
std::vector<std::uint8_t> testRegion() {
    return region(500, [](int bx, int by, std::vector<std::uint8_t>& bytes) {
        if (bx == 0 && by == 0) {
            bytes.push_back(1);
            for (int cell = 0; cell < 64; ++cell) {
                putWord(bytes, floorValue(-3208 + 8 * cell, cell == 5 ? 3 : 15));
            }
            return true;
        }
        if (bx == 0 && by == 1) {
            bytes.push_back(2);
            for (int cell = 0; cell < 64; ++cell) {
                bytes.push_back(2);
                putWord(bytes, floorValue(-3200));
                putWord(bytes, floorValue(-2800));
            }
            return true;
        }
        return false;
    });
}

} // namespace

TEST_CASE("A geodata region's flat, complex and multilayer blocks read as their floors") {
    GeoRegion read;
    std::string error;
    REQUIRE(read.parse(testRegion(), &error));
    GeoFloor floors[8];
    REQUIRE(read.floors(0, 0, floors, 8) == 1);
    CHECK(floors[0].z == -3208);
    CHECK(floors[0].nswe == 15);
    REQUIRE(read.floors(0, 5, floors, 8) == 1); // Cell index x * 8 + y.
    CHECK(floors[0].z == -3208 + 40);
    CHECK(floors[0].nswe == 3);
    REQUIRE(read.floors(3, 6, floors, 8) == 1);
    CHECK(floors[0].z == -3208 + 8 * (3 * 8 + 6));
    REQUIRE(read.floors(2, 9, floors, 8) == 2);
    CHECK(floors[0].z == -3200);
    CHECK(floors[1].z == -2800);
    REQUIRE(read.floors(100, 100, floors, 8) == 1);
    CHECK(floors[0].z == 500);
    CHECK(read.floors(2048, 0, floors, 8) == 0);

    std::vector<std::uint8_t> cut = testRegion();
    cut.resize(cut.size() / 2);
    CHECK_FALSE(read.parse(cut, &error));
    CHECK_FALSE(error.empty());
}

TEST_CASE("The floor at a world point is the one the character stands on") {
    const std::filesystem::path folder = writeRegion("observex-geodata-tests", "20_18.l2j", testRegion());
    Geodata geodata(folder);
    // Region 20_18 begins at world 0, 0; cell (2, 9) is under the bridge.
    const double x = 2 * kGeoCell + 8;
    const double y = 9 * kGeoCell + 8;
    REQUIRE(geodata.floorNear(x, y, -3150).has_value());
    CHECK(geodata.floorNear(x, y, -3150)->z == -3200);
    CHECK(geodata.floorNear(x, y, -2810)->z == -2800);
    CHECK(geodata.floorNear(x, y, -2900)->z == -3200);
    CHECK(geodata.floorNear(x, y, -5000)->z == -3200); // Below both: the lowest.
    CHECK_FALSE(geodata.floorNear(-8, y, 0).has_value()); // Region 19_18: no file.
    CHECK(Geodata::regionName(x, y) == "20_18.l2j");
    CHECK(Geodata::regionName(-8, y) == "19_18.l2j");
    CHECK(Geodata::regionName(-10163, 26863) == "19_18.l2j");
}

TEST_CASE("Only the regions nearest the one read last stay loaded") {
    // A row of eleven flat regions, 10_18 to 20_18, each at its own height.
    std::filesystem::path folder;
    for (int rx = 10; rx <= 20; ++rx) {
        folder = writeRegion("observex-geodata-kept-tests", std::to_string(rx) + "_18.l2j", region(rx * 10, nullptr));
    }
    Geodata geodata(folder);
    const std::shared_ptr<const GeoRegion> first = geodata.region(10, 18);
    REQUIRE(first != nullptr);
    for (int rx = 11; rx <= 20; ++rx) {
        REQUIRE(geodata.region(rx, 18) != nullptr);
        CHECK(geodata.loadedRegions() <= Geodata::kKept);
    }
    CHECK(geodata.loadedRegions() == Geodata::kKept);
    GeoFloor floors[2];
    REQUIRE(first->floors(0, 0, floors, 2) == 1); // Let go by the geodata, still whole where it is held.
    CHECK(floors[0].z == 100);
    CHECK(geodata.region(-1, 18) == nullptr); // No file: not counted.
    CHECK(geodata.loadedRegions() == Geodata::kKept);
    const std::shared_ptr<const GeoRegion> again = geodata.region(10, 18); // Read again when needed.
    REQUIRE(again != nullptr);
    REQUIRE(again->floors(5, 5, floors, 2) == 1);
    CHECK(floors[0].z == 100);
    CHECK(geodata.region(20, 18) != nullptr); // Let go as the farthest from 10_18: read again too.
    CHECK(geodata.loadedRegions() == Geodata::kKept);
}

TEST_CASE("The geodata's ground cuts a trunk down to the ground round it and marks it blocked") {
    // Flat at 496 but for a trunk of 2 x 2 cells in block (125, 125), its top 1,000 higher, no way into it.
    const std::vector<std::uint8_t> bytes = region(496, [](int bx, int by, std::vector<std::uint8_t>& out) {
        if (bx != 125 || by != 125) {
            return false;
        }
        out.push_back(1);
        for (int cell = 0; cell < 64; ++cell) {
            const int cx = cell / 8;
            const int cy = cell % 8;
            const bool trunk = cx >= 3 && cx <= 4 && cy >= 3 && cy <= 4;
            putWord(out, floorValue(trunk ? 1496 : 496, trunk ? 0 : 15));
        }
        return true;
    });
    const std::filesystem::path folder = writeRegion("observex-geodata-field-tests", "20_18.l2j", bytes);
    Geodata geodata(folder);
    const double middle = 1000 * kGeoCell; // Block 125's first cell.
    const std::shared_ptr<const GeoField> field = GeoField::build(geodata, middle, middle, 496);
    REQUIRE(field != nullptr);
    const double trunk = middle + 4 * kGeoCell; // Between its four cells.
    CHECK(field->ground(trunk, trunk) == doctest::Approx(496.0f).epsilon(0.001));
    CHECK(field->blocked(trunk - 8, trunk - 8));
    CHECK_FALSE(field->blocked(middle + 40 * kGeoCell, middle));
    CHECK(field->ground(middle + 40 * kGeoCell, middle) == doctest::Approx(496.0f).epsilon(0.001));
    CHECK(field->covers(middle, middle, 4200));
    CHECK_FALSE(field->covers(middle + 5000, middle, 4200));
    CHECK(field->missing() == 0);
    // Near world x 0: west of it is region 19_18, not there.
    const std::shared_ptr<const GeoField> edge = GeoField::build(geodata, 100, middle, 496);
    REQUIRE(edge != nullptr);
    CHECK(std::isnan(edge->ground(-100, middle)));
    CHECK(edge->ground(400, middle) == doctest::Approx(496.0f).epsilon(0.001));
    CHECK(edge->missing() > 0);
    CHECK(edge->missingRegion() == "19_18.l2j");
}

TEST_CASE("A world point lands on the screen where the camera sees it") {
    GroundCamera camera = groundCamera(0.6f, 10.0f, 2560.0f, 1440.0f);
    camera.known = true;
    camera.yaw = 0.0f; // Facing north.
    const double feet[3] = {1000.0, 2000.0, -3000.0};
    float sx = 0.0f;
    float sy = 0.0f;
    // The point it circles is the screen's middle.
    REQUIRE(worldOnScreen(camera, feet, feet[0], feet[1], feet[2] + kGroundTarget * kUnitsPerOwnHeight, sx, sy));
    CHECK(sx == doctest::Approx(camera.middleX).epsilon(0.001));
    CHECK(sy == doctest::Approx(camera.middleY).epsilon(0.001));
    // North (y down) is ahead: higher up the screen; east is right.
    REQUIRE(worldOnScreen(camera, feet, feet[0], feet[1] - 300.0, feet[2], sx, sy));
    CHECK(sx == doctest::Approx(camera.middleX).epsilon(0.001));
    const float aheadY = sy;
    REQUIRE(worldOnScreen(camera, feet, feet[0], feet[1], feet[2], sx, sy));
    CHECK(aheadY < sy);
    REQUIRE(worldOnScreen(camera, feet, feet[0] + 300.0, feet[1], feet[2], sx, sy));
    CHECK(sx > camera.middleX);
    // Facing east, east is ahead (same as north was facing north).
    camera.yaw = 1.5707963f;
    REQUIRE(worldOnScreen(camera, feet, feet[0] + 300.0, feet[1], feet[2], sx, sy));
    CHECK(sx == doctest::Approx(camera.middleX).epsilon(0.001));
    CHECK(sy == doctest::Approx(aheadY).epsilon(0.001));
    // Behind the camera: not on the screen.
    CHECK_FALSE(worldOnScreen(camera, feet, feet[0] - 2000.0, feet[1], feet[2], sx, sy));
}

TEST_CASE("A /loc answer's numbers are read as OCR may write them") {
    int x = 0;
    int y = 0;
    int z = 0;
    REQUIRE(LocationReader::parse("Current location: -10,163 / 26,863 / -3,208 (Near the Dark Elf Village)", x, y, z));
    CHECK(x == -10163);
    CHECK(y == 26863);
    CHECK(z == -3208);
    // Dots for commas, a space after one, '~' and an en dash for minus, the slashes read as 1 and 7.
    REQUIRE(LocationReader::parse("Current Iocation: ~10.163 1 26, 863 7 \xE2\x80\x93" "3,208 (Near the", x, y, z));
    CHECK(x == -10163);
    CHECK(y == 26863);
    CHECK(z == -3208);
    REQUIRE(LocationReader::parse("Current location: 83,400/147,888/-3,400", x, y, z));
    CHECK(x == 83400);
    CHECK(y == 147888);
    CHECK(z == -3400);
    CHECK_FALSE(LocationReader::parse("You have earned 1,200 experience and 51 SP.", x, y, z));
    CHECK_FALSE(LocationReader::parse("Current location: -10,163 / 26,863", x, y, z));
    CHECK_FALSE(LocationReader::parse("Current location: -10,163 / 26,863 / -93,208", x, y, z)); // No such height.
}

TEST_CASE("Where the character is: a /loc, the minimap's scale from a second, then the minimap") {
    WorldPosition position;
    position.minimap({true, 5, 0.0f, 0.0f, 1.0f});
    CHECK(position.spot().doubt == WorldPosition::Doubt::NoLocation);
    CHECK_FALSE(position.spot().known);

    LocationRead first{};
    first.count = 1;
    first.x = 1000;
    first.y = 2000;
    first.z = -3000;
    first.map = {true, 5, 0.0f, 0.0f, 1.0f};
    position.located(first);
    CHECK(position.sure());
    CHECK(position.spot().x == 1000.0);

    position.minimap({true, 5, 1.0f, 0.0f, 1.0f}); // A pixel: standing.
    CHECK(position.sure());
    position.minimap({true, 5, -50.0f, 0.0f, 1.0f}); // Walked, the scale not known.
    CHECK(position.spot().doubt == WorldPosition::Doubt::Moved);

    // 300 east while the map slid 50 west: 6 units a pixel.
    LocationRead second = first;
    second.count = 2;
    second.x = 1300;
    second.map = {true, 5, -50.0f, 0.0f, 1.0f};
    const std::string line = position.located(second);
    CHECK(line.find("learned") != std::string::npos);
    CHECK(position.scale() == doctest::Approx(6.0f));
    CHECK(position.sure());

    position.minimap({true, 5, -60.0f, 10.0f, 1.0f}); // The map 10 more west and 10 down: walked east and north.
    CHECK(position.sure());
    CHECK(position.spot().x == doctest::Approx(1360.0));
    CHECK(position.spot().y == doctest::Approx(1940.0));

    position.minimap({false, 5, 0.0f, 0.0f, 0.0f});
    CHECK(position.spot().doubt == WorldPosition::Doubt::MinimapHidden);
    CHECK(position.spot().x == doctest::Approx(1360.0)); // Where it was stays.
    position.minimap({true, 6, 0.0f, 0.0f, 1.0f}); // Another run: lost.
    CHECK(position.spot().doubt == WorldPosition::Doubt::MinimapLost);

    // A walk the other way than the map slid teaches nothing.
    WorldPosition other;
    other.located(first);
    LocationRead wrong = second;
    wrong.x = 700;
    CHECK(other.located(wrong).find("another way") != std::string::npos);
    CHECK(other.scale() == 0.0f);
}

namespace {

// A minimap's window, its map slid (`dx`, `dy`) pixels: hills and roads (two waves bent by others), grey.
struct MapImage {
    static constexpr int kX0 = 2053;
    static constexpr int kY0 = 4;
    static constexpr int kW = 502;
    static constexpr int kH = 503;
    std::vector<std::uint8_t> pixels = std::vector<std::uint8_t>(static_cast<std::size_t>(kW) * kH * 4);

    void draw(float dx, float dy, bool flat = false) {
        for (int y = 0; y < kH; ++y) {
            for (int x = 0; x < kW; ++x) {
                const double u = x - dx;
                const double v = y - dy;
                const double value =
                    flat ? 120.0
                         : 128.0 + 45.0 * std::sin(0.11 * u + 0.8 * std::sin(0.031 * v)) +
                               45.0 * std::sin(0.093 * v + 0.9 * std::sin(0.027 * u + 1.3));
                const auto grey = static_cast<std::uint8_t>(std::clamp(value, 0.0, 255.0));
                std::uint8_t* p = pixels.data() + (static_cast<std::size_t>(y) * kW + x) * 4;
                p[0] = p[1] = p[2] = grey;
                p[3] = 255;
            }
        }
    }
    UiPiece piece() const {
        UiPiece out;
        out.x0 = kX0;
        out.y0 = kY0;
        out.x1 = kX0 + kW;
        out.y1 = kY0 + kH;
        return out;
    }
    ColourPart part() const { return {{kX0, kY0, kX0 + kW, kY0 + kH}, pixels.data(), static_cast<std::size_t>(kW) * 4}; }
};

} // namespace

TEST_CASE("The minimap's slide is followed while walking, and a run ends when it is lost") {
    MinimapScroll scroll;
    MapImage image;
    const UiPiece piece = image.piece();
    image.draw(0.0f, 0.0f);
    const ColourPart part = image.part();
    const MinimapRead start = scroll.follow(&piece, &part);
    REQUIRE(start.seen);
    CHECK(scroll.points() >= MinimapScroll::kMinPoints);
    CHECK(start.x == 0.0f);

    // Walking: 3 pixels west and 2 south an image, 30 images (the key taken again on the way).
    MinimapRead read;
    for (int step = 1; step <= 30; ++step) {
        image.draw(-3.0f * static_cast<float>(step), 2.0f * static_cast<float>(step));
        read = scroll.follow(&piece, &part);
        REQUIRE(read.seen);
        CHECK(read.run == start.run);
    }
    CHECK(read.x == doctest::Approx(-90.0f).epsilon(0.015));
    CHECK(read.y == doctest::Approx(60.0f).epsilon(0.02));

    // Hidden for a while, walked on 10 pixels meanwhile: found again in the same run.
    CHECK_FALSE(scroll.follow(nullptr, nullptr).seen);
    image.draw(-100.0f, 60.0f);
    read = scroll.follow(&piece, &part);
    REQUIRE(read.seen);
    CHECK(read.run == start.run);
    CHECK(read.x == doctest::Approx(-100.0f).epsilon(0.015));

    // The sea (nothing to follow): not seen, and a new run once there is a map again.
    image.draw(0.0f, 0.0f, true);
    for (int k = 0; k < MinimapScroll::kMisses + 1; ++k) {
        CHECK_FALSE(scroll.follow(&piece, &part).seen);
    }
    image.draw(0.0f, 0.0f);
    read = scroll.follow(&piece, &part);
    REQUIRE(read.seen);
    CHECK(read.run != start.run);
}
