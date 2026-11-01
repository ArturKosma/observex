#include "engine/Finder.h"

#include "core/Marks.h"
#include "engine/Detector.h"
#include "engine/Gpu.h"
#include "engine/ImageFile.h"
#include "engine/Recognizer.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <exception>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <system_error>
#include <utility>

namespace observex {

namespace {

namespace fs = std::filesystem;

fs::path guessesFile(const fs::path& folder, const std::string& category) {
    return folder / (category + "-guesses.txt");
}

fs::path fromUtf8(const std::string& text) {
    return fs::path(std::u8string(reinterpret_cast<const char8_t*>(text.data()), text.size()));
}

std::string toUtf8(const fs::path& path) {
    const std::u8string text = path.u8string();
    return std::string(reinterpret_cast<const char*>(text.data()), text.size());
}

} // namespace

bool Finder::needed(const fs::path& folder, const char* category, const fs::path& modelFolder) {
    const fs::path model = modelFolder / L"model.onnx";
    std::error_code error;
    if (!fs::exists(model, error)) {
        return false;
    }
    const fs::path guesses = guessesFile(folder, category);
    if (!fs::exists(guesses, error)) {
        return true;
    }
    const fs::file_time_type guessed = fs::last_write_time(guesses, error);
    const fs::file_time_type trained = fs::last_write_time(model, error);
    return !error && guessed < trained;
}

Finder::Finder(fs::path folder, std::vector<std::string> images, const char* category, fs::path modelFolder)
    : folder_(std::move(folder)), images_(std::move(images)), category_(category),
      modelFolder_(std::move(modelFolder)) {
    thread_ = std::jthread([this](std::stop_token stop) { run(stop); });
}

Finder::~Finder() {
    if (thread_.joinable()) {
        thread_.request_stop();
        thread_.join();
    }
}

void Finder::run(std::stop_token stop) {
    // Below normal: capture, live recognition and the UI come first.
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    SetThreadDescription(GetCurrentThread(), L"observex finder");

    try {
        const ComPtr<IDXGIAdapter1> adapter = findPrimaryGpu(); // An adapter object of its own (see Gpu.h).
        const GpuDevice gpu = createGpuDevice(adapter.Get());
        std::unique_ptr<Detector> detector; // Made for the first image's size, and again if the size changes.
        unsigned width = 0;
        unsigned height = 0;
        std::array<Detection, Detector::kMaxCandidates> found;
        ImageMarks guesses;
        for (const std::string& name : images_) {
            if (stop.stop_requested()) {
                return;
            }
            const ImageData image = readImageFile(folder_ / fromUtf8(name));
            if (!detector || image.width != width || image.height != height) {
                detector.reset();
                detector = std::make_unique<Detector>(gpu.device.Get(), adapter.Get(), modelFolder_, image.width,
                                                      image.height, stop, progress_);
                width = image.width;
                height = image.height;
            }
            gpu.context->UpdateSubresource(detector->input(), 0, nullptr, image.bgra.data(), image.width * 4, 0);
            const int count = detector->detect(gpu.context.Get(), Recognizer::kMinSure, found.data(),
                                               static_cast<int>(found.size()));
            if (count < 0) {
                throw std::runtime_error("The GPU stopped running the model (it may have been reset).");
            }
            std::vector<Mark>& marks = guesses[name];
            for (int i = 0; i < count; ++i) {
                const Detection& thing = found[static_cast<std::size_t>(i)];
                Mark mark;
                mark.box = {static_cast<int>(std::lround(thing.x)), static_cast<int>(std::lround(thing.y)),
                            std::max(1, static_cast<int>(std::lround(thing.width))),
                            std::max(1, static_cast<int>(std::lround(thing.height)))};
                mark.guess = thing.sure;
                marks.push_back(mark);
            }
            // Reading order, so Tab in the review goes top to bottom.
            std::sort(marks.begin(), marks.end(), [](const Mark& a, const Mark& b) {
                return a.box.y != b.box.y ? a.box.y < b.box.y : a.box.x < b.box.x;
            });
            done_.fetch_add(1, std::memory_order_relaxed);
        }

        // Through a temporary file, so the review never reads half of it.
        const fs::path file = guessesFile(folder_, category_);
        fs::path temporary = file;
        temporary += L".tmp";
        {
            std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
            out << formatMarks(guesses, "observex guesses, category: " + category_ + ", by models/" + category_ +
                                            "/model.onnx.\nEach mark: how sure the model is (0 to 1), then x, y, "
                                            "width and height in image pixels.");
            if (!out) {
                throw std::runtime_error("Could not save " + toUtf8(file));
            }
        }
        fs::rename(temporary, file);
        state_.store(State::Done, std::memory_order_release);
    } catch (const std::exception& e) {
        if (!stop.stop_requested()) {
            error_ = e.what();
            state_.store(State::Failed, std::memory_order_release);
        }
    }
}

} // namespace observex
