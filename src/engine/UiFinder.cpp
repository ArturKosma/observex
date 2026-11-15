#include "engine/UiFinder.h"

#include "core/StatWindow.h"
#include "engine/GreyMaker.h"
#include "heuristics/Miniatures.h"
#include "heuristics/TextOcr.h"

#include <algorithm>
#include <cmath>
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

// The words in `area` of `grey`, as light letters on a dark ground (the game's tooltips): turned over (dark letters on
// light read best), levels stretched so letters are black and the dark ground white, doubled smoothly (small letters),
// with a white margin, then Windows OCR (`picture` is its buffer).
std::string readLightText(const GreyLevel& grey, const StatsArea& area, TextOcr& ocr,
                          std::vector<std::uint8_t>& picture) {
    constexpr int kScale = 2;
    constexpr int kMargin = 12;
    constexpr float kGround = 100.0f; // At most this light: ground (white).
    constexpr float kLetter = 190.0f; // At least this light: letter (black).
    const int cw = area.x1 - area.x0;
    const int ch = area.y1 - area.y0;
    if (cw <= 1 || ch <= 1) {
        return {};
    }
    const int w = cw * kScale + 2 * kMargin;
    const int h = ch * kScale + 2 * kMargin;
    picture.assign(static_cast<std::size_t>(w) * static_cast<std::size_t>(h), 255);
    const auto at = [&](int x, int y) {
        return static_cast<float>(grey.pixels[static_cast<std::ptrdiff_t>(area.y0 + y) * grey.stride + area.x0 + x]);
    };
    for (int y = 0; y < ch * kScale; ++y) {
        const float s = std::clamp((static_cast<float>(y) + 0.5f) / kScale - 0.5f, 0.0f, static_cast<float>(ch - 1));
        const int s0 = static_cast<int>(s);
        const int s1 = std::min(s0 + 1, ch - 1);
        const float fs = s - static_cast<float>(s0);
        std::uint8_t* row =
            picture.data() + static_cast<std::size_t>(y + kMargin) * static_cast<std::size_t>(w) + kMargin;
        for (int x = 0; x < cw * kScale; ++x) {
            const float u =
                std::clamp((static_cast<float>(x) + 0.5f) / kScale - 0.5f, 0.0f, static_cast<float>(cw - 1));
            const int u0 = static_cast<int>(u);
            const int u1 = std::min(u0 + 1, cw - 1);
            const float fu = u - static_cast<float>(u0);
            const float v = (at(u0, s0) * (1.0f - fu) + at(u1, s0) * fu) * (1.0f - fs) +
                            (at(u0, s1) * (1.0f - fu) + at(u1, s1) * fu) * fs;
            const float light = std::clamp((v - kGround) * 255.0f / (kLetter - kGround), 0.0f, 255.0f);
            row[x] = static_cast<std::uint8_t>(std::lround(255.0f - light));
        }
    }
    std::string error;
    std::string text = ocr.read(picture.data(), w, h, w, &error);
    return text.empty() && !error.empty() ? "(OCR: " + error + ")" : text;
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

std::uint64_t UiFinder::readText(const StatsArea& area) {
    {
        const std::scoped_lock lock(textMutex_);
        textArea_ = area;
    }
    return textAsked_.fetch_add(1, std::memory_order_acq_rel) + 1;
}

const UiText& UiFinder::text() {
    text_.fetch();
    return text_.readSlot();
}

const UiSkillPictures& UiFinder::skillPictures() {
    skillPictures_.fetch();
    return skillPictures_.readSlot();
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
    // The Debug tab's steps, in the order they run on each image.
    enum Step { kGrey, kFind, kCopyParts, kMiniatures, kReadParts, kStats, kPictures, kText };
    CostMeter meter({"Grey image, read back", "Finding", "Parts copied", "Miniatures", "Parts read back",
                     "Stats reading", "Skill pictures", "Text read (OCR)"});
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
        stats.costs = meter.take(static_cast<double>(elapsed) * msPerTick / 1000.0);
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
    constexpr int kMaxParts = 12; // The pieces' (6 or so), a name plate's, the minimap and the messages.
    std::array<Staging, kMaxParts> staging;
    std::array<StatsArea, kMaxParts> areas{};
    std::array<ColourPart, kMaxParts> parts{};
    // The miniatures asked for (Add Skill) come back the same way, side by side in one texture.
    ComPtr<ID3D11Texture2D> miniStaging;
    std::uint64_t miniaturesAnswered = 0;
    std::vector<StatsArea> miniFound(256);

    // Text asked for (readText) is read here, by Windows OCR made on this thread when first needed.
    TextOcr ocr;
    std::uint64_t textAnswered = 0;
    std::vector<std::uint8_t> textPicture;

    const GreyArea whole{0, 0, static_cast<int>(width_), static_cast<int>(height_)};
    ImageStamp stamp;
    while (!stop.stop_requested()) {
        if (reader_->waitForImage(kWaitMs)) {
            const std::int64_t start = qpcNow();
            meter.begin();
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
                meter.lap(kGrey);
                UiPieces& pieces = found_.writeSlot();
                pieces.count = layout->find(grey->levels[0], pieces.pieces.data(), UiPieces::kMax);
                pieces.image = stamp.number;
                const std::int64_t found = qpcNow();
                meter.lap(kFind);

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
                {
                    const std::scoped_lock lock(lookMutex_);
                    statsReader->setLook(look_);
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
                meter.lap(kCopyParts);
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
                    meter.lap(kMiniatures);
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
                meter.lap(kReadParts);
                GameStats& stats = gameStats_.writeSlot();
                statsReader->read(on, {parts.data(), static_cast<std::size_t>(mapped)}, stats);
                meter.lap(kStats);
                // The skills seen ready (their key read), their pictures copied while the parts are mapped.
                UiSkillPictures& ready = skillPictures_.writeSlot();
                ready.image = stamp.number;
                ready.count = 0;
                for (int g = 0; g < stats.count && ready.count < UiSkillPictures::kMax; ++g) {
                    const GameGauge& gauge = stats.gauges[static_cast<std::size_t>(g)];
                    const StatsArea& at = gauge.at;
                    const int side = at.x1 - at.x0;
                    if (std::strcmp(gauge.piece.data(), kSkillsPiece) != 0 || gauge.key[0] == '\0' || side <= 0 ||
                        side > UiSkillPictures::kSide || at.y1 - at.y0 != side) {
                        continue;
                    }
                    const auto part = std::find_if(parts.begin(), parts.begin() + mapped, [&](const ColourPart& p) {
                        return p.area.x0 <= at.x0 && p.area.y0 <= at.y0 && p.area.x1 >= at.x1 && p.area.y1 >= at.y1;
                    });
                    if (part == parts.begin() + mapped) {
                        continue;
                    }
                    UiSkillPictures::Picture& picture = ready.pictures[static_cast<std::size_t>(ready.count++)];
                    picture.name = gauge.name;
                    picture.key = gauge.key;
                    picture.at = at;
                    for (int y = 0; y < side; ++y) {
                        std::memcpy(picture.bgra.data() + static_cast<std::size_t>(y * side) * 4,
                                    part->bgra + static_cast<std::size_t>(at.y0 + y - part->area.y0) * part->pitch +
                                        static_cast<std::size_t>(at.x0 - part->area.x0) * 4,
                                    static_cast<std::size_t>(side) * 4);
                    }
                }
                skillPictures_.publish();
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
                meter.lap(kPictures);
                // Text asked for: read off the grey image (still this image's), after the readings went out.
                if (const std::uint64_t textAsked = textAsked_.load(std::memory_order_acquire);
                    textAsked != textAnswered) {
                    textAnswered = textAsked;
                    StatsArea area;
                    {
                        const std::scoped_lock lock(textMutex_);
                        area = textArea_;
                    }
                    const GreyLevel& level = grey->levels[0];
                    area = {std::clamp(area.x0, 0, level.width), std::clamp(area.y0, 0, level.height),
                            std::clamp(area.x1, 0, level.width), std::clamp(area.y1, 0, level.height)};
                    const std::int64_t textStart = qpcNow();
                    const std::string words = readLightText(level, area, ocr, textPicture);
                    UiText& answer = text_.writeSlot();
                    answer.request = textAsked;
                    answer.image = stamp.number;
                    answer.shownQpc = stamp.shownQpc;
                    answer.area = area;
                    answer.ms = static_cast<float>(static_cast<double>(qpcNow() - textStart) * msPerTick);
                    std::snprintf(answer.text.data(), answer.text.size(), "%s", words.c_str());
                    text_.publish();
                    meter.lap(kText);
                }
                meter.end();
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

void UiFinder::setLook(const PlateLook& look) {
    const std::scoped_lock lock(lookMutex_);
    look_ = look;
}

void UiFinder::fail(std::string message) {
    error_ = std::move(message);
    state_.store(State::Failed, std::memory_order_release);
}

} // namespace observex
