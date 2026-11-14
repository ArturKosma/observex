// observex_bench --depth CLIP: the Show flag Depth over a saved clip, as the app shows it live (engine/DepthNet on each
// frame, shaded with core/Depth's Depthing, the interface found by core/UiLayout left out), and what it costs and how
// steady it is. See bench/Bench.cpp's header for the options.
#include "DepthBench.h"

#include "core/ClipGuesses.h"
#include "core/ClipLines.h"
#include "core/Depth.h"
#include "core/Recordings.h"
#include "core/UiLayout.h"
#include "engine/ClipPlayer.h"
#include "engine/DepthNet.h"
#include "engine/DepthStore.h"
#include "engine/Gpu.h"
#include "engine/HeuristicsLibrary.h"
#include "engine/ImageFile.h"
#include "engine/ScreenCapture.h"
#include "heuristics/DepthRules.h"
#include "heuristics/Snapper.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <vector>

namespace observex {

namespace {

namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

double msSince(Clock::time_point start) {
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

// Samples of one measure: their mean, a percentile and the most.
struct Samples {
    std::vector<double> values;
    void add(double value) { values.push_back(value); }
    double mean() const {
        double sum = 0.0;
        for (const double value : values) {
            sum += value;
        }
        return values.empty() ? 0.0 : sum / static_cast<double>(values.size());
    }
    double percentile(double share) const {
        if (values.empty()) {
            return 0.0;
        }
        std::vector<double> sorted = values;
        std::sort(sorted.begin(), sorted.end());
        return sorted[std::min(sorted.size() - 1, static_cast<std::size_t>(share * static_cast<double>(sorted.size())))];
    }
    double most() const { return values.empty() ? 0.0 : *std::max_element(values.begin(), values.end()); }
};

// A .npy file of 32-bit floats in 2 dimensions (numpy.save of the Python network's map). False if it is not one.
bool readNpy(const fs::path& file, std::vector<float>& values, int& rows, int& columns) {
    std::ifstream in(file, std::ios::binary);
    char magic[8] = {};
    if (!in.read(magic, 8) || std::memcmp(magic, "\x93NUMPY", 6) != 0) {
        return false;
    }
    std::uint32_t headerSize = 0;
    if (magic[6] == 1) {
        std::uint16_t size = 0;
        in.read(reinterpret_cast<char*>(&size), 2);
        headerSize = size;
    } else {
        in.read(reinterpret_cast<char*>(&headerSize), 4);
    }
    std::string header(headerSize, '\0');
    if (!in.read(header.data(), headerSize) || header.find("'<f4'") == std::string::npos ||
        header.find("'fortran_order': False") == std::string::npos) {
        return false;
    }
    const std::size_t shape = header.find('(', header.find("'shape'"));
    if (shape == std::string::npos) {
        return false;
    }
    char* end = nullptr;
    rows = static_cast<int>(std::strtol(header.c_str() + shape + 1, &end, 10));
    columns = *end == ',' ? static_cast<int>(std::strtol(end + 1, nullptr, 10)) : 0;
    if (rows <= 0 || columns <= 0) {
        return false;
    }
    values.resize(static_cast<std::size_t>(rows) * static_cast<std::size_t>(columns));
    return static_cast<bool>(in.read(reinterpret_cast<char*>(values.data()),
                                     static_cast<std::streamsize>(values.size() * sizeof(float))));
}

// How well `b` agrees with `a`: their correlation, and how far `b` fitted to `a` (a scale and an offset: the network's
// map has its own of each image) is off on average, as a share of `a`'s range.
struct Agreement {
    double correlation = 0.0;
    double offShare = 0.0;
};
Agreement agree(const std::vector<float>& a, const std::vector<float>& b) {
    const double n = static_cast<double>(a.size());
    double ma = 0.0;
    double mb = 0.0;
    for (std::size_t i = 0; i < a.size(); ++i) {
        ma += a[i];
        mb += b[i];
    }
    ma /= n;
    mb /= n;
    double ab = 0.0;
    double aa = 0.0;
    double bb = 0.0;
    float low = a[0];
    float high = a[0];
    for (std::size_t i = 0; i < a.size(); ++i) {
        ab += (a[i] - ma) * (b[i] - mb);
        aa += (a[i] - ma) * (a[i] - ma);
        bb += (b[i] - mb) * (b[i] - mb);
        low = std::min(low, a[i]);
        high = std::max(high, a[i]);
    }
    Agreement agreement;
    agreement.correlation = ab / std::sqrt(std::max(aa * bb, 1e-30));
    const double scale = ab / std::max(bb, 1e-30);
    double off = 0.0;
    for (std::size_t i = 0; i < a.size(); ++i) {
        off += std::abs(ma + scale * (b[i] - mb) - a[i]);
    }
    agreement.offShare = off / n / std::max(static_cast<double>(high - low), 1e-30);
    return agreement;
}

// `width` x `height` BGRA pixels into a picture `outWidth` x `outHeight`, bilinearly.
void resize(const std::uint32_t* in, int width, int height, std::uint32_t* out, int outWidth, int outHeight) {
    for (int y = 0; y < outHeight; ++y) {
        const float sy = std::clamp((static_cast<float>(y) + 0.5f) * static_cast<float>(height) /
                                            static_cast<float>(outHeight) - 0.5f,
                                    0.0f, static_cast<float>(height - 1));
        const int y0 = static_cast<int>(sy);
        const int y1 = std::min(y0 + 1, height - 1);
        const float ty = sy - static_cast<float>(y0);
        for (int x = 0; x < outWidth; ++x) {
            const float sx = std::clamp((static_cast<float>(x) + 0.5f) * static_cast<float>(width) /
                                                static_cast<float>(outWidth) - 0.5f,
                                        0.0f, static_cast<float>(width - 1));
            const int x0 = static_cast<int>(sx);
            const int x1 = std::min(x0 + 1, width - 1);
            const float tx = sx - static_cast<float>(x0);
            std::uint32_t pixel = 0xFF000000u;
            for (int c = 0; c < 3; ++c) {
                const auto at = [&](int px, int py) {
                    return static_cast<float>((in[static_cast<std::size_t>(py) * static_cast<std::size_t>(width) +
                                                  static_cast<std::size_t>(px)] >>
                                               (8 * c)) &
                                              0xFFu);
                };
                const float value = (at(x0, y0) * (1 - tx) + at(x1, y0) * tx) * (1 - ty) +
                                    (at(x0, y1) * (1 - tx) + at(x1, y1) * tx) * ty;
                pixel |= static_cast<std::uint32_t>(std::lround(value)) << (8 * c);
            }
            out[static_cast<std::size_t>(y) * static_cast<std::size_t>(outWidth) + static_cast<std::size_t>(x)] = pixel;
        }
    }
}

// A rectangle's outline, 2 pixels wide, cut to the picture.
void outline(std::uint32_t* pixels, int width, int height, int x0, int y0, int x1, int y1, std::uint32_t colour) {
    x0 = std::clamp(x0, 0, width - 1);
    x1 = std::clamp(x1, 0, width - 1);
    y0 = std::clamp(y0, 0, height - 1);
    y1 = std::clamp(y1, 0, height - 1);
    const auto put = [&](int x, int y) {
        pixels[static_cast<std::size_t>(y) * static_cast<std::size_t>(width) + static_cast<std::size_t>(x)] = colour;
    };
    for (int t = 0; t < 2; ++t) {
        for (int x = x0; x <= x1; ++x) {
            put(x, std::min(y0 + t, height - 1));
            put(x, std::max(y1 - t, 0));
        }
        for (int y = y0; y <= y1; ++y) {
            put(std::min(x0 + t, width - 1), y);
            put(std::max(x1 - t, 0), y);
        }
    }
}

} // namespace

int depthClip(const DepthBenchOptions& options) {
    fs::path video(options.clip);
    fs::path clipFolder; // With its clip.txt, so --out becomes a copy of the clip with the depth drawn.
    if (fs::is_directory(video)) {
        clipFolder = video;
        video /= kClipVideo;
    }
    std::string clipText;
    if (!clipFolder.empty()) {
        std::ifstream file(clipFolder / kClipText, std::ios::binary);
        clipText.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
    }
    const ClipText clip = readClipText(clipText);
    const fs::path modelFolder = options.model != nullptr ? fs::path(options.model) : fs::path("models/depth");
    const fs::path layoutFolder = options.layout != nullptr ? fs::path(options.layout) : fs::path("models/ui");
    UiLayout layout;
    std::string error;
    if (!layout.load(layoutFolder, &error)) {
        std::printf("%s\n", error.c_str());
        return 1;
    }
    std::shared_ptr<HeuristicsLibrary> library;
    if (options.heuristics != nullptr) {
        library = HeuristicsLibrary::load(options.heuristics);
    }
    const auto makeDepthing = [&]() -> std::unique_ptr<Depthing> {
        return library ? library->makeDepth() : std::make_unique<DepthRules>();
    };
    std::unique_ptr<Depthing> depthing = makeDepthing();

    const ComPtr<IDXGIAdapter1> adapter = findPrimaryGpu();
    const GpuDevice gpu = createGpuDevice(adapter.Get(), true); // Video support, for the GPU's video decoder.
    std::printf("Loading the depth network from %s (the first time, TensorRT builds its engine: a minute or so)\n",
                toUtf8(modelFolder.c_str()).c_str());
    std::atomic<float> progress{0.0f};
    const Clock::time_point loadStart = Clock::now();
    DepthNet net(gpu.device.Get(), adapter.Get(), modelFolder, std::stop_token{}, progress);
    const int width = net.width();
    const int height = net.height();
    const std::size_t pixels = static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
    std::printf("Loaded in %.1f s: maps %d x %d\n", msSince(loadStart) / 1000.0, width, height);

    ClipReader reader(gpu.device.Get(), video);
    const int screenWidth = static_cast<int>(reader.width());
    const int screenHeight = static_cast<int>(reader.height());
    const std::vector<double>& times = reader.times();
    std::printf("%s: %d x %d, %zu frames, every %u through the network; shading with %s\n",
                toUtf8(video.c_str()).c_str(), screenWidth, screenHeight, times.size(), options.every,
                options.heuristics != nullptr ? options.heuristics : "the rules built into the bench");

    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = reader.width();
    desc.Height = reader.height();
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = ScreenCapture::kImageFormat;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    ComPtr<ID3D11Texture2D> image;
    check(gpu.device->CreateTexture2D(&desc, nullptr, &image), "CreateTexture2D (clip frame)");
    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> staging;
    check(gpu.device->CreateTexture2D(&desc, nullptr, &staging), "CreateTexture2D (clip frame, CPU)");

    std::vector<std::uint8_t> screen(static_cast<std::size_t>(screenWidth) * static_cast<std::size_t>(screenHeight) * 4);
    GreyImage grey;
    std::vector<UiPiece> found(UiPieces::kMax);
    std::vector<GreyArea> ignore;
    std::vector<std::uint16_t> map(pixels);
    std::vector<std::uint8_t> levels(pixels);
    std::array<std::uint32_t, Depthing::kPaletteSize> palette{};
    std::vector<std::uint32_t> painted(pixels);
    DepthPacker packer;
    std::vector<std::uint16_t> mapBack;
    std::vector<std::uint8_t> levelsBack;
    std::vector<std::shared_ptr<const DepthRecord>> records;

    // Each map pixel's place on the half-size grey image, to tell pixels whose picture stayed still.
    std::vector<std::size_t> stillAt(pixels);
    std::vector<std::uint8_t> stillGrey(pixels);
    std::vector<std::uint8_t> stillBefore;
    std::vector<std::uint8_t> levelsBefore;
    std::vector<float> rawBefore;
    std::vector<float> raw(pixels);

    Samples networkMs;
    Samples shadeMs;
    Samples packMs;
    Samples unpackMs;
    Samples uiMs;
    Samples packedKb;
    Samples mapKb;
    Samples movedShare;    // Of the pixels, the share whose level moved more than 8 from the map before,
    Samples movedLevels;   // how far the levels moved on average,
    Samples stillShare;    // of the still pixels (the picture there moved less than 2 grey levels), the share,
    Samples stillLevels;   // how far their levels moved,
    Samples stillRaw;      // and how far the network's map moved there, as a share of the map's own range,
    Samples stillPixels;   // and how many of the pixels were still.
    bool lossless = true;
    bool compared = false;
    std::uint64_t mapped = 0;
    const std::uint64_t comparedFrame = options.at.empty() ? 0 : options.at.front();
    if (options.out != nullptr) {
        fs::create_directories(options.out);
    }

    ClipReader::Frame frame;
    while (reader.next(frame)) {
        const bool atFrame = std::find(options.at.begin(), options.at.end(), frame.number) != options.at.end();
        if ((frame.number - 1) % std::max(options.every, 1u) != 0 && !atFrame) {
            continue;
        }
        gpu.context->CopySubresourceRegion(image.Get(), 0, 0, 0, 0, frame.texture, frame.subresource, nullptr);
        gpu.context->CopyResource(staging.Get(), image.Get());
        D3D11_MAPPED_SUBRESOURCE mappedPixels{};
        check(gpu.context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mappedPixels), "Map (clip frame)");
        for (int y = 0; y < screenHeight; ++y) {
            std::memcpy(screen.data() + static_cast<std::size_t>(y) * static_cast<std::size_t>(screenWidth) * 4,
                        static_cast<const std::uint8_t*>(mappedPixels.pData) + static_cast<std::size_t>(y) *
                                                                                   mappedPixels.RowPitch,
                        static_cast<std::size_t>(screenWidth) * 4);
        }
        gpu.context->Unmap(staging.Get(), 0);
        grey.fromBgra(screen.data(), screenWidth, screenHeight, static_cast<std::ptrdiff_t>(screenWidth) * 4);
        const GreyView view = grey.view(frame.number);
        const Clock::time_point uiStart = Clock::now();
        const int pieces = layout.find(view.levels[0], found.data(), static_cast<int>(found.size()));
        uiMs.add(msSince(uiStart));
        ignore.clear();
        for (int i = 0; i < pieces; ++i) {
            const UiPiece& piece = found[static_cast<std::size_t>(i)];
            ignore.push_back({piece.x0, piece.y0, piece.x1, piece.y1});
        }

        // The network, as the depth finder runs it (the first few runs record its CUDA graph: not counted).
        const Clock::time_point netStart = Clock::now();
        net.squeeze(gpu.context.Get(), image.Get());
        const std::uint16_t* out = net.run();
        if (out == nullptr) {
            std::printf("The GPU failed running the depth network on frame %llu\n",
                        static_cast<unsigned long long>(frame.number));
            return 1;
        }
        const double netMs = msSince(netStart);
        std::memcpy(map.data(), out, pixels * sizeof(std::uint16_t));
        ++mapped;
        if (mapped > 3) {
            networkMs.add(netMs);
        }

        DepthInput input;
        input.map = map.data();
        input.width = width;
        input.height = height;
        input.screenWidth = screenWidth;
        input.screenHeight = screenHeight;
        input.ignore = ignore.data();
        input.ignoreCount = static_cast<int>(ignore.size());
        const Clock::time_point shadeStart = Clock::now();
        if (!depthing->shade(input, levels.data(), palette.data())) {
            std::printf("Shading failed on frame %llu\n", static_cast<unsigned long long>(frame.number));
            return 1;
        }
        shadeMs.add(msSince(shadeStart));

        // Packed for the clip buffer, and back.
        auto record = std::make_shared<DepthRecord>();
        // Named by its screen image, as the depth finder names its maps (the frame's number without clip.txt).
        const bool knownImage = frame.number >= 1 && frame.number <= clip.frameImage.size();
        record->image = knownImage ? clip.frameImage[frame.number - 1] : frame.number;
        record->width = width;
        record->height = height;
        record->screenWidth = screenWidth;
        record->screenHeight = screenHeight;
        record->ignore = ignore;
        record->palette = palette;
        const Clock::time_point packStart = Clock::now();
        if (!packer.pack(map.data(), levels.data(), *record)) {
            std::printf("Packing failed on frame %llu\n", static_cast<unsigned long long>(frame.number));
            return 1;
        }
        packMs.add(msSince(packStart));
        packedKb.add(static_cast<double>(record->map.size() + record->levels.size()) / 1024.0);
        mapKb.add(static_cast<double>(record->map.size()) / 1024.0);
        const Clock::time_point unpackStart = Clock::now();
        lossless = packer.unpackMap(*record, mapBack) && packer.unpackLevels(*record, levelsBack) && mapBack == map &&
                   levelsBack == levels && lossless;
        unpackMs.add(msSince(unpackStart));
        // A frame repeating the image before (the screen stood still) is the same map: kept once.
        if (records.empty() || record->image > records.back()->image) {
            records.push_back(std::move(record));
        }

        // Frame to frame: how much the shown levels moved, and where the picture stood still.
        float rawLow = 1e30f;
        float rawHigh = -1e30f;
        for (std::size_t i = 0; i < pixels; ++i) {
            raw[i] = halfToFloat(map[i]);
            rawLow = std::min(rawLow, raw[i]);
            rawHigh = std::max(rawHigh, raw[i]);
        }
        const GreyLevel& half = view.levels[1];
        if (mapped == 1) {
            for (int y = 0; y < height; ++y) {
                const int gy = std::min(static_cast<int>((static_cast<float>(y) + 0.5f) * static_cast<float>(half.height) /
                                                         static_cast<float>(height)),
                                        half.height - 1);
                for (int x = 0; x < width; ++x) {
                    const int gx = std::min(static_cast<int>((static_cast<float>(x) + 0.5f) *
                                                             static_cast<float>(half.width) / static_cast<float>(width)),
                                            half.width - 1);
                    stillAt[static_cast<std::size_t>(y) * static_cast<std::size_t>(width) + static_cast<std::size_t>(x)] =
                        static_cast<std::size_t>(gy) * static_cast<std::size_t>(half.stride) + static_cast<std::size_t>(gx);
                }
            }
        }
        for (std::size_t i = 0; i < pixels; ++i) {
            stillGrey[i] = half.pixels[stillAt[i]];
        }
        if (levelsBefore.size() == pixels) {
            std::size_t moved = 0;
            double sum = 0.0;
            std::size_t still = 0;
            std::size_t stillMoved = 0;
            double stillSum = 0.0;
            double stillRawSum = 0.0;
            for (std::size_t i = 0; i < pixels; ++i) {
                const int change = std::abs(static_cast<int>(levels[i]) - static_cast<int>(levelsBefore[i]));
                moved += change > 8 ? 1 : 0;
                sum += change;
                if (std::abs(static_cast<int>(stillGrey[i]) - static_cast<int>(stillBefore[i])) < 2) {
                    ++still;
                    stillMoved += change > 8 ? 1 : 0;
                    stillSum += change;
                    stillRawSum += std::abs(raw[i] - rawBefore[i]);
                }
            }
            movedShare.add(static_cast<double>(moved) / static_cast<double>(pixels));
            movedLevels.add(sum / static_cast<double>(pixels));
            stillPixels.add(static_cast<double>(still) / static_cast<double>(pixels));
            if (still > pixels / 20) { // A frame with almost nothing still says nothing of flicker.
                stillShare.add(static_cast<double>(stillMoved) / static_cast<double>(still));
                stillLevels.add(stillSum / static_cast<double>(still));
                stillRaw.add(stillRawSum / static_cast<double>(still) / std::max(rawHigh - rawLow, 1e-6f));
            }
        }
        levelsBefore = levels;
        stillBefore = stillGrey;
        rawBefore = raw;

        // Against the Python network's map of the same frame.
        if (options.compare != nullptr && frame.number == comparedFrame) {
            std::vector<float> python;
            int rows = 0;
            int columns = 0;
            if (!readNpy(options.compare, python, rows, columns) || rows != height || columns != width) {
                std::printf("%s is not a %d x %d map of 32-bit floats\n", options.compare, width, height);
            } else {
                const Agreement agreement = agree(python, raw);
                std::printf("Against Python's map of frame %llu (%s): correlation %.5f, fitted off by %.2f%% of its "
                            "range on average\n",
                            static_cast<unsigned long long>(frame.number), options.compare, agreement.correlation,
                            agreement.offShare * 100.0);
                compared = true;
            }
        }

        // A picture: the screen and its depth side by side, half size, the interface left out outlined.
        const bool picture = options.out != nullptr && (atFrame || mapped == 1 || frame.number % 150 == 0);
        if (picture) {
            const int halfWidth = screenWidth / 2;
            const int halfHeight = screenHeight / 2;
            std::vector<std::uint32_t> side(static_cast<std::size_t>(halfWidth) * 2 * static_cast<std::size_t>(halfHeight));
            std::vector<std::uint32_t> left(static_cast<std::size_t>(halfWidth) * static_cast<std::size_t>(halfHeight));
            std::vector<std::uint32_t> right(left.size());
            resize(reinterpret_cast<const std::uint32_t*>(screen.data()), screenWidth, screenHeight, left.data(),
                   halfWidth, halfHeight);
            paintDepth(levels.data(), palette.data(), pixels, painted.data());
            resize(painted.data(), width, height, right.data(), halfWidth, halfHeight);
            for (const GreyArea& area : ignore) {
                outline(right.data(), halfWidth, halfHeight, area.x0 / 2, area.y0 / 2, area.x1 / 2, area.y1 / 2,
                        0xFF60C0FFu);
            }
            for (int y = 0; y < halfHeight; ++y) {
                std::copy_n(left.begin() + static_cast<std::ptrdiff_t>(y) * halfWidth, halfWidth,
                            side.begin() + static_cast<std::ptrdiff_t>(y) * halfWidth * 2);
                std::copy_n(right.begin() + static_cast<std::ptrdiff_t>(y) * halfWidth, halfWidth,
                            side.begin() + static_cast<std::ptrdiff_t>(y) * halfWidth * 2 + halfWidth);
            }
            char name[40];
            std::snprintf(name, sizeof(name), "depth-%06llu.jpg", static_cast<unsigned long long>(frame.number));
            writeImageFile(fs::path(options.out) / name, ImageFormat::Jpeg, side.data(),
                           static_cast<unsigned>(halfWidth * 2), static_cast<unsigned>(halfHeight),
                           static_cast<unsigned>(halfWidth * 2 * 4));
        }
    }
    if (mapped == 0) {
        std::printf("No frames\n");
        return 1;
    }

