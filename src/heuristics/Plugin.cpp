// observex_heuristics.dll: today's heuristics (heuristics/Rules), snapping (heuristics/SnapRules) and following the
// camera (heuristics/GroundFollower) behind the plain C entry the app loads (heuristics/Abi, engine/
// HeuristicsLibrary). Nothing here throws out of the DLL.
#include "heuristics/Abi.h"
#include "heuristics/GroundMotion.h"
#include "heuristics/Rules.h"
#include "heuristics/SnapRules.h"

#include <algorithm>
#include <span>

namespace {

void* create() {
    try {
        return new observex::Rules();
    } catch (...) {
        return nullptr;
    }
}

void destroy(void* heuristics) { delete static_cast<observex::Rules*>(heuristics); }

int update(void* heuristics, double timeMs, const observex::Detection* found, int count, observex::Tracked* out,
           int max) {
    try {
        return static_cast<observex::Rules*>(heuristics)->update(
            timeMs, std::span<const observex::Detection>(found, static_cast<std::size_t>(count > 0 ? count : 0)), out,
            max);
    } catch (...) {
        return -1;
    }
}

void clear(void* heuristics) { static_cast<observex::Rules*>(heuristics)->clear(); }

void* createSnapping() {
    try {
        return new observex::SnapRules();
    } catch (...) {
        return nullptr;
    }
}

void destroySnapping(void* snapping) { delete static_cast<observex::SnapRules*>(snapping); }

bool looked(void* snapping, double lookMs, const observex::GreyView* image, const observex::Tracked* shown, int count) {
    try {
        static_cast<observex::SnapRules*>(snapping)->looked(
            lookMs, image, std::span<const observex::Tracked>(shown, static_cast<std::size_t>(count > 0 ? count : 0)));
        return true;
    } catch (...) {
        return false;
    }
}

int snap(void* snapping, double timeMs, const observex::GreyView* image, observex::Tracked* out, int max) {
    try {
        return static_cast<observex::SnapRules*>(snapping)->snap(timeMs, *image, out, max);
    } catch (...) {
        return -1;
    }
}

double snapLeadMs(void* snapping) { return static_cast<observex::SnapRules*>(snapping)->leadMs(); }

void clearSnapping(void* snapping) { static_cast<observex::SnapRules*>(snapping)->clear(); }

int learnAreas(void* snapping, const observex::Tracked* shown, int count, observex::GreyArea* out, int max) {
    try {
        return static_cast<observex::SnapRules*>(snapping)->learnAreas(
            std::span<const observex::Tracked>(shown, static_cast<std::size_t>(count > 0 ? count : 0)), out, max);
    } catch (...) {
        return -1;
    }
}

int snapAreas(void* snapping, double timeMs, bool look, double lookMs, const observex::Tracked* shown, int count,
              observex::GreyArea* out, int max) {
    try {
        return static_cast<observex::SnapRules*>(snapping)->snapAreas(
            timeMs, look, lookMs,
            std::span<const observex::Tracked>(shown, static_cast<std::size_t>(count > 0 ? count : 0)), out, max);
    } catch (...) {
        return -1;
    }
}

int finds(void* snapping, observex::SnapFind* out, int max) {
    try {
        const std::span<const observex::SnapFind> all = static_cast<observex::SnapRules*>(snapping)->finds();
        const int count = std::min(static_cast<int>(all.size()), max > 0 ? max : 0);
        std::copy_n(all.begin(), count, out);
        return count;
    } catch (...) {
        return -1;
    }
}

bool replay(void* snapping, const observex::SnapFind* recorded, int count) {
    try {
        static_cast<observex::SnapRules*>(snapping)->replay(
            std::span<const observex::SnapFind>(recorded, static_cast<std::size_t>(count > 0 ? count : 0)));
        return true;
    } catch (...) {
        return false;
    }
}

void* createGround() {
    try {
        return new observex::GroundFollower();
    } catch (...) {
        return nullptr;
    }
}

void destroyGround(void* ground) { delete static_cast<observex::GroundFollower*>(ground); }

bool groundBegin(void* ground, double timeMs, int width, int height, const observex::GroundHints* hints) {
    try {
        static_cast<observex::GroundFollower*>(ground)->begin(timeMs, width, height, *hints);
        return true;
    } catch (...) {
        return false;
    }
}

int groundFirstLevel(void* ground) { return static_cast<observex::GroundFollower*>(ground)->firstLevel(); }

int groundFullAreas(void* ground, observex::GreyArea* out, int max) {
    try {
        return static_cast<observex::GroundFollower*>(ground)->fullAreas(out, max);
    } catch (...) {
        return -1;
    }
}

int groundEnd(void* ground, const observex::GreyView* image) {
    try {
        return static_cast<observex::GroundFollower*>(ground)->end(*image) ? 1 : 0;
    } catch (...) {
        return -1;
    }
}

bool groundCamera(void* ground, observex::GroundCamera* out) {
    try {
        *out = static_cast<observex::GroundFollower*>(ground)->camera();
        return true;
    } catch (...) {
        return false;
    }
}

bool groundNote(void* ground, observex::GroundNote* out) {
    try {
        *out = static_cast<observex::GroundFollower*>(ground)->note();
        return true;
    } catch (...) {
        return false;
    }
}

void clearGround(void* ground) { static_cast<observex::GroundFollower*>(ground)->clear(); }

int groundHeadAreas(void* ground, double timeMs, const observex::GroundHints* hints, observex::GreyArea* out,
                    int max) {
    try {
        return static_cast<observex::GroundFollower*>(ground)->headAreas(timeMs, *hints, out, max);
    } catch (...) {
        return -1;
    }
}

int groundHeadFirstLevel(void* ground) {
    return static_cast<observex::GroundFollower*>(ground)->headFirstLevel();
}

int groundHead(void* ground, const observex::GreyView* image) {
    try {
        return static_cast<observex::GroundFollower*>(ground)->head(*image) ? 1 : 0;
    } catch (...) {
        return -1;
    }
}

bool groundDepth(void* ground, const observex::DepthInput* map, double timeMs, observex::GroundDepthNote* out) {
    try {
        *out = static_cast<observex::GroundFollower*>(ground)->depth(*map, timeMs);
        return true;
    } catch (...) {
        return false;
    }
}

} // namespace

