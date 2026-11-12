#include "engine/UiFinder.h"

#include "core/StatWindow.h"
#include "engine/GreyMaker.h"

#include <algorithm>
#include <cstdio>
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

UiFinder::UiFinder(IDXGIAdapter1* gpu, ImageChannel& channel, std::filesystem::path folder)
    : folder_(std::move(folder)), width_(channel.width()), height_(channel.height()) {
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

    // The layout, read again whenever its files change.
    auto layout = std::make_unique<UiLayout>(); // Empty: only framed windows.
    bool loaded = false;
    std::string problem;
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
    };
    readLayout();

    LARGE_INTEGER frequency;
    QueryPerformanceFrequency(&frequency);
    const double msPerTick = 1000.0 / static_cast<double>(frequency.QuadPart);
    StatWindow work;
    StatWindow finding;
    std::int64_t windowStart = qpcNow();
    const auto publishStats = [&](std::int64_t elapsed) {
        const StatWindow::Summary workSummary = work.take();
        const StatWindow::Summary findSummary = finding.take();
        UiFinderStats& stats = stats_.writeSlot();
        stats.imagesPerSecond =
            elapsed > 0 ? static_cast<float>(workSummary.count / (static_cast<double>(elapsed) * msPerTick / 1000.0))
                        : 0.0f;
        stats.workAvgMs = workSummary.average;
        stats.workMaxMs = workSummary.max;
        stats.findAvgMs = findSummary.average;
        std::snprintf(stats.problem.data(), stats.problem.size(), "%s", problem.c_str());
        stats_.publish();
    };
    publishStats(0); // So a problem with ui.txt shows at once.
    state_.store(State::Running, std::memory_order_release);

    const GreyArea whole{0, 0, static_cast<int>(width_), static_cast<int>(height_)};
    ImageStamp stamp;
    while (!stop.stop_requested()) {
        if (reader_->waitForImage(kWaitMs)) {
            const std::int64_t start = qpcNow();
            if (ID3D11Texture2D* image = reader_->useNewImage(&stamp)) {
                // Its grey image first, so the image goes back to the channel at once; then only the full-size level
                // comes back.
                const GreyView* grey = nullptr;
                try {
                    greys->make(gpu_.context.Get(), stamp.number, image);
                    reader_->endUse();
                    greys->read(gpu_.context.Get(), stamp.number, {&whole, 1}, 1);
                    greys->wait(gpu_.context.Get());
                    grey = greys->find(stamp.number);
                } catch (const std::exception&) {
                    reader_->endUse(); // Harmless once ended.
                    fail("The GPU stopped making grey images (it may have been reset)");
                    return;
                }
                const std::int64_t read = qpcNow();
                UiPieces& pieces = found_.writeSlot();
                pieces.count = layout->find(grey->levels[0], pieces.pieces.data(), UiPieces::kMax);
                pieces.image = stamp.number;
                found_.publish();
                const std::int64_t done = qpcNow();
                work.add(static_cast<float>(static_cast<double>(done - start) * msPerTick));
                finding.add(static_cast<float>(static_cast<double>(done - read) * msPerTick));
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

void UiFinder::fail(std::string message) {
    error_ = std::move(message);
    state_.store(State::Failed, std::memory_order_release);
}

} // namespace observex
