#include "engine/Finder.h"

#include "core/Features.h"
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

// A found thing's box in whole image pixels, as the guesses file has it.
Box boxOf(const Detection& thing) {
    return {static_cast<int>(std::lround(thing.x)), static_cast<int>(std::lround(thing.y)),
            std::max(1, static_cast<int>(std::lround(thing.width))),
            std::max(1, static_cast<int>(std::lround(thing.height)))};
}

// Writes `bytes` to `file` through a temporary file, so the review never reads half of it.
void save(const fs::path& file, const std::string& bytes) {
    fs::path temporary = file;
    temporary += L".tmp";
    {
        std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
        out << bytes;
        if (!out) {
            throw std::runtime_error("Could not save " + toUtf8(file));
        }
    }
    fs::rename(temporary, file);
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
        GuessFeatures features; // Of each guess, for a model that gives them.
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
            // Reading order, so Tab in the review goes top to bottom. The features follow the same order.
            const auto end = found.begin() + count;
            std::sort(found.begin(), end, [](const Detection& a, const Detection& b) {
                const Box first = boxOf(a);
                const Box second = boxOf(b);
                return first.y != second.y ? first.y < second.y : first.x < second.x;
            });
            std::vector<Mark>& marks = guesses[name];
            for (auto thing = found.begin(); thing != end; ++thing) {
                Mark mark;
                mark.box = boxOf(*thing);
                mark.guess = thing->sure;
                mark.kind = thing->kind; // With kinds, the surest and the next, so the review can tell when it is unsure.
                mark.secondKind = thing->secondKind;
                mark.secondGuess = thing->second;
                mark.kindGuess = thing->kindSure;
                marks.push_back(mark);
            }
            features.size = detector->featureSize();
            if (features.size > 0) {
                std::vector<float>& numbers = features.images[name];
                numbers.resize(marks.size() * static_cast<std::size_t>(features.size));
                if (!detector->features({found.data(), marks.size()}, numbers.data())) {
                    throw std::runtime_error("The GPU stopped running the model (it may have been reset).");
                }
            }
            done_.fetch_add(1, std::memory_order_relaxed);
        }

        // The model's kinds, by the words the review knows them by (empty for a model of one category).
        std::vector<std::string_view> kinds;
        if (detector) {
            for (const std::string& kind : detector->kinds()) {
                kinds.push_back(kind);
            }
        }
        save(guessesFile(folder_, category_),
             formatMarks(guesses,
                         "observex guesses, category: " + category_ + ", by models/" + category_ +
                             "/model.onnx.\nEach mark: how sure the model is (0 to 1), " +
                             (kinds.empty() ? std::string() : "the kind it is surest of, the next with how sure, ") +
                             "then x, y, width and height in image pixels.",
                         kinds));
        // After the guesses, so features newer than the guesses are theirs (the review checks).
        const fs::path featuresFile = folder_ / (category_ + "-features.bin");
        if (features.size > 0) {
            save(featuresFile, formatFeatures(features));
        } else {
            std::error_code error;
            fs::remove(featuresFile, error); // An older model's: not of these guesses.
        }
        state_.store(State::Done, std::memory_order_release);
    } catch (const std::exception& e) {
        if (!stop.stop_requested()) {
            error_ = e.what();
            state_.store(State::Failed, std::memory_order_release);
        }
    }
}

} // namespace observex
