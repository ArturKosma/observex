#pragma once

#include "core/TripleBuffer.h"
#include "engine/Gpu.h"

#include <array>
#include <cstdint>

namespace observex {

// Carries the newest captured image from the pipeline's GPU device to the UI's GPU device without either
// side ever waiting for the other. Three shared textures rotate through a TripleBuffer: the pipeline
// writes one, the UI shows another, and the third is the hand-over. Each texture's keyed mutex makes the
// GPU finish one side's work on it before the other side's starts.
class PreviewChannel {
public:
    // Creates the shared textures on the pipeline's device. Throws on failure.
    PreviewChannel(ID3D11Device* device, unsigned width, unsigned height, DXGI_FORMAT format);

    PreviewChannel(const PreviewChannel&) = delete;
    PreviewChannel& operator=(const PreviewChannel&) = delete;

    // Pipeline thread: copies `image` (same size and format) into a free texture and hands it to the UI.
    void write(ID3D11DeviceContext* context, ID3D11Texture2D* image);

    unsigned width() const { return width_; }
    unsigned height() const { return height_; }

private:
    friend class PreviewReader;

    // A texture's keyed mutex is released with kWriterKey when the pipeline may fill it,
    // and with kReaderKey when it holds an image for the UI.
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

// The UI's side of a PreviewChannel, on the UI's GPU device (which must be on the same GPU).
// Destroy it before the channel.
class PreviewReader {
public:
    // Opens the channel's shared textures. Throws on failure.
    PreviewReader(ID3D11Device* device, PreviewChannel& channel);

    PreviewReader(const PreviewReader&) = delete;
    PreviewReader& operator=(const PreviewReader&) = delete;

    // If a new image arrived, copies it into mip 0 of `target` (same size and format, on the UI's
    // device) and returns true. Call once per UI frame.
    bool copyNewImage(ID3D11DeviceContext* context, ID3D11Texture2D* target);

    unsigned width() const { return channel_.width(); }
    unsigned height() const { return channel_.height(); }

private:
    struct Slot {
        ComPtr<ID3D11Texture2D> texture;
        ComPtr<IDXGIKeyedMutex> mutex;
    };

    PreviewChannel& channel_;
    std::array<Slot, 3> slots_;
};

} // namespace observex
