#include "engine/HeuristicsLibrary.h"

#include "heuristics/Abi.h"
#include "heuristics/GroundMotion.h"
#include "heuristics/Rules.h"
#include "heuristics/SnapRules.h"

#include <windows.h>

#include <cstdio>
#include <ctime>
#include <stdexcept>
#include <system_error>
#include <utility>
#include <vector>

namespace observex {

namespace fs = std::filesystem;

namespace {

std::string utf8(const fs::path& path) {
    const std::u8string text = path.u8string();
    return {reinterpret_cast<const char*>(text.data()), text.size()};
}

// Where the copies go: one folder in the temp folder, a name per process and load.
fs::path copyFolder() {
    std::error_code error;
    const fs::path temp = fs::temp_directory_path(error);
    return (error ? fs::path(L".") : temp) / L"observex-heuristics";
}

// Heuristics made by a loaded DLL: its pointer, called through the DLL's functions.
class DllHeuristics final : public Heuristics {
public:
    DllHeuristics(std::shared_ptr<const HeuristicsLibrary> library, const ObservexHeuristicsApi& api)
        : library_(std::move(library)), api_(api), heuristics_(api.create()) {
        if (heuristics_ == nullptr) {
            throw std::runtime_error("observex_heuristics.dll could not make its heuristics");
        }
    }
    ~DllHeuristics() override { api_.destroy(heuristics_); }

    DllHeuristics(const DllHeuristics&) = delete;
    DllHeuristics& operator=(const DllHeuristics&) = delete;

    int update(double timeMs, std::span<const Detection> found, Tracked* out, int max) override {
        // A failure inside (it ran out of memory) shows nothing on this look rather than stopping the app.
        const int count = api_.update(heuristics_, timeMs, found.data(), static_cast<int>(found.size()), out, max);
        return count < 0 ? 0 : count;
    }
    void clear() override { api_.clear(heuristics_); }

private:
    std::shared_ptr<const HeuristicsLibrary> library_; // Keeps the DLL loaded while these live.
    const ObservexHeuristicsApi& api_;
    void* heuristics_;
};

// More searches than an image has things (engine/Recognizer's Found::kMax, 100).
constexpr std::size_t kMaxFinds = 256;

// Snapping made by a loaded DLL, likewise.
class DllSnapping final : public Snapping {
public:
    DllSnapping(std::shared_ptr<const HeuristicsLibrary> library, const ObservexHeuristicsApi& api)
        : library_(std::move(library)), api_(api), snapping_(api.createSnapping()) {
        if (snapping_ == nullptr) {
            throw std::runtime_error("observex_heuristics.dll could not make its snapping");
        }
    }
    ~DllSnapping() override { api_.destroySnapping(snapping_); }

    DllSnapping(const DllSnapping&) = delete;
    DllSnapping& operator=(const DllSnapping&) = delete;

    void looked(double lookMs, const GreyView* image, std::span<const Tracked> shown) override {
        api_.looked(snapping_, lookMs, image, shown.data(), static_cast<int>(shown.size()));
    }
    int snap(double timeMs, const GreyView& image, Tracked* out, int max) override {
        const int count = api_.snap(snapping_, timeMs, &image, out, max);
        return count < 0 ? 0 : count;
    }
    // If they failed, all of the image.
    int learnAreas(std::span<const Tracked> shown, GreyArea* out, int max) override {
        const int count = api_.learnAreas(snapping_, shown.data(), static_cast<int>(shown.size()), out, max);
        return count >= 0 ? count : all(out, max);
    }
    int snapAreas(double timeMs, bool look, double lookMs, std::span<const Tracked> shown, GreyArea* out,
                  int max) override {
        const int count =
            api_.snapAreas(snapping_, timeMs, look, lookMs, shown.data(), static_cast<int>(shown.size()), out, max);
        return count >= 0 ? count : all(out, max);
    }
    double leadMs() const override { return api_.snapLeadMs(snapping_); }
    std::span<const SnapFind> finds() const override {
        finds_.resize(kMaxFinds);
        const int count = api_.finds(snapping_, finds_.data(), static_cast<int>(finds_.size()));
        return {finds_.data(), static_cast<std::size_t>(count > 0 ? count : 0)};
    }
    void replay(std::span<const SnapFind> recorded) override {
        api_.replay(snapping_, recorded.data(), static_cast<int>(recorded.size()));
    }
    void clear() override { api_.clearSnapping(snapping_); }

private:
    static int all(GreyArea* out, int max) {
        if (max < 1) {
            return 0;
        }
        constexpr int kFar = 1 << 28; // Past any screen's edges.
        out[0] = {-kFar, -kFar, kFar, kFar};
        return 1;
    }

    std::shared_ptr<const HeuristicsLibrary> library_;
    const ObservexHeuristicsApi& api_;
    void* snapping_;
    mutable std::vector<SnapFind> finds_; // Where finds() copies them to (allocated once).
};

// Following the camera made by a loaded DLL, likewise. A failure inside shows as an image not followed.
class DllGrounding final : public Grounding {
public:
    DllGrounding(std::shared_ptr<const HeuristicsLibrary> library, const ObservexHeuristicsApi& api)
        : library_(std::move(library)), api_(api), ground_(api.createGround()) {
        if (ground_ == nullptr) {
            throw std::runtime_error("observex_heuristics.dll could not make its following of the camera");
        }
    }
    ~DllGrounding() override { api_.destroyGround(ground_); }

    DllGrounding(const DllGrounding&) = delete;
    DllGrounding& operator=(const DllGrounding&) = delete;

