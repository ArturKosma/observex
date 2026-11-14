#include "engine/UiFinder.h"

#include "core/StatWindow.h"
#include "engine/GreyMaker.h"
#include "heuristics/Miniatures.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <vector>
#include <exception>
#include <system_error>
#include <utility>

namespace observex {

namespace {

// Longest wait for a new image. Also how quickly the thread notices a stop request.
constexpr unsigned kWaitMs = 50;

std::int64_t qpcNow() {
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return now.QuadPart;
}

// When the layout's files last changed: the newest of ui.txt, the pictures and the folder itself (which changes when
// a file is added or removed).
std::filesystem::file_time_type lastChange(const std::filesystem::path& folder) {
    namespace fs = std::filesystem;
    std::error_code error;
    fs::file_time_type newest = fs::last_write_time(folder, error);
    if (error) {
        return {};
    }
    for (fs::directory_iterator it(folder, error), end; !error && it != end; it.increment(error)) {
        std::error_code fileError;
        const fs::file_time_type time = it->last_write_time(fileError);
        if (!fileError) {
            newest = std::max(newest, time);
        }
    }
    return newest;
}

} // namespace

UiFinder::UiFinder(IDXGIAdapter1* gpu, ImageChannel& channel, std::filesystem::path folder,
                   std::shared_ptr<const StatsSource> stats)
    : folder_(std::move(folder)), statsSource_(stats ? std::move(stats) : std::make_shared<const StatsSource>()),
      width_(channel.width()), height_(channel.height()) {
    gpu_ = createGpuDevice(findOwnAdapter(gpu).Get()); // An adapter object of its own: cannot hold up the others.
    reader_ = std::make_unique<ImageReader>(gpu_.device.Get(), channel);
    thread_ = std::jthread([this](std::stop_token stop) { run(stop); });
}

UiFinder::~UiFinder() {
    if (thread_.joinable()) {
        thread_.request_stop();
        thread_.join();
    }
}

const UiPieces& UiFinder::found() {
    found_.fetch();
    return found_.readSlot();
}

const UiFinderStats& UiFinder::stats() {
    stats_.fetch();
    return stats_.readSlot();
}

const GameStats& UiFinder::gameStats() {
    gameStats_.fetch();
    return gameStats_.readSlot();
}

std::uint64_t UiFinder::findMiniatures() { return miniaturesAsked_.fetch_add(1, std::memory_order_acq_rel) + 1; }

const UiMiniatures& UiFinder::miniatures() {
    miniatures_.fetch();
    return miniatures_.readSlot();
}

void UiFinder::run(std::stop_token stop) {
    // Below the others: the interface moves only when the user moves it, so a late find costs nothing.
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    SetThreadDescription(GetCurrentThread(), L"observex ui finder");

    std::unique_ptr<GreyMaker> greys;
    try {
        greys = std::make_unique<GreyMaker>(gpu_.device.Get(), width_, height_, 1); // Only the newest.
    } catch (const std::exception& e) {
        fail(e.what());
        return;
    }

    // The layout and the bars to read, read again whenever their files change.
    auto layout = std::make_unique<UiLayout>(); // Empty: only framed windows.
    bool loaded = false;
    std::string problem;
    std::unique_ptr<StatsReading> statsReader; // Made below.
    bool statsLoaded = false;
    std::string statsProblem;
    std::uint64_t statsVersion = statsSource_->version();
    // The profile's skills, as last given (setSkills), handed to every reader made.
    std::vector<SkillToFind> skills;
    std::uint64_t skillsVersion = 0;
    const auto giveSkills = [&] {
        std::vector<SkillPicture> pictures(skills.size());
        for (std::size_t i = 0; i < skills.size(); ++i) {
            const SkillToFind& skill = skills[i];
            std::snprintf(pictures[i].name.data(), pictures[i].name.size(), "%s", skill.name.c_str());
            pictures[i].cooldown = skill.cooldown;
            pictures[i].side = skill.side;
            pictures[i].grey = skill.grey.data();
        }
        statsReader->setSkills(pictures);
    };
    // A new reader (rebuilt rules, or stats.txt changed); while stats.txt cannot be read, the one before goes on.
    const auto makeReader = [&] {
        std::string why;
        std::unique_ptr<StatsReading> fresh = statsSource_->make(folder_, &why);
        if (why.empty() || !statsReader) {
            statsReader = std::move(fresh);
            giveSkills();
        }
        if (why.empty()) {
            statsLoaded = true;
            statsProblem.clear();
        } else {
            statsProblem = (statsLoaded ? "stats.txt changed and could not be read, so the one before goes on: "
                                        : "No numbers are read: ") +
                           why;
        }
    };
    std::filesystem::file_time_type layoutChange = lastChange(folder_);
    const auto readLayout = [&] {
        auto fresh = std::make_unique<UiLayout>();
        std::string why;
        if (fresh->load(folder_, &why)) {
            layout = std::move(fresh);
            loaded = true;
            problem.clear();
        } else {
            problem = (loaded ? "ui.txt changed and could not be read, so the one before goes on: "
                              : "Only framed windows are found: ") +
                      why;
        }
        makeReader();
    };
    readLayout();

    LARGE_INTEGER frequency;
    QueryPerformanceFrequency(&frequency);
    const double msPerTick = 1000.0 / static_cast<double>(frequency.QuadPart);
    StatWindow work;
    StatWindow finding;
    StatWindow reading;
    std::int64_t windowStart = qpcNow();
    const auto publishStats = [&](std::int64_t elapsed) {
        const StatWindow::Summary workSummary = work.take();
        const StatWindow::Summary findSummary = finding.take();
        const StatWindow::Summary readSummary = reading.take();
        UiFinderStats& stats = stats_.writeSlot();
        stats.imagesPerSecond =
            elapsed > 0 ? static_cast<float>(workSummary.count / (static_cast<double>(elapsed) * msPerTick / 1000.0))
                        : 0.0f;
        stats.workAvgMs = workSummary.average;
        stats.workMaxMs = workSummary.max;
        stats.findAvgMs = findSummary.average;
        stats.statsAvgMs = readSummary.average;
        std::snprintf(stats.problem.data(), stats.problem.size(), "%s", problem.c_str());
        std::snprintf(stats.statsProblem.data(), stats.statsProblem.size(), "%s", statsProblem.c_str());
        stats_.publish();
    };
    publishStats(0); // So a problem with ui.txt shows at once.
    state_.store(State::Running, std::memory_order_release);

    // The parts the stats reader reads come back in colour through textures the CPU can read, one a part, made (or
    // made larger) when a part needs it.
    struct Staging {
        ComPtr<ID3D11Texture2D> texture;
        unsigned width = 0;
        unsigned height = 0;
    };
    constexpr int kMaxParts = 8;
    std::array<Staging, kMaxParts> staging;
    std::array<StatsArea, kMaxParts> areas{};
    std::array<ColourPart, kMaxParts> parts{};
    // The miniatures asked for (Add Skill) come back the same way, side by side in one texture.
    ComPtr<ID3D11Texture2D> miniStaging;
    std::uint64_t miniaturesAnswered = 0;
    std::vector<StatsArea> miniFound(256);

    const GreyArea whole{0, 0, static_cast<int>(width_), static_cast<int>(height_)};
    ImageStamp stamp;
    while (!stop.stop_requested()) {
        if (reader_->waitForImage(kWaitMs)) {
            const std::int64_t start = qpcNow();
            if (ID3D11Texture2D* image = reader_->useNewImage(&stamp)) {
                // Its grey image, and only the full-size level comes back.
                const GreyView* grey = nullptr;
                try {
                    greys->make(gpu_.context.Get(), stamp.number, image);
                    greys->read(gpu_.context.Get(), stamp.number, {&whole, 1}, 1);
                    greys->wait(gpu_.context.Get());
                    grey = greys->find(stamp.number);
                } catch (const std::exception&) {
                    reader_->endUse();
                    fail("The GPU stopped making grey images (it may have been reset)");
                    return;
                }
                const std::int64_t read = qpcNow();
                UiPieces& pieces = found_.writeSlot();
                pieces.count = layout->find(grey->levels[0], pieces.pieces.data(), UiPieces::kMax);
                pieces.image = stamp.number;
                const std::int64_t found = qpcNow();

                // Then the parts of the same image the numbers are in, in colour; the image goes back to the channel
                // once they are copied. Read by the newest rules (the DLL rebuilt since the last image: a new reader).
                if (const std::uint64_t version = statsSource_->version(); version != statsVersion) {
                    statsVersion = version;
                    makeReader();
                }
                if (const std::uint64_t version = skillsVersion_.load(std::memory_order_acquire);
                    version != skillsVersion) {
                    skillsVersion = version;
                    {
                        const std::scoped_lock lock(skillsMutex_);
                        skills = skills_;
                    }
                    giveSkills();
                }
                const std::span<const UiPiece> on(pieces.pieces.data(), static_cast<std::size_t>(pieces.count));
                const int count = statsReader->areas(on, static_cast<int>(width_), static_cast<int>(height_),
                                                     areas.data(), kMaxParts);
                D3D11_TEXTURE2D_DESC imageDesc{};
                image->GetDesc(&imageDesc);
                int copied = 0;
                for (int i = 0; i < count; ++i) {
                    const StatsArea& area = areas[static_cast<std::size_t>(i)];
                    Staging& part = staging[static_cast<std::size_t>(i)];
                    const auto w = static_cast<unsigned>(area.x1 - area.x0);
                    const auto h = static_cast<unsigned>(area.y1 - area.y0);
                    if (part.texture == nullptr || part.width < w || part.height < h) {
                        D3D11_TEXTURE2D_DESC desc{};
                        desc.Width = std::max(w, part.width);
                        desc.Height = std::max(h, part.height);
                        desc.MipLevels = 1;
                        desc.ArraySize = 1;
                        desc.Format = imageDesc.Format;
                        desc.SampleDesc.Count = 1;
                        desc.Usage = D3D11_USAGE_STAGING;
                        desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
                        part.texture.Reset();
                        if (FAILED(gpu_.device->CreateTexture2D(&desc, nullptr, &part.texture))) {
                            part = {};
                            break;
                        }
                        part.width = desc.Width;
                        part.height = desc.Height;
                    }
                    const D3D11_BOX box{static_cast<UINT>(area.x0), static_cast<UINT>(area.y0), 0,
                                        static_cast<UINT>(area.x1), static_cast<UINT>(area.y1), 1};
                    gpu_.context->CopySubresourceRegion(part.texture.Get(), 0, 0, 0, 0, image, 0, &box);
                    copied = i + 1;
                }
                // Asked for the miniatures: every one on the image, the hotbar's first, then the other pieces'.
                const std::uint64_t miniaturesAsked = miniaturesAsked_.load(std::memory_order_acquire);
                int miniCount = 0;
                std::array<StatsArea, UiMiniatures::kMax> minis{};
                if (miniaturesAsked != miniaturesAnswered) {
                    const GreyLevel& level = grey->levels[0];
                    const int miniTotal = observex::findMiniatures(level.pixels, level.stride, level.width, level.height,
                                                     {0, 0, level.width, level.height}, miniFound.data(),
                                                     static_cast<int>(miniFound.size()));
                    const auto rank = [&](const StatsArea& box) {
                        int best = 2;
                        for (const UiPiece& piece : on) {
                            if (box.x0 >= piece.x0 && box.y0 >= piece.y0 && box.x1 <= piece.x1 && box.y1 <= piece.y1) {
                                best = std::min(best, std::strcmp(piece.name.data(), kSkillsPiece) == 0 ? 0 : 1);
                            }
                        }
                        return best;
                    };
                    std::stable_sort(miniFound.begin(), miniFound.begin() + miniTotal,
                                     [&](const StatsArea& a, const StatsArea& b) { return rank(a) < rank(b); });
                    if (miniStaging == nullptr) {
                        D3D11_TEXTURE2D_DESC desc{};
                        desc.Width = UiMiniatures::kMax * UiMiniatures::kSide;
                        desc.Height = UiMiniatures::kSide;
                        desc.MipLevels = 1;
                        desc.ArraySize = 1;
                        desc.Format = imageDesc.Format;
                        desc.SampleDesc.Count = 1;
                        desc.Usage = D3D11_USAGE_STAGING;
                        desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
                        if (FAILED(gpu_.device->CreateTexture2D(&desc, nullptr, &miniStaging))) {
                            miniStaging.Reset();
                        }
                    }
                    for (int i = 0; i < miniTotal && miniCount < UiMiniatures::kMax && miniStaging != nullptr; ++i) {
                        const StatsArea& at = miniFound[static_cast<std::size_t>(i)];
                        if (at.x1 - at.x0 > UiMiniatures::kSide || at.y1 - at.y0 > UiMiniatures::kSide) {
                            continue;
                        }
                        const D3D11_BOX box{static_cast<UINT>(at.x0), static_cast<UINT>(at.y0), 0,
                                            static_cast<UINT>(at.x1), static_cast<UINT>(at.y1), 1};
                        gpu_.context->CopySubresourceRegion(miniStaging.Get(), 0,
                                                            static_cast<UINT>(miniCount * UiMiniatures::kSide), 0, 0,
                                                            image, 0, &box);
                        minis[static_cast<std::size_t>(miniCount++)] = at;
                    }
                }
                reader_->endUse();
                if (miniaturesAsked != miniaturesAnswered) {
                    miniaturesAnswered = miniaturesAsked;
                    UiMiniatures& answer = miniatures_.writeSlot();
                    answer.request = miniaturesAsked;
                    answer.image = stamp.number;
                    answer.count = 0;
                    D3D11_MAPPED_SUBRESOURCE map{};
                    if (miniCount > 0 && SUCCEEDED(gpu_.context->Map(miniStaging.Get(), 0, D3D11_MAP_READ, 0, &map))) {
                        for (int y = 0; y < UiMiniatures::kSide; ++y) {
                            std::memcpy(answer.bgra.data() + static_cast<std::size_t>(y) * UiMiniatures::kPitch,
                                        static_cast<const std::uint8_t*>(map.pData) + static_cast<std::size_t>(y) *
                                                                                          map.RowPitch,
                                        UiMiniatures::kPitch);
                        }
                        gpu_.context->Unmap(miniStaging.Get(), 0);
                        answer.count = miniCount;
                        answer.boxes = minis;
                    }
                    miniatures_.publish();
                }
                int mapped = 0;
                for (int i = 0; i < copied; ++i) {
                    D3D11_MAPPED_SUBRESOURCE map{};
                    if (FAILED(gpu_.context->Map(staging[static_cast<std::size_t>(i)].texture.Get(), 0, D3D11_MAP_READ,
                                                 0, &map))) {
                        break;
                    }
                    parts[static_cast<std::size_t>(i)] = {areas[static_cast<std::size_t>(i)],
                                                          static_cast<const std::uint8_t*>(map.pData), map.RowPitch};
                    mapped = i + 1;
                }
                GameStats& stats = gameStats_.writeSlot();
                statsReader->read(on, {parts.data(), static_cast<std::size_t>(mapped)}, stats);
                for (int i = 0; i < mapped; ++i) {
                    gpu_.context->Unmap(staging[static_cast<std::size_t>(i)].texture.Get(), 0);
                }
                if (!stats.hasTarget) {
                    // Why the target's window was not found: its anchors' nearest places (a clip's why lines).
                    const std::string whyNot = layout->whyNot(kTargetPiece);
                    if (!whyNot.empty()) {
                        const std::size_t used = strnlen(stats.why.data(), stats.why.size());
                        std::snprintf(stats.why.data() + used, stats.why.size() - used, "%s(%s)",
                                      used > 0 ? " " : "", whyNot.c_str());
                    }
                }
                const std::int64_t done = qpcNow();
                stats.image = stamp.number;
                stats.shownQpc = stamp.shownQpc;
                stats.readMs = static_cast<float>(static_cast<double>(done - found) * msPerTick);
                found_.publish();
                gameStats_.publish();
                work.add(static_cast<float>(static_cast<double>(done - start) * msPerTick));
                finding.add(static_cast<float>(static_cast<double>(found - read) * msPerTick));
                reading.add(stats.readMs);
            }
        }

        // Once a second: ui.txt read again if it changed, and the numbers to the UI (lock-free).
        const std::int64_t now = qpcNow();
        const std::int64_t elapsed = now - windowStart;
        if (elapsed >= frequency.QuadPart) {
            if (const std::filesystem::file_time_type change = lastChange(folder_); change != layoutChange) {
                layoutChange = change;
                readLayout();
            }
            publishStats(elapsed);
            windowStart = now;
        }
    }
}

void UiFinder::setSkills(std::vector<SkillToFind> skills) {
    {
        const std::scoped_lock lock(skillsMutex_);
        skills_ = std::move(skills);
    }
    skillsVersion_.fetch_add(1, std::memory_order_acq_rel);
}

void UiFinder::fail(std::string message) {
    error_ = std::move(message);
    state_.store(State::Failed, std::memory_order_release);
}

} // namespace observex
