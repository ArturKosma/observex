#pragma once

#include "engine/Gpu.h"
#include "engine/ImageChannel.h"

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

struct IMFDXGIDeviceManager;
struct IMFMediaBuffer;
struct IMFSample;
struct IMFSourceReader;

namespace observex {

// Reads a clip's video (clip.mp4) frame by frame with the GPU's video decoder (Media Foundation): each frame comes as a
// texture of ScreenCapture::kImageFormat on the reader's GPU device, converted from the video's colours on the GPU.
// If the GPU cannot decode it, it decodes on the CPU and uploads each frame instead (about ten times slower, the same
// frames). For the player's thread and the bench (--clip-frames); seek() is for stepping through a clip too. One thread
// only, with COM initialised on it.
class ClipReader {
public:
    struct Frame {
        ID3D11Texture2D* texture = nullptr; // width() x height(); valid until next(), rewind() or seek().
        UINT subresource = 0;               // Its slice of `texture` (decoders keep frames in texture arrays).
        std::uint64_t number = 0;           // Its place in the video, from 1.
        double seconds = 0.0;               // Since the first frame.
    };

    // Opens `video` to decode it on `device`: on the GPU if made with createGpuDevice(adapter, true), else on the CPU.
    // Reads every frame's time and the keyframes first (without decoding: about 20 ms for a 10 s clip), then decodes
    // the first frame (about 0.1 s to open in all). Throws with a plain message if it cannot be opened or decoded.
    ClipReader(ID3D11Device* device, const std::filesystem::path& video);
    ~ClipReader();

    ClipReader(const ClipReader&) = delete;
    ClipReader& operator=(const ClipReader&) = delete;

    unsigned width() const { return width_; }
    unsigned height() const { return height_; }
    // Every frame's time in seconds from the first, in order: times()[n - 1] is frame n's. Never empty.
    const std::vector<double>& times() const { return times_; }
    // True when frames come straight from the GPU's decoder, false when through the CPU.
    bool onGpu() const { return onGpu_; }

    // Decodes the next frame into `frame`. False after the last. Throws if the video cannot be decoded.
    bool next(Frame& frame);
    // The number of the frame next() gives next.
    std::uint64_t nextNumber() const { return nextNumber_; }
    // Back to the start: the next next() gives frame 1.
    void rewind();
    // Goes to the keyframe at or before frame `number` (1 to times().size()), where decoding can start: the next
    // next() gives it, and the ones after lead to `number`. Returns its number. For skipping ahead, and stepping.
    std::uint64_t seek(std::uint64_t number);
    // The keyframe (a frame that decodes on its own) at or before frame `number`.
    std::uint64_t keyframeAtOrBefore(std::uint64_t number) const;
    // The keyframes' numbers, in order, from 1.
    const std::vector<std::uint64_t>& keyframes() const { return keyframes_; }

private:
    // Keeps Media Foundation started while the reader lives (MFStartup and MFShutdown are counted).
    struct Started {
        Started();
        ~Started();
        Started(const Started&) = delete;
        Started& operator=(const Started&) = delete;
    };

    void open(const std::filesystem::path& video, bool gpu);
    void readType(bool first);
    void keepEveryFrame();
    void close();
    ID3D11Texture2D* fit(ID3D11Texture2D* texture, UINT& subresource);
    ID3D11Texture2D* upload(IMFMediaBuffer* buffer);
    ID3D11Texture2D* ownTexture();

