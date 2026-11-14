// observex_heuristics.dll's depth: how the depth network's map is shown (heuristics/DepthRules) behind the plain C
// entry the app loads (heuristics/DepthAbi, engine/HeuristicsLibrary). Nothing here throws out of the DLL.
#include "heuristics/DepthAbi.h"
#include "heuristics/DepthRules.h"

namespace {

void* create() {
    try {
        return new observex::DepthRules();
    } catch (...) {
        return nullptr;
    }
}

void destroy(void* depthing) { delete static_cast<observex::DepthRules*>(depthing); }

bool shade(void* depthing, const observex::DepthInput* input, std::uint8_t* levels, std::uint32_t* palette) {
    try {
        return static_cast<observex::DepthRules*>(depthing)->shade(*input, levels, palette);
    } catch (...) {
        return false;
    }
}

} // namespace

extern "C" __declspec(dllexport) const ObservexDepthApi* observexDepth() {
    static const ObservexDepthApi api{kObservexDepthAbi, static_cast<std::uint32_t>(sizeof(observex::DepthInput)),
                                      static_cast<std::uint32_t>(sizeof(observex::GreyArea)), create, destroy, shade};
    return &api;
}
