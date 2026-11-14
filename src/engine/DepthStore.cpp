#include "engine/DepthStore.h"

#include <windows.h>

#include <compressapi.h>

#include <cstring>
#include <fstream>
#include <stdexcept>

namespace observex {

namespace {

constexpr char kMagic[8] = {'O', 'X', 'D', 'E', 'P', 'T', 'H', '1'};
constexpr std::uint32_t kMaxSide = 1 << 14;  // Past any map or screen: a file saying more is broken.
constexpr std::uint32_t kMaxIgnore = 1 << 12;

std::size_t pixelsOf(const DepthRecord& record) {
    return static_cast<std::size_t>(record.width) * static_cast<std::size_t>(record.height);
}

// The value before pixel `i` of a `width`-wide image (row by row): the one to its left, at a row's start the one
// above, 0 for the first.
template <typename T> T before(const T* values, std::size_t i, int width) {
    if (i == 0) {
        return T{0};
    }
    const auto w = static_cast<std::size_t>(width);
    return i % w != 0 ? values[i - 1] : values[i - w];
}

template <typename T> void put(std::ofstream& out, const T& value) {
    out.write(reinterpret_cast<const char*>(&value), sizeof(value));
}

template <typename T> bool get(std::ifstream& in, T& value) {
    return static_cast<bool>(in.read(reinterpret_cast<char*>(&value), sizeof(value)));
}

bool getBytes(std::ifstream& in, std::vector<std::uint8_t>& bytes, std::uint32_t most) {
    std::uint32_t size = 0;
    if (!get(in, size) || size > most) {
        return false;
    }
    bytes.resize(size);
    return size == 0 || static_cast<bool>(in.read(reinterpret_cast<char*>(bytes.data()), size));
}

} // namespace

DepthPacker::DepthPacker() {
    COMPRESSOR_HANDLE compressor = nullptr;
    DECOMPRESSOR_HANDLE decompressor = nullptr;
    if (!CreateCompressor(COMPRESS_ALGORITHM_XPRESS_HUFF, nullptr, &compressor)) {
        throw std::runtime_error("Windows' compression did not open (error " + std::to_string(GetLastError()) + ")");
    }
    if (!CreateDecompressor(COMPRESS_ALGORITHM_XPRESS_HUFF, nullptr, &decompressor)) {
        CloseCompressor(compressor);
        throw std::runtime_error("Windows' decompression did not open (error " + std::to_string(GetLastError()) + ")");
    }
    compressor_ = compressor;
    decompressor_ = decompressor;
}

DepthPacker::~DepthPacker() {
    CloseCompressor(static_cast<COMPRESSOR_HANDLE>(compressor_));
    CloseDecompressor(static_cast<DECOMPRESSOR_HANDLE>(decompressor_));
}

bool DepthPacker::compress(const std::uint8_t* data, std::size_t size, std::vector<std::uint8_t>& out) {
    SIZE_T needed = 0;
    if (!Compress(static_cast<COMPRESSOR_HANDLE>(compressor_), data, size, nullptr, 0, &needed) &&
        GetLastError() != ERROR_INSUFFICIENT_BUFFER) {
        return false;
    }
    out.resize(needed);
    SIZE_T written = 0;
    if (!Compress(static_cast<COMPRESSOR_HANDLE>(compressor_), data, size, out.data(), out.size(), &written)) {
        return false;
    }
    out.resize(written);
    return true;
}

bool DepthPacker::decompress(const std::vector<std::uint8_t>& packed, std::uint8_t* out, std::size_t size) {
    SIZE_T written = 0;
    return !packed.empty() &&
           Decompress(static_cast<DECOMPRESSOR_HANDLE>(decompressor_), packed.data(), packed.size(), out, size,
                      &written) &&
           written == size;
}

bool DepthPacker::pack(const std::uint16_t* map, const std::uint8_t* levels, DepthRecord& record) {
    const std::size_t pixels = pixelsOf(record);
    if (pixels == 0) {
        return false;
    }
    // The map: each value's difference from the one before (positive half floats grow with their bits, so a small
    // change of depth is a small difference), their low bytes first, then their high bytes (mostly 0 or 255).
    scratch_.resize(pixels * 2);
    for (std::size_t i = 0; i < pixels; ++i) {
        const auto difference = static_cast<std::uint16_t>(map[i] - before(map, i, record.width));
        scratch_[i] = static_cast<std::uint8_t>(difference & 0xFFu);
        scratch_[pixels + i] = static_cast<std::uint8_t>(difference >> 8);
    }
    if (!compress(scratch_.data(), pixels * 2, record.map)) {
        return false;
    }
    scratch_.resize(pixels);
    for (std::size_t i = 0; i < pixels; ++i) {
        scratch_[i] = static_cast<std::uint8_t>(levels[i] - before(levels, i, record.width));
    }
    return compress(scratch_.data(), pixels, record.levels);
}

bool DepthPacker::unpackMap(const DepthRecord& record, std::vector<std::uint16_t>& map) {
    const std::size_t pixels = pixelsOf(record);
    scratch_.resize(pixels * 2);
    if (pixels == 0 || !decompress(record.map, scratch_.data(), pixels * 2)) {
        return false;
    }
    map.resize(pixels);
    for (std::size_t i = 0; i < pixels; ++i) {
        const auto difference = static_cast<std::uint16_t>(scratch_[i] | (scratch_[pixels + i] << 8));
        map[i] = static_cast<std::uint16_t>(before(map.data(), i, record.width) + difference);
    }
    return true;
}

bool DepthPacker::unpackLevels(const DepthRecord& record, std::vector<std::uint8_t>& levels) {
    const std::size_t pixels = pixelsOf(record);
    levels.resize(pixels);
    if (pixels == 0 || !decompress(record.levels, levels.data(), pixels)) {
        return false;
    }
    for (std::size_t i = 0; i < pixels; ++i) {
        levels[i] = static_cast<std::uint8_t>(levels[i] + before(levels.data(), i, record.width));
    }
    return true;
}

// depth.bin: "OXDEPTH1", how many records, then each: image (8 bytes), width, height, screen width, screen height,
// the number of interface pieces and each one's x0 y0 x1 y1 (4 bytes each), the packed map and the packed levels
// (each its size, 4 bytes, then its bytes), the palette (256 x 4 bytes). Little-endian, as Windows is.
void writeDepthFile(const std::filesystem::path& file, std::span<const std::shared_ptr<const DepthRecord>> records) {
    std::filesystem::path temporary = file;
    temporary += L".tmp";
    {
        std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
        out.write(kMagic, sizeof(kMagic));
        put(out, static_cast<std::uint32_t>(records.size()));
        for (const std::shared_ptr<const DepthRecord>& record : records) {
            put(out, record->image);
            put(out, static_cast<std::int32_t>(record->width));
            put(out, static_cast<std::int32_t>(record->height));
            put(out, static_cast<std::int32_t>(record->screenWidth));
            put(out, static_cast<std::int32_t>(record->screenHeight));
            put(out, static_cast<std::uint32_t>(record->ignore.size()));
            for (const GreyArea& area : record->ignore) {
                put(out, static_cast<std::int32_t>(area.x0));
                put(out, static_cast<std::int32_t>(area.y0));
                put(out, static_cast<std::int32_t>(area.x1));
                put(out, static_cast<std::int32_t>(area.y1));
            }
            for (const std::vector<std::uint8_t>* bytes : {&record->map, &record->levels}) {
                put(out, static_cast<std::uint32_t>(bytes->size()));
                out.write(reinterpret_cast<const char*>(bytes->data()), static_cast<std::streamsize>(bytes->size()));
            }
            out.write(reinterpret_cast<const char*>(record->palette.data()),
                      static_cast<std::streamsize>(sizeof(record->palette)));
        }
        if (!out) {
            throw std::runtime_error("cannot write " + file.filename().string());
        }
    }
    std::filesystem::rename(temporary, file);
}

std::vector<DepthRecord> readDepthFile(const std::filesystem::path& file, std::string* error) {
    const auto fail = [&](const char* why) {
        if (error != nullptr) {
            *error = why;
        }
        return std::vector<DepthRecord>{};
    };
    std::ifstream in(file, std::ios::binary);
    if (!in) {
        return fail("no depth.bin");
    }
    char magic[sizeof(kMagic)] = {};
    std::uint32_t count = 0;
    if (!in.read(magic, sizeof(magic)) || std::memcmp(magic, kMagic, sizeof(kMagic)) != 0 || !get(in, count)) {
        return fail("depth.bin is not a depth file of this observex");
    }
    std::vector<DepthRecord> records;
    for (std::uint32_t r = 0; r < count; ++r) {
        DepthRecord record;
        std::int32_t sides[4] = {};
        std::uint32_t ignored = 0;
        if (!get(in, record.image) || !get(in, sides) || !get(in, ignored) || ignored > kMaxIgnore) {
            return fail("depth.bin is cut short");
        }
        for (const std::int32_t side : sides) {
            if (side <= 0 || static_cast<std::uint32_t>(side) > kMaxSide) {
                return fail("depth.bin is broken");
            }
        }
        record.width = sides[0];
        record.height = sides[1];
        record.screenWidth = sides[2];
        record.screenHeight = sides[3];
        record.ignore.resize(ignored);
        for (GreyArea& area : record.ignore) {
            std::int32_t corners[4] = {};
            if (!get(in, corners)) {
                return fail("depth.bin is cut short");
            }
            area = {corners[0], corners[1], corners[2], corners[3]};
        }
        const std::uint32_t most = static_cast<std::uint32_t>(pixelsOf(record) * 3 + 4096); // More than packing makes.
        if (!getBytes(in, record.map, most) || !getBytes(in, record.levels, most) || !get(in, record.palette)) {
            return fail("depth.bin is cut short");
        }
        records.push_back(std::move(record));
    }
    return records;
}

} // namespace observex