    Started started_; // First, so Media Foundation shuts down after everything below is released.
    ComPtr<ID3D11Device> device_;
    ComPtr<ID3D11DeviceContext> context_;
    ComPtr<IMFDXGIDeviceManager> deviceManager_; // Lends the device to Media Foundation (GPU decoding only).
    ComPtr<IMFSourceReader> reader_;
    ComPtr<IMFSample> sample_;           // The frame handed out last; holds its texture or memory.
    ComPtr<ID3D11Texture2D> decoded_;    // Its texture, on the GPU.
    ComPtr<ID3D11Texture2D> own_;        // For frames that need a copy: cut to size, or uploaded from the CPU.
    std::vector<long long> sampleTimes_; // Every frame's time in the file (100 ns units), in order.
    std::vector<double> times_;
    std::vector<std::uint64_t> keyframes_; // The keyframes' numbers, in order, from 1.
    std::vector<std::uint8_t> rows_;       // A CPU frame turned top row first, when it comes bottom row first.
    long cpuStride_ = 0;           // Bytes from one row of a CPU frame to the next (negative: bottom row first),
    unsigned cpuRows_ = 0;         // and its rows (can be more than the picture's).
    std::uint64_t nextNumber_ = 1; // The number of the frame next() decodes next.
    unsigned width_ = 0;
    unsigned height_ = 0;
    bool onGpu_ = false;
};

// Plays a saved clip's video (clip.mp4, see core/Recordings and engine/ClipRecorder) on its own thread: decodes it
// with the GPU's video decoder (Media Foundation) on its own GPU device and hands each frame to the UI through an
// ImageChannel when its time comes, so the UI never waits for decoding. seek() shows any frame, for a slider and for
// stepping frame by frame.
class ClipPlayer {
public:
    enum class State : std::uint8_t {
        Playing,
        Paused,
        Ended,  // Showed the last frame; play() starts again from the first, seek() makes it Paused.
        Failed, // See error(); e.g. the file cannot be decoded.
    };

    // Opens `video` and starts playing it from its first frame at once. `gpu` is the UI's GPU: the frames reach the UI
    // through frames(), so both devices must be on it. Throws if the video cannot be opened.
    ClipPlayer(IDXGIAdapter1* gpu, const std::filesystem::path& video);
    // Stops. Destroy any reader of frames() first.
    ~ClipPlayer();

    ClipPlayer(const ClipPlayer&) = delete;
    ClipPlayer& operator=(const ClipPlayer&) = delete;

    // The video's size in pixels and length (from its first frame to its last), known once constructed.
    unsigned width() const;
    unsigned height() const;
    double seconds() const;
    // Every frame's time in seconds from the first, in order: times()[n - 1] is frame n's (the same numbers as
    // clip.txt's frame lines and ImageStamp::number). Known once constructed; never empty.
    const std::vector<double>& times() const;
    // The keyframes' numbers in order, from 1: the frames that decode on their own (a clip has one at least every
    // 0.1 s while the screen changes; 0.5 s in clips saved before 2026-11-11 noon). Known once constructed. Seeking
    // one shows it within a few milliseconds, as no frame before it needs decoding; any other frame waits for the ones
    // from the keyframe before it.
    const std::vector<std::uint64_t>& keyframes() const;

    // The frames as they come, for an ImageReader on the UI's device (ScreenCapture::kImageFormat, width() x height()).
    // Each frame's ImageStamp::number is its place in the video, from 1 (clip.txt's frame lines use the same numbers);
    // its shownQpc is unused (0).
    ImageChannel& frames();

    // UI thread. play() goes on from where it was paused or sought, or from the first frame once Ended.
    void play();
    void pause();
    // UI thread. Shows frame `number` (clamped to 1 .. times().size()) as soon as it is decoded, then goes on from it:
    // playing on from there while Playing, else stays on it (Ended becomes Paused). One being decoded is finished and
    // shown first, then the latest asked meanwhile (those between are skipped), so a dragged slider shows a frame each
    // time one is decoded; unless the latest is on the way to it, then that one at once. Never waits.
    // The frame after the one shown comes within a millisecond (the decoder has it ready); any other is decoded from
    // the key frame at or before it, the frames between for nothing (at 1440p on the 4090, 6 ms for a keyframe and
    // about 1.3 ms for each frame after it: 40 ms on average and 90 at most with a keyframe every 0.5 s, the clips
    // before 2026-11-11 noon; --clip-seek in the bench measures it). The one shown already is not sent again.
    void seek(std::uint64_t number);

    State state() const;
    const std::string& error() const; // Valid once state() is Failed.
    // Seconds into the video of the frame handed over last (0 before the first).
    double position() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace observex
