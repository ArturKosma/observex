#include "app/Snapshot.h"

#include "engine/Gpu.h"
#include "engine/ImageFile.h"

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
        if (rgba) {
            for (UINT x = 0; x < stride; x += 4) {
                std::swap(row[x], row[x + 2]); // To BGRA.
            }
        }
    }
    context->Unmap(staging.Get(), 0);

    writeImageFile(path, ImageFormat::Png, pixels.data(), desc.Width, desc.Height, stride);
}

} // namespace observex