extern "C" __declspec(dllexport) const ObservexHeuristicsApi* observexHeuristics() {
    static const ObservexHeuristicsApi api{kObservexHeuristicsAbi,
                                           static_cast<std::uint32_t>(sizeof(observex::Detection)),
                                           static_cast<std::uint32_t>(sizeof(observex::Tracked)),
                                           create,
                                           destroy,
                                           update,
                                           clear,
                                           static_cast<std::uint32_t>(sizeof(observex::GreyView)),
                                           createSnapping,
                                           destroySnapping,
                                           looked,
                                           snap,
                                           snapLeadMs,
                                           clearSnapping,
                                           static_cast<std::uint32_t>(sizeof(observex::GreyArea)),
                                           learnAreas,
                                           snapAreas,
                                           static_cast<std::uint32_t>(sizeof(observex::SnapFind)),
                                           finds,
                                           replay,
                                           static_cast<std::uint32_t>(sizeof(observex::GroundCamera)),
                                           static_cast<std::uint32_t>(sizeof(observex::GroundHints)),
                                           static_cast<std::uint32_t>(sizeof(observex::GroundNote)),
                                           createGround,
                                           destroyGround,
                                           groundBegin,
                                           groundFirstLevel,
                                           groundFullAreas,
                                           groundEnd,
                                           groundCamera,
                                           groundNote,
                                           clearGround,
                                           groundHeadAreas,
                                           groundHeadFirstLevel,
                                           groundHead,
                                           static_cast<std::uint32_t>(sizeof(observex::DepthInput)),
                                           static_cast<std::uint32_t>(sizeof(observex::GroundDepthNote)),
                                           groundDepth};
    return &api;
}
