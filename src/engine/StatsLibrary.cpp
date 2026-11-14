#include "engine/StatsLibrary.h"

#include "heuristics/StatsAbi.h"
#include "heuristics/StatsReader.h"

#include <windows.h>

#include <array>
#include <cstdio>
#include <ctime>
#include <exception>
#include <stdexcept>
#include <system_error>
#include <utility>

namespace observex {

namespace fs = std::filesystem;

namespace {

std::string utf8(const fs::path& path) {
    const std::u8string text = path.u8string();
    return {reinterpret_cast<const char*>(text.data()), text.size()};
}

// Where the copies go: one folder in the temp folder (not the heuristics' own), a name per process and load.
fs::path copyFolder() {
    std::error_code error;
    const fs::path temp = fs::temp_directory_path(error);
    return (error ? fs::path(L".") : temp) / L"observex-stats";
}

// A reader made by a loaded DLL: its pointer, called through the DLL's functions. A failure inside reads nothing.
class DllStatsReading final : public StatsReading {
public:
    DllStatsReading(std::shared_ptr<const StatsLibrary> library, const ObservexStatsApi& api, void* reader)
        : library_(std::move(library)), api_(api), reader_(reader) {}
    ~DllStatsReading() override { api_.destroy(reader_); }

    DllStatsReading(const DllStatsReading&) = delete;
    DllStatsReading& operator=(const DllStatsReading&) = delete;

