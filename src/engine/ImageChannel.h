#pragma once

#include "core/TripleBuffer.h"
#include "engine/Gpu.h"

#include <array>
#include <cstdint>

namespace observex {

// Carries the newest captured image from the pipeline's GPU device to one reader on another GPU device
// (the UI's preview, or the recorder) without either side ever waiting for the other. Three shared
// textures rotate through a TripleBuffer: the pipeline writes one, the reader uses another, and the third
// is the hand-over. Each texture's keyed mutex makes the GPU finish one side's work on it before the other
// side's starts.
class ImageChannel {
public:
    // Creates the shared textures on the pipeline's device. Throws on failure.
    ImageChannel(ID3D11Device* device, unsigned width, unsigned height, DXGI_FORMAT format);

    ImageChannel(const ImageChannel&) = delete;
    ImageChannel& operator=(const ImageChannel&) = delete;

    // Pipeline thread: copies `image` (same size and format) into a free texture and hands it to the reader.
    void write(ID3D11DeviceContext* context, ID3D11Texture2D* image);

    unsigned width() const { return width_; }
    unsigned height() const { return height_; }

private:
    friend class ImageReader;

    // A texture's keyed mutex is released with kWriterKey when the pipeline may fill it,
    // and with kReaderKey when it holds an image for the reader.
    static constexpr UINT64 kWriterKey = 0;
    static constexpr UINT64 kReaderKey = 1;

    struct Slot {
        ComPtr<ID3D11Texture2D> texture;
        ComPtr<IDXGIKeyedMutex> mutex;
        HANDLE sharedHandle = nullptr;
    };

    std::array<Slot, 3> slots_;
    TripleBuffer<std::uint64_t> handover_; // Slot index = texture index; the value is the image number.
    std::uint64_t written_ = 0;
    UINT64 writeKey_ = kWriterKey; // Key the current write texture was last released with.
    unsigned width_;
    unsigned height_;
};

// The reading side of an ImageChannel, on the reader's GPU device (which must be on the same GPU).
// Use it from one thread only. Destroy it before the channel.
class ImageReader {
public:
    // Opens the channel's shared textures. Throws on failure.
    ImageReader(ID3D11Device* device, ImageChannel& channel);

    ImageReader(const ImageReader&) = delete;
    ImageReader& operator=(const ImageReader&) = delete;

    // If a new image arrived, copies it into mip 0 of `target` (same size and format, on the reader's
    // device) and returns true. Never waits.
    bool copyNewImage(ID3D11DeviceContext* context, ID3D11Texture2D* target);

    unsigned width() const { return channel_.width(); }
    unsigned height() const { return channel_.height(); }

private:
    struct Slot {
        ComPtr<ID3D11Texture2D> texture;
        ComPtr<IDXGIKeyedMutex> mutex;
    };

    ImageChannel& channel_;
    std::array<Slot, 3> slots_;
};

} // namespace observex
