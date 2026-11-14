#pragma once

#include "core/GameStats.h"
#include "core/UiLayout.h"

#include <cstdint>

// What observex_stats.dll hands the app (engine/StatsLibrary): plain C, so the two need only agree on this and on
// UiPiece (core/UiLayout), StatsArea, ColourPart, GameStats and SkillPicture (core/GameStats), which the sizes below
// check. Change
// any of them and bump kObservexStatsAbi: the app then reads with the rules built into it until both are rebuilt.
extern "C" {

inline constexpr std::uint32_t kObservexStatsAbi = 4;

struct ObservexStatsApi {
    std::uint32_t abi;       // kObservexStatsAbi when the DLL was built.
    std::uint32_t pieceSize; // sizeof(UiPiece), sizeof(StatsArea), sizeof(ColourPart) and sizeof(GameStats) then.
    std::uint32_t areaSize;
    std::uint32_t partSize;
    std::uint32_t statsSize;
    std::uint32_t skillSize; // sizeof(SkillPicture).
    // A new reader by `folder`'s stats.txt (UTF-8), or null if it could not be made. One that cannot read stats.txt
    // is made all the same (it reads nothing) and says why in `error` (`errorSize` bytes, with its 0); else error[0]
    // is 0.
    void* (*create)(const char* folder, char* error, std::uint32_t errorSize);
    void (*destroy)(void* reader);
    // StatsReading::areas; -1 if it failed (nothing written).
    int (*areas)(void* reader, const observex::UiPiece* pieces, int count, int width, int height,
                 observex::StatsArea* out, int max);
    // StatsReading::read; false if it failed (nothing read).
    bool (*read)(void* reader, const observex::UiPiece* pieces, int count, const observex::ColourPart* parts,
                 int partCount, observex::GameStats* out);
    // StatsReading::setSkills (the pictures are copied); false if it failed (the reader keeps none).
    bool (*setSkills)(void* reader, const observex::SkillPicture* skills, int count);
};

// The one function the DLL exports, by this name.
using ObservexStatsEntry = const ObservexStatsApi* (*)();
inline constexpr const char* kObservexStatsEntry = "observexStats";

} // extern "C"
