#pragma once

#include "core/GameStats.h"
#include "heuristics/MessageLines.h"
#include "heuristics/TextOcr.h"

#include <cstdint>
#include <string>
#include <string_view>

namespace observex {

// Reads what the character picked up, as the game says it among its system messages (the user asked, 2026-11-16: "in
// the log I just want the exp and drop pick up like adena or item"): yellow lines at the bottom of the Messages window,
// "You have obtained 69 Adena." and "You have earned 1st Onyx Talisman." (or "... earned 3 Animal Bone."). A white
// "You have earned Armor Mastery." is a skill learned: not read.
//
// Each new yellow line (heuristics/MessageLines) is read with Windows' OCR (~2 ms a line, only when one comes): smooth
// x3 and blocks x4, the same text taken, else the one a third reading (smooth x4) agrees with, else the first that
// says what was picked up. A new yellow line right under one that said it and not saying it itself is the rest of a
// long name (the window wraps it). Use one from one thread.
class PickupReader {
public:
    // Reads this image's new lines (`lines`, MessageLines::look done on it): each pickup goes into `out` (its count one
    // more, the newest in items, Adena added to adena). Says what it read into `why` (empty: nothing to say).
    void read(MessageLines& lines, TextOcr& ocr, PickupRead& out, std::string& why);

    // What `text` says was picked up: "You have obtained 69 Adena." as "Adena" and 69, "You have earned 1st Onyx
    // Talisman." as "1st Onyx Talisman" and 1, "... earned 3 Animal Bone." as "Animal Bone" and 3 (thousands written
    // with ',' or '.'). False if it says neither "obtained" nor "earned" (a letter or so read wrong is fine) with a name
    // after it.
    static bool parse(std::string_view text, std::string& item, std::int64_t& amount);

private:
    std::string error_;
};

} // namespace observex
