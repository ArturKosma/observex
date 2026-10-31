#include "engine/ImageFile.h"

#include "engine/Gpu.h"

#include <wincodec.h>

#include <cstdint>
#include <vector>

namespace observex {

namespace {

// Keeps COM initialised on this thread for the call. Any existing COM mode on the thread is fine for WIC.
class ComScope {
public:
    ComScope() : started_(SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED))) {}
    ~ComScope() {
        if (started_) {
            CoUninitialize();
        }
    }
    ComScope(const ComScope&) = delete;
    ComScope& operator=(const ComScope&) = delete;

private:
    bool started_;
};

} // namespace

void writeImageFile(const std::filesystem::path& path, ImageFormat format, const void* bgra, unsigned width,
                    unsigned height, unsigned stride) {
    // 24-bit BGR: both encoders take it as is, and the files carry no useless alpha.
    const unsigned rowBytes = width * 3;
    std::vector<std::uint8_t> bgr(static_cast<std::size_t>(rowBytes) * height);
    for (unsigned y = 0; y < height; ++y) {
        const std::uint8_t* from = static_cast<const std::uint8_t*>(bgra) + static_cast<std::size_t>(y) * stride;
        std::uint8_t* to = bgr.data() + static_cast<std::size_t>(y) * rowBytes;
        for (unsigned x = 0; x < width; ++x, from += 4, to += 3) {
            to[0] = from[0];
            to[1] = from[1];
            to[2] = from[2];
        }
    }

    const ComScope com;
    ComPtr<IWICImagingFactory> factory;
    check(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory)),
          "WIC factory");
    ComPtr<IWICStream> stream;
    check(factory->CreateStream(&stream), "IWICImagingFactory::CreateStream");
    check(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE), "Open the image file for writing");
    ComPtr<IWICBitmapEncoder> encoder;
    check(factory->CreateEncoder(format == ImageFormat::Jpeg ? GUID_ContainerFormatJpeg : GUID_ContainerFormatPng,
                                 nullptr, &encoder),
          "Image encoder");
    check(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache), "IWICBitmapEncoder::Initialize");

    ComPtr<IWICBitmapFrameEncode> frame;
    ComPtr<IPropertyBag2> options;
    check(encoder->CreateNewFrame(&frame, &options), "IWICBitmapEncoder::CreateNewFrame");
    if (format == ImageFormat::Jpeg) {
        PROPBAG2 option{};
        option.pstrName = const_cast<LPOLESTR>(L"ImageQuality");
        VARIANT value{};
        value.vt = VT_R4;
        value.fltVal = 0.85f;
        check(options->Write(1, &option, &value), "JPEG quality");
    }
    check(frame->Initialize(options.Get()), "IWICBitmapFrameEncode::Initialize");
    check(frame->SetSize(width, height), "IWICBitmapFrameEncode::SetSize");
    WICPixelFormatGUID pixelFormat = GUID_WICPixelFormat24bppBGR;
    check(frame->SetPixelFormat(&pixelFormat), "IWICBitmapFrameEncode::SetPixelFormat");
    if (pixelFormat != GUID_WICPixelFormat24bppBGR) {
        check(E_NOTIMPL, "The image encoder does not take 24-bit BGR");
    }
    check(frame->WritePixels(height, rowBytes, static_cast<UINT>(bgr.size()), bgr.data()),
          "IWICBitmapFrameEncode::WritePixels");
    check(frame->Commit(), "IWICBitmapFrameEncode::Commit");
    check(encoder->Commit(), "IWICBitmapEncoder::Commit");
}

ImageData readImageFile(const std::filesystem::path& path) {
    const ComScope com;
    ComPtr<IWICImagingFactory> factory;
    check(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory)),
          "WIC factory");
    ComPtr<IWICBitmapDecoder> decoder;
    check(factory->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand,
                                             &decoder),
          "Open the image file");
    ComPtr<IWICBitmapFrameDecode> frame;
    check(decoder->GetFrame(0, &frame), "IWICBitmapDecoder::GetFrame");
    ComPtr<IWICFormatConverter> converter;
    check(factory->CreateFormatConverter(&converter), "IWICImagingFactory::CreateFormatConverter");
    check(converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, nullptr, 0.0,
                                WICBitmapPaletteTypeCustom),
          "Convert the image to BGRA");

    ImageData image;
    check(converter->GetSize(&image.width, &image.height), "IWICFormatConverter::GetSize");
    const UINT stride = image.width * 4;
    image.bgra.resize(static_cast<std::size_t>(stride) * image.height);
    check(converter->CopyPixels(nullptr, stride, static_cast<UINT>(image.bgra.size()), image.bgra.data()),
          "Read the image's pixels");
    return image;
}

} // namespace observex