    // As a clip keeps them: written to depth.bin, read back and shaded again by a new shading (Today) against the
    // levels shown (Recorded).
    const fs::path file = options.out != nullptr ? fs::path(options.out) / kClipDepth
                                                 : fs::temp_directory_path() / "observex-bench-depth.bin";
    const Clock::time_point writeStart = Clock::now();
    writeDepthFile(file, records);
    const double writeMs = msSince(writeStart);
    const std::uintmax_t fileBytes = fs::file_size(file);
    const std::vector<DepthRecord> back = readDepthFile(file, &error);
    std::unique_ptr<Depthing> today = makeDepthing();
    std::size_t same = 0;
    for (const DepthRecord& record : back) {
        DepthInput input;
        if (!packer.unpackMap(record, mapBack) || !packer.unpackLevels(record, levelsBack)) {
            continue;
        }
        input.map = mapBack.data();
        input.width = record.width;
        input.height = record.height;
        input.screenWidth = record.screenWidth;
        input.screenHeight = record.screenHeight;
        input.ignore = record.ignore.data();
        input.ignoreCount = static_cast<int>(record.ignore.size());
        std::array<std::uint32_t, Depthing::kPaletteSize> todayPalette{};
        if (today->shade(input, levels.data(), todayPalette.data()) && levels == levelsBack &&
            todayPalette == record.palette) {
            ++same;
        }
    }
    if (options.out == nullptr) {
        std::error_code removeError;
        fs::remove(file, removeError);
    }
    // --out a copy of the clip, as if the flag Depth had been on while it was recorded: each map drawn from its own
    // frame on (live it comes a frame or two later), so the clip screen's Depth shows it (Recorded) and shades it again
    // (Today).
    if (options.out != nullptr && !clip.frameImage.empty()) {
        std::string text = clipText;
        text += "# depth lines added by observex_bench --depth: each map drawn from its own frame on.\n";
        for (std::size_t n = 1; n <= clip.frameImage.size() && n <= clip.frameMs.size(); ++n) {
            const auto made = std::lower_bound(records.begin(), records.end(), clip.frameImage[n - 1],
                                               [](const auto& at, std::uint64_t image) {
                                                   return at->image < image;
                                               });
            if (made != records.end() && (*made)->image == clip.frameImage[n - 1] &&
                (n == 1 || clip.frameImage[n - 2] != clip.frameImage[n - 1])) {
                appendDepth(text, clip.frameImage[n - 1], clip.frameMs[n - 1], (*made)->image);
            }
        }
        std::ofstream out(fs::path(options.out) / kClipText, std::ios::binary | std::ios::trunc);
        out << text;
        for (const char* name : {kClipVideo, kClipPicture, kClipFile}) {
            std::error_code copyError;
            fs::copy_file(clipFolder / name, fs::path(options.out) / name, fs::copy_options::overwrite_existing,
                          copyError);
        }
        std::printf("%s is now a copy of the clip with the depth drawn on it\n", options.out);
    }