    int areas(std::span<const UiPiece> pieces, int width, int height, StatsArea* out, int max) const override {
        const int count =
            api_.areas(reader_, pieces.data(), static_cast<int>(pieces.size()), width, height, out, max);
        return count < 0 ? 0 : count;
    }
    void read(std::span<const UiPiece> pieces, std::span<const ColourPart> parts, GameStats& out) override {
        if (!api_.read(reader_, pieces.data(), static_cast<int>(pieces.size()), parts.data(),
                       static_cast<int>(parts.size()), &out)) {
            out.count = 0;
        }
    }
    void setSkills(std::span<const SkillPicture> skills) override {
        api_.setSkills(reader_, skills.data(), static_cast<int>(skills.size()));
    }

private:
    std::shared_ptr<const StatsLibrary> library_; // Keeps the DLL loaded while it lives.
    const ObservexStatsApi& api_;
    void* reader_;
};

// The rules built into the app, by `folder`'s stats.txt.
std::unique_ptr<StatsReading> builtIn(const fs::path& folder, std::string* error) {
    auto reader = std::make_unique<StatsReader>();
    std::string why;
    if (!reader->load(folder, &why) && error != nullptr) {
        *error = why;
    }
    return reader;
}

} // namespace

std::shared_ptr<StatsLibrary> StatsLibrary::load(const fs::path& file) {
    std::error_code error;
    const fs::file_time_type written = fs::last_write_time(file, error);
    if (error) {
        throw std::runtime_error("There is no " + utf8(file.filename()) + " in " + utf8(file.parent_path()));
    }
    // A copy is loaded, so the build can write the file meanwhile (Windows locks a loaded DLL's file).
    static std::atomic<unsigned> loads{0};
    const fs::path folder = copyFolder();
    fs::create_directories(folder, error);
    const fs::path copy = folder / (std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(++loads) + L".dll");
    if (!CopyFileW(file.c_str(), copy.c_str(), FALSE)) {
        throw std::runtime_error("Could not copy " + utf8(file.filename()) + " to load it (error " +
                                 std::to_string(GetLastError()) + ")");
    }
    std::shared_ptr<StatsLibrary> library(new StatsLibrary());
    library->file_ = file;
    library->copy_ = copy;
    library->written_ = written;
    library->module_ = LoadLibraryW(copy.c_str());
    if (library->module_ == nullptr) {
        throw std::runtime_error("Could not load " + utf8(file.filename()) + " (error " +
                                 std::to_string(GetLastError()) + ")");
    }
    const auto entry = reinterpret_cast<ObservexStatsEntry>(
        reinterpret_cast<void*>(GetProcAddress(static_cast<HMODULE>(library->module_), kObservexStatsEntry)));
    const ObservexStatsApi* api = entry != nullptr ? entry() : nullptr;
    if (api == nullptr) {
        throw std::runtime_error(utf8(file.filename()) + " is not observex's stats reading (no " +
                                 kObservexStatsEntry + ")");
    }
    if (api->abi != kObservexStatsAbi || api->pieceSize != sizeof(UiPiece) || api->areaSize != sizeof(StatsArea) ||
        api->partSize != sizeof(ColourPart) || api->statsSize != sizeof(GameStats) ||
        api->skillSize != sizeof(SkillPicture)) {
        throw std::runtime_error(utf8(file.filename()) +
                                 " was built for another observex: rebuild the app too (observex)");
    }
    library->api_ = api;
    return library;
}

StatsLibrary::~StatsLibrary() {
    if (module_ != nullptr) {
        FreeLibrary(static_cast<HMODULE>(module_));
    }
    std::error_code error;
    fs::remove(copy_, error);
}

std::unique_ptr<StatsReading> StatsLibrary::make(const fs::path& folder, std::string* error) const {
    std::array<char, 512> why{};
    void* reader = api_->create(utf8(folder).c_str(), why.data(), static_cast<std::uint32_t>(why.size()));
    if (reader == nullptr) {
        throw std::runtime_error(utf8(file_.filename()) + " could not make its reader");
    }
    if (error != nullptr) {
        *error = why.data();
    }
    return std::make_unique<DllStatsReading>(shared_from_this(), *api_, reader);
}

StatsSource::StatsSource(fs::path file) : file_(std::move(file)) {
    // Copies left by an app that did not get to delete them (one still running keeps its own: those cannot go).
    std::error_code error;
    for (const fs::directory_entry& entry : fs::directory_iterator(copyFolder(), error)) {
        std::error_code removeError;
        fs::remove(entry.path(), removeError);
    }
    if (const std::optional<Stamp> stamp = stampOf()) {
        load(*stamp);
    } else {
        error_ = "There is no " + utf8(file_.filename()) + " next to the app";
    }
}

std::optional<StatsSource::Stamp> StatsSource::stampOf() const {
    if (file_.empty()) {
        return std::nullopt;
    }
    std::error_code error;
    Stamp stamp;
    stamp.written = fs::last_write_time(file_, error);
    if (error) {
        return std::nullopt;
    }
    stamp.bytes = fs::file_size(file_, error);
    if (error) {
        return std::nullopt;
    }
    return stamp;
}

bool StatsSource::load(Stamp stamp) {
    tried_ = stamp;
    try {
        std::shared_ptr<StatsLibrary> library = StatsLibrary::load(file_);
        {
            const std::scoped_lock lock(mutex_);
            library_ = std::move(library);
        }
        version_.fetch_add(1, std::memory_order_acq_rel);
        error_.clear();
        return true;
    } catch (const std::exception& e) {
        error_ = e.what();
        return false;
    }
}

bool StatsSource::poll() {
    const auto now = std::chrono::steady_clock::now();
    if (now - lastPoll_ < kPollMs) {
        return false;
    }
    lastPoll_ = now;
    const std::optional<Stamp> stamp = stampOf();
    if (!stamp || stamp == tried_) { // Gone (a clean build): the loaded one stays.
        pending_.reset();
        return false;
    }
    if (pending_ != stamp) {
        pending_ = stamp;
        pendingSince_ = now;
        return false;
    }
    if (now - pendingSince_ < kSettleMs) {
        return false;
    }
    pending_.reset();
    return load(*stamp);
}

std::unique_ptr<StatsReading> StatsSource::make(const fs::path& folder, std::string* error) const {
    std::shared_ptr<StatsLibrary> library;
    {
        const std::scoped_lock lock(mutex_);
        library = library_;
    }
    if (library) {
        try {
            return library->make(folder, error);
        } catch (const std::exception& e) {
            std::unique_ptr<StatsReading> reader = builtIn(folder, error);
            if (error != nullptr && error->empty()) {
                *error = std::string(e.what()) + ", so the rules built into the app read";
            }
            return reader;
        }
    }
    return builtIn(folder, error);
}

std::string StatsSource::describe() const {
    std::shared_ptr<StatsLibrary> library;
    {
        const std::scoped_lock lock(mutex_);
        library = library_;
    }
    if (!library) {
        return "built into the app";
    }
    // "observex_stats.dll, built 13:05:22" today, else with the day.
    const auto written = std::chrono::clock_cast<std::chrono::system_clock>(library->written());
    const std::time_t time = std::chrono::system_clock::to_time_t(written);
    const std::time_t now = std::time(nullptr);
    std::tm local{};
    std::tm today{};
    localtime_s(&local, &time);
    localtime_s(&today, &now);
    char when[40];
    if (local.tm_year == today.tm_year && local.tm_yday == today.tm_yday) {
        std::snprintf(when, sizeof(when), "%02d:%02d:%02d", local.tm_hour, local.tm_min, local.tm_sec);
    } else {
        std::snprintf(when, sizeof(when), "%04d-%02d-%02d %02d:%02d", local.tm_year + 1900, local.tm_mon + 1,
                      local.tm_mday, local.tm_hour, local.tm_min);
    }
    return utf8(library->file().filename()) + ", built " + when;
}

} // namespace observex
