#include "engine/LiveLog.h"

#include "core/LiveLines.h"
#include "engine/Recognizer.h"

#include <share.h>

#include <stdexcept>

namespace observex {

namespace {

std::int64_t qpcNow() {
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return now.QuadPart;
}

constexpr std::size_t kBufferBytes = 1 << 20; // About 40 s of looking; written in one go when full.

} // namespace

LiveLog::LiveLog(const std::filesystem::path& file, const std::string& about) : startQpc_(qpcNow()) {
    LARGE_INTEGER frequency;
    QueryPerformanceFrequency(&frequency);
    msPerTick_ = 1000.0 / static_cast<double>(frequency.QuadPart);

    std::filesystem::create_directories(file.parent_path());
    file_ = _wfsopen(file.c_str(), L"wb", _SH_DENYWR); // Binary: plain \n line ends. Others may read it meanwhile.
    if (file_ == nullptr) {
        throw std::runtime_error("cannot create " + toUtf8(file.c_str()));
    }
    std::setvbuf(file_, nullptr, _IOFBF, kBufferBytes);
    std::fprintf(file_,
                 "# observex live log: what live recognition found on every image it looked at while recording.\n"
                 "# %s\n"
                 "# Times are milliseconds since the recording started, of when the screen showed the image.\n"
                 "# look IMAGE TIME [SURE X Y WIDTH HEIGHT]...: the model looked at screen image number IMAGE and found\n"
                 "#   these things, surest first, in screen pixels (at least %.0f%% sure; the app draws %.0f%% and up).\n"
                 "#   Image numbers count every new screen image, so gaps are images it had no time for.\n"
                 "# kinds IMAGE [KIND SURE SECOND SURE]...: for a model with kinds, right after its look, each thing's\n"
                 "#   surest kind and how sure of it, then its second-surest, in the same order (kinds numbered from 0\n"
                 "#   as in the model's model.json).\n"
                 "# mirror IMAGE HOW [KINDS]: asked whether to save it (recording only unsure), the model looked at image\n"
                 "#   IMAGE again mirrored and saw it HOW differently (0 = the same; saved from %.2f, see core/Mirror).\n"
                 "#   KINDS, for a model with kinds: the closest any thing's two surest kinds were in either look (also\n"
                 "#   saved under %.2f: unsure which kind it is).\n"
                 "# saved FILE IMAGE TIME: the recording saved screen image number IMAGE as FILE.\n"
                 "# mark N TIME: moment N marked (Ctrl+Alt+M until 2026-11-12); the image shown then is saved too.\n"
                 "# resume TIME: capture started again; image numbers start again from 1.\n",
                 about.c_str(), static_cast<double>(kMinSure * 100.0f),
                 static_cast<double>(Recognizer::kMinSure * 100.0f), static_cast<double>(Recognizer::kMirrorDisagrees),
                 static_cast<double>(kKindsClose));
}

LiveLog::~LiveLog() {
    std::fclose(file_);
}

void LiveLog::looked(const ImageStamp& stamp, const Detection* found, int count) {
    char lines[kLookChars]; // Its look line and kinds line, added together so no other line comes between.
    const std::size_t length =
        writeLook(lines, static_cast<unsigned long long>(stamp.number), msSince(stamp.shownQpc),
                  std::span<const Detection>(found, static_cast<std::size_t>(count)));
    add(lines, static_cast<int>(length));
}

void LiveLog::mirrored(const ImageStamp& stamp, float disagreement, float closestKinds) {
    char line[64];
    const int length =
        closestKinds < 0.0f
            ? std::snprintf(line, sizeof(line), "mirror %llu %.3f\n", static_cast<unsigned long long>(stamp.number),
                            static_cast<double>(disagreement))
            : std::snprintf(line, sizeof(line), "mirror %llu %.3f %.3f\n",
                            static_cast<unsigned long long>(stamp.number), static_cast<double>(disagreement),
                            static_cast<double>(closestKinds));
    add(line, length);
}

void LiveLog::saved(const ImageStamp& stamp, const char* name) {
    char line[128];
    const int length = std::snprintf(line, sizeof(line), "saved %s %llu %.1f\n", name,
                                     static_cast<unsigned long long>(stamp.number), msSince(stamp.shownQpc));
    add(line, length);
}

void LiveLog::mark(unsigned number) {
    char line[64];
    const int length = std::snprintf(line, sizeof(line), "mark %u %.1f\n", number, msSince(qpcNow()));
    std::scoped_lock lock(mutex_);
    std::fwrite(line, 1, static_cast<std::size_t>(length), file_);
    std::fflush(file_); // So a mark is on the disk even if observex stops unexpectedly.
}

void LiveLog::resumed() {
    char line[64];
    const int length = std::snprintf(line, sizeof(line), "resume %.1f\n", msSince(qpcNow()));
    add(line, length);
}

double LiveLog::msSince(std::int64_t qpc) const {
    return static_cast<double>(qpc - startQpc_) * msPerTick_;
}

void LiveLog::add(const char* line, int length) {
    std::scoped_lock lock(mutex_);
    std::fwrite(line, 1, static_cast<std::size_t>(length), file_);
}

} // namespace observex
