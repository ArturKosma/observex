#include "core/ShotKey.h"

#include <doctest/doctest.h>

using namespace observex;

namespace {

constexpr unsigned kS = 'S';
constexpr unsigned kCtrl = 0xA2;

} // namespace

TEST_CASE("Shift then Win, released: a screenshot") {
    ShotKey key;
    CHECK_FALSE(key.key(ShotKey::kLeftShift, true, true));
    CHECK_FALSE(key.key(ShotKey::kLeftWin, true, true));
    CHECK_FALSE(key.key(ShotKey::kLeftWin, true, true)); // Held: it repeats.
    CHECK(key.key(ShotKey::kLeftWin, false, true));
    CHECK_FALSE(key.key(ShotKey::kLeftShift, false, false));
}

TEST_CASE("Win then Shift counts too, and Shift may come up first") {
    ShotKey key;
    CHECK_FALSE(key.key(ShotKey::kRightWin, true, false));
    CHECK_FALSE(key.key(ShotKey::kRightShift, true, true));
    CHECK_FALSE(key.key(ShotKey::kRightShift, false, false));
    CHECK(key.key(ShotKey::kRightWin, false, false));
}

TEST_CASE("Win alone, or a Windows shortcut with Shift+Win, takes none") {
    ShotKey key;
    CHECK_FALSE(key.key(ShotKey::kLeftWin, true, false));
    CHECK_FALSE(key.key(ShotKey::kLeftWin, false, false));

    // Win+Shift+S (the snipping tool).
    CHECK_FALSE(key.key(ShotKey::kLeftShift, true, true));
    CHECK_FALSE(key.key(ShotKey::kLeftWin, true, true));
    CHECK_FALSE(key.key(kS, true, true));
    CHECK_FALSE(key.key(kS, false, true));
    CHECK_FALSE(key.key(ShotKey::kLeftWin, false, true));

    // Ctrl+Shift+Win+B.
    CHECK_FALSE(key.key(kCtrl, true, true));
    CHECK_FALSE(key.key(ShotKey::kLeftWin, true, true));
    CHECK_FALSE(key.key(kCtrl, true, true));
    CHECK_FALSE(key.key(ShotKey::kLeftWin, false, true));
}

TEST_CASE("each Shift+Win is one screenshot, and a key before it does not spoil it") {
    ShotKey key;
    CHECK_FALSE(key.key(kS, true, false));
    CHECK_FALSE(key.key(kS, false, false));
    int shots = 0;
    for (int i = 0; i < 3; ++i) {
        key.key(ShotKey::kLeftWin, true, true);
        shots += key.key(ShotKey::kLeftWin, false, true) ? 1 : 0;
    }
    CHECK(shots == 3);
    CHECK_FALSE(key.key(ShotKey::kLeftWin, false, true)); // An up with no down before it.
}
