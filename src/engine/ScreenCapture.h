#pragma once

#include "engine/Gpu.h"

#include <cstdint>

namespace observex {

// Grabs new screen images from one monitor with DXGI Desktop Duplication, the fastest capture path on
// Windows. Images stay on the GPU. Windows produces a new image only when something on screen changes,
// and at most once per monitor refresh.
class ScreenCapture {
public:
    // Every image is in this format, whichever duplication API Windows offers.
    static constexpr DXGI_FORMAT kImageFormat = DXGI_FORMAT_B8G8R8A8_UNORM;

    enum class Status {
        NewImage, // `frame` holds a new screen image.
        NoChange, // Only the mouse pointer moved; the image is the same.
        Timeout,  // Nothing changed within the timeout.
        Lost,     // Capture was interrupted (display mode change, fullscreen switch, UAC, lock screen); call reopen().
        Failed,   // Capture stopped for good (e.g. the GPU was removed).
    };

    struct Frame {
        ID3D11Texture2D* texture = nullptr; // Valid until the next acquire() or reopen().
        std::int64_t presentQpc = 0;        // When the screen was updated (QueryPerformanceCounter ticks).
        std::uint32_t updates = 0;          // Screen updates merged into this image; 1 means none were skipped.
    };

    // Starts duplicating `output` on `device` (which must belong to the output's GPU). Throws on failure.
    ScreenCapture(ID3D11Device* device, IDXGIOutput1* output);
    ~ScreenCapture();

    ScreenCapture(const ScreenCapture&) = delete;
    ScreenCapture& operator=(const ScreenCapture&) = delete;

    // Releases the previous image, then waits up to `timeoutMs` for a new one.
    Status acquire(unsigned timeoutMs, Frame& frame);

    // Recreates the capture after Status::Lost. Returns false while the screen cannot be captured yet
    // (e.g. during a UAC prompt); call again a little later.
    bool reopen();

    unsigned width() const { return desc_.ModeDesc.Width; }
    unsigned height() const { return desc_.ModeDesc.Height; }
    double refreshHz() const;

private:
    HRESULT open();
    void releaseImage();

    ComPtr<ID3D11Device> device_;
    ComPtr<IDXGIOutput1> output_;
    ComPtr<IDXGIOutputDuplication> duplication_;
    ComPtr<ID3D11Texture2D> image_;
    bool holdingImage_ = false;
    DXGI_OUTDUPL_DESC desc_{};
};

} // namespace observex
