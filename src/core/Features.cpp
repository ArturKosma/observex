#include "core/Features.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iterator>
#include <system_error>

namespace observex {

namespace {

constexpr char kMagic[] = {'o', 'x', 'f', '1'};

void putCount(std::string& bytes, std::size_t count) {
    const std::uint32_t value = static_cast<std::uint32_t>(count);
    bytes.append(reinterpret_cast<const char*>(&value), sizeof(value)); // Little-endian, as Windows on x86 is.
}

// Reads `size` bytes from `bytes` at `at` into `to`, and moves on; false if there are not that many.
bool take(std::string_view bytes, std::size_t& at, void* to, std::size_t size) {
    if (bytes.size() - at < size) {
        return false;
    }
    std::memcpy(to, bytes.data() + at, size);
    at += size;
    return true;
}

bool takeCount(std::string_view bytes, std::size_t& at, std::uint32_t& count) {
    return take(bytes, at, &count, sizeof(count));
}

} // namespace

std::span<const float> GuessFeatures::of(const std::string& image, std::size_t index) const {
    const auto found = images.find(image);
    const std::size_t step = static_cast<std::size_t>(size);
    if (found == images.end() || step == 0 || (index + 1) * step > found->second.size()) {
        return {};
    }
    return std::span<const float>(found->second).subspan(index * step, step);
}

std::string formatFeatures(const GuessFeatures& features) {
    std::string bytes(kMagic, sizeof(kMagic));
    putCount(bytes, static_cast<std::size_t>(features.size));
    putCount(bytes, features.images.size());
    for (const auto& [name, numbers] : features.images) {
        putCount(bytes, name.size());
        bytes += name;
        putCount(bytes, features.size > 0 ? numbers.size() / static_cast<std::size_t>(features.size) : 0);
        bytes.append(reinterpret_cast<const char*>(numbers.data()), numbers.size() * sizeof(float));
    }
    return bytes;
}

GuessFeatures parseFeatures(std::string_view bytes) {
    std::size_t at = 0;
    char magic[sizeof(kMagic)] = {};
    std::uint32_t size = 0;
    std::uint32_t images = 0;
    if (!take(bytes, at, magic, sizeof(magic)) || std::memcmp(magic, kMagic, sizeof(kMagic)) != 0 ||
        !takeCount(bytes, at, size) || !takeCount(bytes, at, images) || size == 0) {
        return {};
    }
    GuessFeatures features;
    features.size = static_cast<int>(size);
    for (std::uint32_t image = 0; image < images; ++image) {
        std::uint32_t nameLength = 0;
        std::uint32_t guesses = 0;
        if (!takeCount(bytes, at, nameLength) || bytes.size() - at < nameLength) {
            return {};
        }
        std::string name(bytes.substr(at, nameLength));
        at += nameLength;
        if (!takeCount(bytes, at, guesses) || (bytes.size() - at) / sizeof(float) / size < guesses) {
            return {};
        }
        std::vector<float>& numbers = features.images[name];
        numbers.resize(static_cast<std::size_t>(guesses) * size);
        take(bytes, at, numbers.data(), numbers.size() * sizeof(float));
    }
    return at == bytes.size() ? features : GuessFeatures{};
}

bool featuresMatch(const GuessFeatures& features, const ImageMarks& guesses) {
    const std::size_t size = static_cast<std::size_t>(features.size);
    return size > 0 && features.images.size() == guesses.size() &&
           std::all_of(guesses.begin(), guesses.end(), [&](const auto& image) {
               const auto found = features.images.find(image.first);
               return found != features.images.end() && found->second.size() == image.second.size() * size;
           });
}

GuessFeatures freshFeatures(const std::filesystem::path& folder, std::string_view category,
                            const std::filesystem::path& modelFolder, const ImageMarks& guesses) {
    namespace fs = std::filesystem;
    const fs::path featuresFile = folder / (std::string(category) + "-features.bin");
    const fs::path guessesFile = folder / (std::string(category) + "-guesses.txt");
    std::error_code error;
    const fs::file_time_type written = fs::last_write_time(featuresFile, error);
    if (error || written < fs::last_write_time(guessesFile, error) || error ||
        written < fs::last_write_time(modelFolder / L"model.onnx", error) || error) {
        return {};
    }
    std::ifstream in(featuresFile, std::ios::binary);
    GuessFeatures features =
        parseFeatures(std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()));
    if (!featuresMatch(features, guesses)) {
        return {};
    }
    return features;
}

} // namespace observex
