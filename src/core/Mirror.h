#pragma once

#include "core/Detection.h"

#include <span>

namespace observex {

// How differently a model saw an image and its mirror image (`mirrored` turned back, so both are in the image's own
// pixels). Each thing at least `minSure` sure in either counts how far its sureness is from how sure the other was
// that the thing overlapping it most there (at least `minOverlap`) is of the same kind, or from 0 if none does; the
// total is halved. A thing 90% sure in both adds 0, 90% in one and 50% in the other 0.4, 90% in one and missing in the
// other 0.45; with kinds, Monster 90% in one and Player 90% (Monster 5%) in the other 0.85. A mirrored screen
// looks like one the model knows, so a big difference means it does not really know what it is looking at: on its
// held-out images such images hold most of its mistakes (docs/training-loop.md, step 6.2). Pass both lists down to
// LiveLog::kMinSure (0.1), so a thing just under `minSure` in one view still answers for it.
float mirrorDisagreement(std::span<const Detection> plain, std::span<const Detection> mirrored, float minSure = 0.3f,
                         float minOverlap = 0.3f);

} // namespace observex
