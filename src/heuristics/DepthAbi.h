#pragma once

#include "core/Depth.h"
#include "core/Heuristics.h"

#include <cstdint>

// What observex_heuristics.dll hands the app for depth (core/Depth's Depthing; engine/HeuristicsLibrary): plain C, an
// entry of its own beside heuristics/Abi's, so the app and the DLL need only agree on this and on DepthInput and
// GreyArea, which the sizes below check. Change any of them and bump kObservexDepthAbi: the app then shades with the
// rules built into it until both are rebuilt. A DLL without this entry (built before 2026-11-12's depth) likewise.
extern "C" {

inline constexpr std::uint32_t kObservexDepthAbi = 1;

struct ObservexDepthApi {
    std::uint32_t abi;       // kObservexDepthAbi when the DLL was built.
    std::uint32_t inputSize; // sizeof(DepthInput) and sizeof(GreyArea) then.
    std::uint32_t areaSize;
    // A new shading; null if it could not be made.
    void* (*create)();
    void (*destroy)(void* depthing);
    // Depthing::shade; false if it failed.
    bool (*shade)(void* depthing, const observex::DepthInput* input, std::uint8_t* levels, std::uint32_t* palette);
};

using ObservexDepthEntry = const ObservexDepthApi* (*)();
inline constexpr const char* kObservexDepthEntry = "observexDepth";

} // extern "C"
