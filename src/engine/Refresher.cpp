#include "engine/Refresher.h"

#include "core/Recordings.h"
#include "engine/Finder.h"
#include "engine/KindUpdater.h"

#include <windows.h>

#include <chrono>
#include <exception>
#include <string_view>
#include <system_error>
#include <utility>

namespace observex {

namespace {

// How often it looks whether the Finder is done; also how quickly it notices a stop request.
constexpr auto kPoll = std::chrono::milliseconds(50);

} // namespace

Refresher::Refresher(std::filesystem::path recordings, std::string category, std::vector<std::string> kinds,
                     std::filesystem::path modelFolder, std::filesystem::path skip)
    : recordings_(std::move(recordings)), category_(std::move(category)), kinds_(std::move(kinds)),
      modelFolder_(std::move(modelFolder)), skip_(std::move(skip)) {
    thread_ = std::jthread([this](std::stop_token stop) { run(stop); });
}

Refresher::~Refresher() {
    if (thread_.joinable()) {
        thread_.request_stop();
        thread_.join();
    }
}

void Refresher::run(std::stop_token stop) {
    // Below normal, as the Finder: capture, live recognition and the UI come first.
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    SetThreadDescription(GetCurrentThread(), L"observex refresher");

    try {
        const std::vector<std::string_view> kinds(kinds_.begin(), kinds_.end());
        std::vector<std::filesystem::path> stale = KindUpdater::stale(recordings_, category_, kinds, modelFolder_);
        std::erase_if(stale, [&](const std::filesystem::path& folder) {
            std::error_code error;
            return !skip_.empty() && std::filesystem::equivalent(folder, skip_, error);
        });
        stale_.store(static_cast<int>(stale.size()), std::memory_order_relaxed);
        for (const std::filesystem::path& folder : stale) {
            std::vector<std::string> images = listImages(folder);
            if (images.empty()) {
                continue; // Its images were removed: nothing to look at.
            }
            Finder finder(folder, std::move(images), category_.c_str(), modelFolder_);
            while (finder.state() == Finder::State::Running) {
                if (stop.stop_requested()) {
                    return; // The Finder's destructor stops it: it writes nothing.
                }
                std::this_thread::sleep_for(kPoll);
            }
            if (finder.state() == Finder::State::Failed) {
                error_ = finder.error();
                break;
            }
            // A model without features (exported before 2026-11-08) can never make them fresh: no point going on.
            std::error_code error;
            if (!std::filesystem::exists(folder / (category_ + "-features.bin"), error)) {
                error_ = "The model gives no features: export it again (tools/train/export.py)";
                break;
            }
            refreshed_.fetch_add(1, std::memory_order_relaxed);
        }
    } catch (const std::exception& e) {
        error_ = e.what();
    }
    done_.store(true, std::memory_order_release);
}

} // namespace observex
