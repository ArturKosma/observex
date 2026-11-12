#pragma once

#include "core/TripleBuffer.h"
#include "engine/Gpu.h"

#include <array>
#include <cstdint>

namespace observex {

// Which captured image an image is: the pipeline's count of new screen images since it started (1 for the first),
// and when the screen showed it (QueryPerformanceCounter ticks). Lets readers on different threads (the recogniser,
// the recorder) say they handled the same image.
struct ImageStamp {
    std::uint64_t number = 0;
    std::int64_t shownQpc = 0;
};

// Carries the newest captured image from the pipeline's GPU device to one reader on another GPU device
// (the UI's preview, or the recorder) without either side ever waiting for the other. Three shared
// textures rotate through a TripleBuffer: the pipeline writes one, the reader uses another, and the third
// is the hand-over. Each texture's keyed mutex makes the GPU finish one side's work on it before the other
// side's starts.
class ImageChannel {
public:
    // Creates the shared textures on the pipeline's device. Throws on failure.
    ImageChannel(ID3D11Device* device, unsigned width, unsigned height, DXGI_FORMAT format);
    ~ImageChannel();

    ImageChannel(const ImageChannel&) = delete;
    ImageChannel& operator=(const ImageChannel&) = delete;

    // Pipeline thread: copies `image` (same size and format) into a free texture and hands it to the reader. Only its
    // `subresource` (e.g. one slice of a video decoder's texture array).
    void write(ID3D11DeviceContext* context, ID3D11Texture2D* image, const ImageStamp& stamp, UINT subresource = 0);

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
    TripleBuffer<ImageStamp> handover_; // Slot index = texture index; the value says which image it holds.
    HANDLE writtenEvent_ = nullptr;     // Set after each write, for ImageReader::waitForImage.
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
    // device), sets `stamp` (if given) to which image it is, and returns true. Never waits.
    bool copyNewImage(ID3D11DeviceContext* context, ID3D11Texture2D* target, ImageStamp* stamp = nullptr);

    // Like copyNewImage, but lends the image itself (a shader can read it: a copy of a 2560 x 1440 screen held up the
    // GPU the model runs on for about 0.4 ms, 2026-11-11): if a new one arrived, returns its texture on the reader's
    // device and sets `stamp`, else null. Never waits. Queue the GPU work that reads it, then call endUse().
    ID3D11Texture2D* useNewImage(ImageStamp* stamp = nullptr);
    void endUse();

    // Waits up to `timeoutMs` for the pipeline to write an image. Returns true if one may have arrived since the
    // last wait (call copyNewImage to take it). For a reader that wants every image the moment it comes.
    bool waitForImage(unsigned timeoutMs) const;

    unsigned width() const { return channel_.width(); }
    unsigned height() const { return channel_.height(); }

private:
    struct Slot {
        ComPtr<ID3D11Texture2D> texture;
        ComPtr<IDXGIKeyedMutex> mutex;
    };

    ImageChannel& channel_;
    std::array<Slot, 3> slots_;
    Slot* lent_ = nullptr; // Between useNewImage and endUse.
};

} // namespace observex
