#include "engine/ScreenCapture.h"

namespace observex {

ScreenCapture::ScreenCapture(ID3D11Device* device, IDXGIOutput1* output) : device_(device), output_(output) {
    check(open(), "Desktop Duplication");
}

ScreenCapture::~ScreenCapture() {
    releaseImage();
}

HRESULT ScreenCapture::open() {
    duplication_.Reset();

    // DuplicateOutput1 handles HDR and high-DPI desktops; the older call is the fallback.
    HRESULT result = E_NOINTERFACE;
    ComPtr<IDXGIOutput5> output5;
    if (SUCCEEDED(output_.As(&output5))) {
        const DXGI_FORMAT formats[] = {kImageFormat};
        result = output5->DuplicateOutput1(device_.Get(), 0, 1, formats, &duplication_);
    }
    if (FAILED(result)) {
        result = output_->DuplicateOutput(device_.Get(), &duplication_);
    }
    if (SUCCEEDED(result)) {
        duplication_->GetDesc(&desc_);
    }
    return result;
}

void ScreenCapture::releaseImage() {
    if (holdingImage_) {
        image_.Reset();
        duplication_->ReleaseFrame();
        holdingImage_ = false;
    }
}

ScreenCapture::Status ScreenCapture::acquire(unsigned timeoutMs, Frame& frame) {
    // Microsoft recommends releasing the previous image right before asking for the next one.
    releaseImage();

    DXGI_OUTDUPL_FRAME_INFO info{};
    ComPtr<IDXGIResource> resource;
    const HRESULT result = duplication_->AcquireNextFrame(timeoutMs, &info, &resource);
    if (result == DXGI_ERROR_WAIT_TIMEOUT) {
        return Status::Timeout;
    }
    if (result == DXGI_ERROR_ACCESS_LOST || result == DXGI_ERROR_INVALID_CALL) {
        return Status::Lost;
    }
    if (FAILED(result)) {
        return Status::Failed;
    }
    holdingImage_ = true;

    if (info.LastPresentTime.QuadPart == 0) {
        return Status::NoChange;
    }
    if (FAILED(resource.As(&image_))) {
        return Status::Failed;
    }
    frame.texture = image_.Get();
    frame.presentQpc = info.LastPresentTime.QuadPart;
    frame.updates = info.AccumulatedFrames;
    return Status::NewImage;
}

bool ScreenCapture::reopen() {
    image_.Reset();
    holdingImage_ = false; // The lost duplication is dropped with its image.
    return SUCCEEDED(open());
}

double ScreenCapture::refreshHz() const {
    const DXGI_RATIONAL rate = desc_.ModeDesc.RefreshRate;
    return rate.Denominator != 0 ? static_cast<double>(rate.Numerator) / rate.Denominator : 0.0;
}

} // namespace observex