    std::printf("\nNetwork (squeeze, network, map back; waited for): %.2f ms on average, %.2f at the 95th percentile, "
                "%.2f at most\n",
                networkMs.mean(), networkMs.percentile(0.95), networkMs.most());
    std::printf("Shading: %.2f ms on average, %.2f at most; interface found in %.2f ms (%zu pieces on the last)\n",
                shadeMs.mean(), shadeMs.most(), uiMs.mean(), ignore.size());
    std::printf("Packing: %.2f ms, unpacking %.2f ms; packed %.0f kB a map (the network's %.0f, the levels %.0f): "
                "%.0f MB for 10 s at %u a second; without loss: %s\n",
                packMs.mean(), unpackMs.mean(), packedKb.mean(), mapKb.mean(), packedKb.mean() - mapKb.mean(),
                packedKb.mean() * 10.0 * 20.0 / 1024.0, 20u, lossless ? "yes" : "NO");
    std::printf("depth.bin: %zu maps, %.1f MB, written in %.0f ms; shaded again from it: %zu of %zu the same as shown\n",
                back.size(), static_cast<double>(fileBytes) / (1024.0 * 1024.0), writeMs, same, back.size());
    std::printf("Frame to frame (%zu pairs): %.1f%% of the pixels moved more than 8 levels, %.2f levels on average\n",
                movedShare.values.size(), movedShare.mean() * 100.0, movedLevels.mean());
    std::printf("  Where the picture stood still (%.0f%% of the pixels on average; %zu pairs with enough): %.1f%% "
                "moved more than 8 levels, %.2f levels on average; the network's map %.2f%% of its range\n",
                stillPixels.mean() * 100.0, stillShare.values.size(), stillShare.mean() * 100.0, stillLevels.mean(),
                stillRaw.mean() * 100.0);
    if (options.compare != nullptr && !compared) {
        std::printf("No frame %llu to compare with %s\n", static_cast<unsigned long long>(comparedFrame),
                    options.compare);
    }
    return lossless && same == back.size() ? 0 : 1;
}

} // namespace observex
