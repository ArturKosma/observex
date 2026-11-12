#pragma once

#include "core/Detection.h"
#include "core/Ground.h"
#include "core/Heuristics.h"

#include <cstdint>

// What observex_heuristics.dll hands the app (engine/HeuristicsLibrary): plain C, so the two only need to agree on
// this and on Detection and Tracked (core/Detection), GreyView, GreyArea and SnapFind (core/Heuristics) and
// GroundCamera, GroundHints and GroundNote (core/Ground), which the sizes below check.
// Change any of them and bump kObservexHeuristicsAbi: the app then refuses a DLL built for another one, and keeps the
// heuristics it has, until both are rebuilt. 2 since 2026-11-11: snapping; 3 since that evening: snapping's searches
// recorded and replayed (SnapFind, GreyView::image); 4 since 2026-11-12: following the camera (the ground); 5 since
// that evening: the ground turned by the minimap between follows (headAreas, head).
extern "C" {

inline constexpr std::uint32_t kObservexHeuristicsAbi = 5;

struct ObservexHeuristicsApi {
    std::uint32_t abi;           // kObservexHeuristicsAbi when the DLL was built.
    std::uint32_t detectionSize; // sizeof(Detection) and sizeof(Tracked) then.
    std::uint32_t trackedSize;
    // A new set of heuristics with nothing followed yet; null if it could not be made.
    void* (*create)();
    void (*destroy)(void* heuristics);
    // Heuristics::update; -1 if it failed (nothing written).
    int (*update)(void* heuristics, double timeMs, const observex::Detection* found, int count,
                  observex::Tracked* out, int max);
    void (*clear)(void* heuristics);

    // Snapping (core/Heuristics), likewise: a new one, or null.
    std::uint32_t greyViewSize; // sizeof(GreyView) when the DLL was built.
    void* (*createSnapping)();
    void (*destroySnapping)(void* snapping);
    // Snapping::looked; `image` may be null. False if it failed.
    bool (*looked)(void* snapping, double lookMs, const observex::GreyView* image, const observex::Tracked* shown,
                   int count);
    // Snapping::snap; -1 if it failed (nothing written).
    int (*snap)(void* snapping, double timeMs, const observex::GreyView* image, observex::Tracked* out, int max);
    double (*snapLeadMs)(void* snapping);
    void (*clearSnapping)(void* snapping);
    // Snapping::learnAreas and snapAreas; -1 if they failed (nothing written).
    std::uint32_t greyAreaSize; // sizeof(GreyArea) when the DLL was built.
    int (*learnAreas)(void* snapping, const observex::Tracked* shown, int count, observex::GreyArea* out, int max);
    int (*snapAreas)(void* snapping, double timeMs, bool look, double lookMs, const observex::Tracked* shown,
                     int count, observex::GreyArea* out, int max);
    // Snapping::finds, up to `max` into `out` (how many; -1 if it failed), and Snapping::replay (false if it failed).
    std::uint32_t snapFindSize; // sizeof(SnapFind) when the DLL was built.
    int (*finds)(void* snapping, observex::SnapFind* out, int max);
    bool (*replay)(void* snapping, const observex::SnapFind* recorded, int count);

    // Following the camera (core/Ground's Grounding), likewise: a new one, or null.
    std::uint32_t groundCameraSize; // sizeof(GroundCamera), sizeof(GroundHints) and sizeof(GroundNote) when built.
    std::uint32_t groundHintsSize;
    std::uint32_t groundNoteSize;
    void* (*createGround)();
    void (*destroyGround)(void* ground);
    // Grounding's begin (false if it failed), firstLevel, fullAreas (-1 if it failed), end (1 fitted, 0 not, -1
    // failed), camera and note (false if they failed: nothing written) and clear.
    bool (*groundBegin)(void* ground, double timeMs, int width, int height, const observex::GroundHints* hints);
    int (*groundFirstLevel)(void* ground);
    int (*groundFullAreas)(void* ground, observex::GreyArea* out, int max);
    int (*groundEnd)(void* ground, const observex::GreyView* image);
    bool (*groundCamera)(void* ground, observex::GroundCamera* out);
    bool (*groundNote)(void* ground, observex::GroundNote* out);
    void (*clearGround)(void* ground);
    // Grounding's headAreas (-1 if it failed) and head (1 the cone seen, 0 not, -1 failed).
    int (*groundHeadAreas)(void* ground, double timeMs, const observex::GroundHints* hints, observex::GreyArea* out,
                           int max);
    int (*groundHead)(void* ground, const observex::GreyView* image);
};

// The one function the DLL exports, by this name.
using ObservexHeuristicsEntry = const ObservexHeuristicsApi* (*)();
inline constexpr const char* kObservexHeuristicsEntry = "observexHeuristics";

} // extern "C"