    void begin(double timeMs, int width, int height, const GroundHints& hints) override {
        api_.groundBegin(ground_, timeMs, width, height, &hints);
    }
    int firstLevel() const override { return api_.groundFirstLevel(ground_); }
    int fullAreas(GreyArea* out, int max) const override {
        const int count = api_.groundFullAreas(ground_, out, max);
        return count < 0 ? 0 : count;
    }
    bool end(const GreyView& image) override { return api_.groundEnd(ground_, &image) == 1; }
    int headAreas(double timeMs, const GroundHints& hints, GreyArea* out, int max) override {
        const int count = api_.groundHeadAreas(ground_, timeMs, &hints, out, max);
        return count < 0 ? 0 : count;
    }
    bool head(const GreyView& image) override { return api_.groundHead(ground_, &image) == 1; }
    GroundCamera camera() const override {
        GroundCamera camera;
        return api_.groundCamera(ground_, &camera) ? camera : GroundCamera{};
    }
    GroundNote note() const override {
        GroundNote note;
        return api_.groundNote(ground_, &note) ? note : GroundNote{};
    }
    void clear() override { api_.clearGround(ground_); }

private:
    std::shared_ptr<const HeuristicsLibrary> library_;
    const ObservexHeuristicsApi& api_;
    void* ground_;
};

} // namespace

std::shared_ptr<HeuristicsLibrary> HeuristicsLibrary::load(const fs::path& file) {
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
    std::shared_ptr<HeuristicsLibrary> library(new HeuristicsLibrary());
    library->file_ = file;
    library->copy_ = copy;
    library->written_ = written;
    library->module_ = LoadLibraryW(copy.c_str());
    if (library->module_ == nullptr) {
        throw std::runtime_error("Could not load " + utf8(file.filename()) + " (error " +
                                 std::to_string(GetLastError()) + ")");
    }
    const auto entry = reinterpret_cast<ObservexHeuristicsEntry>(
        reinterpret_cast<void*>(GetProcAddress(static_cast<HMODULE>(library->module_), kObservexHeuristicsEntry)));
    const ObservexHeuristicsApi* api = entry != nullptr ? entry() : nullptr;
    if (api == nullptr) {
        throw std::runtime_error(utf8(file.filename()) + " is not observex's heuristics (no " +
                                 kObservexHeuristicsEntry + ")");
    }
    if (api->abi != kObservexHeuristicsAbi || api->detectionSize != sizeof(Detection) ||
        api->trackedSize != sizeof(Tracked) || api->greyViewSize != sizeof(GreyView) ||
        api->greyAreaSize != sizeof(GreyArea) || api->snapFindSize != sizeof(SnapFind) ||
        api->groundCameraSize != sizeof(GroundCamera) || api->groundHintsSize != sizeof(GroundHints) ||
        api->groundNoteSize != sizeof(GroundNote)) {
        throw std::runtime_error(utf8(file.filename()) +
                                 " was built for another observex: rebuild the app too (observex)");
    }
    library->api_ = api;
    return library;
}

HeuristicsLibrary::~HeuristicsLibrary() {
    if (module_ != nullptr) {
        FreeLibrary(static_cast<HMODULE>(module_));
    }
    std::error_code error;
    fs::remove(copy_, error);
}

std::unique_ptr<Heuristics> HeuristicsLibrary::make() const {
    return std::make_unique<DllHeuristics>(shared_from_this(), *api_);
}

std::unique_ptr<Snapping> HeuristicsLibrary::makeSnapping() const {
    return std::make_unique<DllSnapping>(shared_from_this(), *api_);
}

std::unique_ptr<Grounding> HeuristicsLibrary::makeGround() const {
    return std::make_unique<DllGrounding>(shared_from_this(), *api_);
}

HeuristicsSource::HeuristicsSource(fs::path file) : file_(std::move(file)) {
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

std::optional<HeuristicsSource::Stamp> HeuristicsSource::stampOf() const {
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

bool HeuristicsSource::load(Stamp stamp) {
    tried_ = stamp;
    try {
        std::shared_ptr<HeuristicsLibrary> library = HeuristicsLibrary::load(file_);
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

bool HeuristicsSource::poll() {
    const auto now = std::chrono::steady_clock::now();
    if (now - lastPoll_ < kPollMs) {
        return false;
    }
    lastPoll_ = now;
    const std::optional<Stamp> stamp = stampOf();
    if (!stamp || stamp == tried_) { // Gone (a clean build): the loaded ones stay.
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

std::unique_ptr<Heuristics> HeuristicsSource::make() const {
    std::shared_ptr<HeuristicsLibrary> library;
    {
        const std::scoped_lock lock(mutex_);
        library = library_;
    }
    if (library) {
        return library->make();
    }
    return std::make_unique<Rules>();
}

std::unique_ptr<Snapping> HeuristicsSource::makeSnapping() const {
    std::shared_ptr<HeuristicsLibrary> library;
    {
        const std::scoped_lock lock(mutex_);
        library = library_;
    }
    if (library) {
        return library->makeSnapping();
    }
    return std::make_unique<SnapRules>();
}

std::unique_ptr<Grounding> HeuristicsSource::makeGround() const {
    std::shared_ptr<HeuristicsLibrary> library;
    {
        const std::scoped_lock lock(mutex_);
        library = library_;
    }
    if (library) {
        return library->makeGround();
    }
    return std::make_unique<GroundFollower>();
}

std::string HeuristicsSource::describe() const {
    std::shared_ptr<HeuristicsLibrary> library;
    {
        const std::scoped_lock lock(mutex_);
        library = library_;
    }
    if (!library) {
        return "built into the app";
    }
    // "observex_heuristics.dll, built 13:05:22" today, else with the day.
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
