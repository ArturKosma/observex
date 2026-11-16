#include "core/MonsterMemory.h"

#include <doctest/doctest.h>

#include <cmath>
#include <vector>

using namespace observex;

namespace {

constexpr float kWidth = 2560.0f;
constexpr float kHeight = 1440.0f;
constexpr double kPi = 3.14159265358979323846;

GroundCamera camera() {
    return groundCamera(0.44f, 8.0f, kWidth, kHeight); // 25 degrees down, 8 own heights off.
}

// A monster standing `east`, `north` of the own feet (own heights), as the camera turned `heading` right sees it.
MonsterSighting seen(std::uint32_t id, double heading, float east, float north) {
    const double c = std::cos(heading);
    const double s = std::sin(heading);
    return {id, static_cast<float>(east * c - north * s), static_cast<float>(east * s + north * c)};
}

} // namespace

TEST_CASE("The monster memory keeps a monster while the camera turns away, and knows it again under a new id") {
    MonsterMemory memory;
    const std::vector<MonsterSighting> first = {seen(5, 0.0, 2.0f, 10.0f)};
    memory.look(1.0, 0.0, camera(), kWidth, kHeight, first);
    REQUIRE(memory.count() == 1);
    const std::uint32_t key = memory.monsters().front().key;
    // Turned half round: it is behind, out of view, and nothing is seen; it stays.
    for (double t = 1.1; t < 5.0; t += 0.1) {
        memory.look(t, kPi, camera(), kWidth, kHeight, {});
    }
    REQUIRE(memory.count() == 1);
    // Its bearing from where the camera looks now: behind, a little left (it stood a little right of ahead).
    CHECK(std::fabs(MonsterMemory::bearing(memory.monsters().front(), kPi)) > 2.9f);
    // Back round: the tracker gives it a new id, a little off where it was (the distances are not exact).
    const std::vector<MonsterSighting> again = {seen(9, 2.0 * kPi, 2.3f, 10.8f)};
    memory.look(5.1, 2.0 * kPi, camera(), kWidth, kHeight, again);
    CHECK(memory.count() == 1);
    CHECK(memory.monsters().front().key == key);
    CHECK(memory.monsters().front().id == 9);
    CHECK(memory.withId(9) != nullptr);
    CHECK(memory.withId(5) == nullptr);
}

TEST_CASE("The monster memory forgets one whose spot is in view and empty, not one out of view") {
    MonsterMemory memory;
    const std::vector<MonsterSighting> both = {seen(1, 0.0, 0.0f, 10.0f), seen(2, 0.0, 2.0f, 12.0f)};
    memory.look(1.0, 0.0, camera(), kWidth, kHeight, both);
    REQUIRE(memory.count() == 2);
    // Turned round: both behind the camera, out of view, and nothing found: kept.
    for (double t = 1.1; t < 2.5; t += 0.05) {
        memory.look(t, kPi, camera(), kWidth, kHeight, {});
    }
    CHECK(memory.count() == 2);
    // Back to where both were seen, nothing found there for more than kGoneSeconds: both gone.
    for (double t = 2.5; t < 2.5 + MonsterMemory::kGoneSeconds + 0.2; t += 0.05) {
        memory.look(t, 0.0, camera(), kWidth, kHeight, {});
    }
    CHECK(memory.count() == 0);
}

TEST_CASE("The monster memory forgets one not seen for kForgetSeconds, and one far off in view is not taken as gone") {
    MonsterMemory memory;
    const std::vector<MonsterSighting> far = {seen(3, 0.0, 0.0f, 40.0f)}; // Past kSureView: the model may miss it.
    memory.look(1.0, 0.0, camera(), kWidth, kHeight, far);
    for (double t = 1.1; t < 10.0; t += 0.1) {
        memory.look(t, 0.0, camera(), kWidth, kHeight, {});
    }
    CHECK(memory.count() == 1);
    memory.look(1.0 + MonsterMemory::kForgetSeconds + 0.1, 0.0, camera(), kWidth, kHeight, {});
    CHECK(memory.count() == 0);
}

TEST_CASE("The monster memory's nearest, given up on, forgotten and cleared") {
    MonsterMemory memory;
    const std::vector<MonsterSighting> two = {seen(1, 0.0, 0.0f, 12.0f), seen(2, 0.0, 3.0f, 5.0f)};
    memory.look(1.0, 0.0, camera(), kWidth, kHeight, two);
    REQUIRE(memory.nearest() != nullptr);
    CHECK(memory.nearest()->id == 2);
    CHECK(memory.nearest()->distance() == doctest::Approx(std::hypot(3.0f, 5.0f)));
    const std::uint32_t near = memory.nearest()->key;
    memory.giveUp(near);
    CHECK(memory.count() == 2); // Once: still counted.
    memory.giveUp(near);
    CHECK(memory.count() == 1); // Twice: given up on,
    CHECK(memory.nearest()->id == 1);
    memory.look(1.1, 0.0, camera(), kWidth, kHeight, two);
    CHECK(memory.monsters().size() == 2); // but kept, so not found again as a new one.
    memory.giveUp(memory.nearest()->key, true);
    CHECK(memory.count() == 0);
    CHECK(memory.nearest() == nullptr);
    memory.forget(near);
    CHECK(memory.monsters().size() == 1);
    memory.clear();
    CHECK(memory.monsters().empty());
    memory.look(2.0, 0.0, camera(), kWidth, kHeight, two);
    CHECK(memory.count() == 2); // Cleared: new again, with new keys.
    CHECK(memory.withKey(near) == nullptr);
}

TEST_CASE("The monster memory's bearings turn with the camera") {
    MonsterMemory memory;
    const std::vector<MonsterSighting> east = {seen(4, 0.0, 10.0f, 0.0f)}; // Straight right of the feet.
    memory.look(1.0, 0.0, camera(), kWidth, kHeight, east);
    REQUIRE(memory.count() == 1);
    const MonsterMemory::Monster& m = memory.monsters().front();
    CHECK(MonsterMemory::bearing(m, 0.0) == doctest::Approx(kPi / 2.0).epsilon(0.001));
    CHECK(MonsterMemory::bearing(m, kPi / 2.0) == doctest::Approx(0.0).epsilon(0.001)); // Turned right to face it.
    CHECK(MonsterMemory::bearing(m, kPi) == doctest::Approx(-kPi / 2.0).epsilon(0.001));
    // Seen from a camera turned 90 degrees right it is straight ahead, and stays where it stands.
    const std::vector<MonsterSighting> ahead = {{4, 0.0f, 10.0f}};
    memory.look(1.1, kPi / 2.0, camera(), kWidth, kHeight, ahead);
    CHECK(memory.monsters().front().x == doctest::Approx(10.0f).epsilon(0.001));
    CHECK(std::fabs(memory.monsters().front().y) < 0.01f);
}
