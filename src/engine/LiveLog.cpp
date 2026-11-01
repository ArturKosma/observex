#include "engine/LiveLog.h"

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
                 "# saved FILE IMAGE TIME: the recording saved screen image number IMAGE as FILE.\n"
                 "# mark N TIME: the user pressed Ctrl+Alt+M (mark N); the image shown then is saved too.\n"
                 "# resume TIME: capture started again; image numbers start again from 1.\n",
                 about.c_str(), static_cast<double>(kMinSure * 100.0f),
                 static_cast<double>(Recognizer::kMinSure * 100.0f));
}

LiveLog::~LiveLog() {
    std::fclose(file_);
}

void LiveLog::looked(const ImageStamp& stamp, const Detection* found, int count) {
    char line[4096];
    int length = std::snprintf(line, sizeof(line), "look %llu %.1f", static_cast<unsigned long long>(stamp.number),
                               msSince(stamp.shownQpc));
    for (int i = 0; i < count && length < static_cast<int>(sizeof(line)) - 64; ++i) {
        const Detection& thing = found[i];
        length += std::snprintf(line + length, sizeof(line) - static_cast<std::size_t>(length), " %.2f %.0f %.0f %.0f %.0f",
                                static_cast<double>(thing.sure), static_cast<double>(thing.x),
                                static_cast<double>(thing.y), static_cast<double>(thing.width),
                                static_cast<double>(thing.height));
    }
    line[length++] = '\n';
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
