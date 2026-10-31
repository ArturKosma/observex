#include "app/Snapshot.h"

#include "engine/Gpu.h"

#include <wincodec.h>

#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <utility>
#include <vector>

namespace observex {

void savePng(ID3D11Device* device, ID3D11DeviceContext* context, ID3D11Texture2D* texture, const wchar_t* path) {
    D3D11_TEXTURE2D_DESC desc{};
    texture->GetDesc(&desc);
    const bool rgba = desc.Format == DXGI_FORMAT_R8G8B8A8_UNORM || desc.Format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    if (!rgba && desc.Format != DXGI_FORMAT_B8G8R8A8_UNORM && desc.Format != DXGI_FORMAT_B8G8R8A8_UNORM_SRGB) {
        throw std::runtime_error("savePng: unsupported texture format");
    }

    // Read the pixels back through a CPU-readable copy.
    D3D11_TEXTURE2D_DESC stagingDesc = desc;
    stagingDesc.MipLevels = 1;
    stagingDesc.ArraySize = 1;
    stagingDesc.Usage = D3D11_USAGE_STAGING;
    stagingDesc.BindFlags = 0;
    stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    stagingDesc.MiscFlags = 0;
    ComPtr<ID3D11Texture2D> staging;
    check(device->CreateTexture2D(&stagingDesc, nullptr, &staging), "CreateTexture2D (snapshot)");
    context->CopySubresourceRegion(staging.Get(), 0, 0, 0, 0, texture, 0, nullptr);

    D3D11_MAPPED_SUBRESOURCE mapped{};
    check(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped), "ID3D11DeviceContext::Map (snapshot)");
    const UINT stride = desc.Width * 4;
    std::vector<std::uint8_t> pixels(static_cast<std::size_t>(stride) * desc.Height);
    for (UINT y = 0; y < desc.Height; ++y) {
        std::uint8_t* row = pixels.data() + static_cast<std::size_t>(y) * stride;
        std::memcpy(row, static_cast<const std::uint8_t*>(mapped.pData) + static_cast<std::size_t>(y) * mapped.RowPitch,
                    stride);
        for (UINT x = 0; x < stride; x += 4) {
            if (rgba) {
                std::swap(row[x], row[x + 2]); // To BGRA, which every WIC PNG encoder takes.
            }
            row[x + 3] = 255; // The window is opaque, whatever the UI wrote into alpha.
        }
    }
    context->Unmap(staging.Get(), 0);

    const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const bool comStarted = SUCCEEDED(com);
    try {
        ComPtr<IWICImagingFactory> factory;
        check(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory)),
              "WIC factory");
        ComPtr<IWICStream> stream;
        check(factory->CreateStream(&stream), "IWICImagingFactory::CreateStream");
        check(stream->InitializeFromFilename(path, GENERIC_WRITE), "Open the snapshot file");
        ComPtr<IWICBitmapEncoder> encoder;
        check(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder), "PNG encoder");
        check(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache), "IWICBitmapEncoder::Initialize");
        ComPtr<IWICBitmapFrameEncode> frame;
        check(encoder->CreateNewFrame(&frame, nullptr), "IWICBitmapEncoder::CreateNewFrame");
        check(frame->Initialize(nullptr), "IWICBitmapFrameEncode::Initialize");
        check(frame->SetSize(desc.Width, desc.Height), "IWICBitmapFrameEncode::SetSize");
        WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
        check(frame->SetPixelFormat(&format), "IWICBitmapFrameEncode::SetPixelFormat");
        if (format != GUID_WICPixelFormat32bppBGRA) {
            throw std::runtime_error("savePng: the PNG encoder does not take BGRA");
        }
        check(frame->WritePixels(desc.Height, stride, static_cast<UINT>(pixels.size()), pixels.data()),
              "IWICBitmapFrameEncode::WritePixels");
        check(frame->Commit(), "IWICBitmapFrameEncode::Commit");
        check(encoder->Commit(), "IWICBitmapEncoder::Commit");
    } catch (...) {
        if (comStarted) {
            CoUninitialize();
        }
        throw;
    }
    if (comStarted) {
        CoUninitialize();
    }
}

} // namespace observex
