#pragma once

#include "engine/Detector.h"
#include "engine/ImageChannel.h"

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <mutex>
#include <string>

namespace observex {

// A recording's log of what live recognition saw, so it can be looked at afterwards image by image (how sure the
// model was of each thing, and for how long; one-image blips): every image the recogniser looked at with everything
// it found, which of those images the recording saved, and the user's marks. One text file in the recording's folder
// (<category>-live.txt); its header explains the lines.
//
// Written from several threads (the recogniser about 200 times a second, the recorder, the UI). Each call formats
// its line first, then adds it under a lock to a large buffer that reaches the disk in big writes, so a call costs
// the recogniser a few microseconds.
class LiveLog {
public:
    // Least sure a thing the recogniser logs can be: below what is drawn (Recognizer::kMinSure), so the log also shows
    // things hovering just under the line. About 4 things per image, about 85 MB per hour.
    static constexpr float kMinSure = 0.1f;

    // Creates `file` (and its folder) and writes the header; `about` describes the model and the screen. Times in the
    // log count from now. Throws on failure.
    LiveLog(const std::filesystem::path& file, const std::string& about);
    ~LiveLog();

    LiveLog(const LiveLog&) = delete;
    LiveLog& operator=(const LiveLog&) = delete;

    // The recogniser looked at image `stamp` and found `count` things in `found` (core/LiveLines: with their kinds, for
    // a model with kinds).
    void looked(const ImageStamp& stamp, const Detection* found, int count);
    // Asked whether image `stamp` is worth recording, the recogniser looked at it again mirrored and saw it
    // `disagreement` differently (core/Mirror); `closestKinds` is core/Detection's closestKinds over both looks, -1
    // (not written) for a model without kinds.
    void mirrored(const ImageStamp& stamp, float disagreement, float closestKinds = -1.0f);
    // The recorder saved image `stamp` as `name`.
    void saved(const ImageStamp& stamp, const char* name);
    // The user marked this moment; `number` counts the marks from 1. Also writes everything so far to the disk.
    void mark(unsigned number);
    // Capture started again (e.g. after a display mode change), so image numbers start again from 1.
    void resumed();

private:
    double msSince(std::int64_t qpc) const;
    void add(const char* line, int length);

    std::mutex mutex_;
    std::FILE* file_ = nullptr;
    std::int64_t startQpc_ = 0;
    double msPerTick_ = 0.0;
};

} // namespace observex
