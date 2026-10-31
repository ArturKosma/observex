#include "app/ScreenImage.h"

#include "engine/ScreenCapture.h"

#include <imgui_impl_dx11.h>

#include <cstdint>

namespace observex {

namespace {

// Draw callback: blending off, so the image's undefined alpha cannot make it see-through.
void setOpaqueBlend(const ImDrawList*, const ImDrawCmd* command) {
    const auto* state = static_cast<ImGui_ImplDX11_RenderState*>(ImGui::GetPlatformIO().Renderer_RenderState);
    state->DeviceContext->OMSetBlendState(static_cast<ID3D11BlendState*>(command->UserCallbackData), nullptr,
                                          0xFFFFFFFF);
}

} // namespace

ScreenImage::ScreenImage(ID3D11Device* device, unsigned width, unsigned height) : width_(width), height_(height) {
    device->GetImmediateContext(&context_);

    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = 0; // The full chain, down to 1 x 1.
    desc.ArraySize = 1;
    desc.Format = ScreenCapture::kImageFormat;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET; // GenerateMips needs both.
    desc.MiscFlags = D3D11_RESOURCE_MISC_GENERATE_MIPS;
    check(device->CreateTexture2D(&desc, nullptr, &texture_), "CreateTexture2D (screen image)");
    texture_->GetDesc(&desc);

    check(device->CreateShaderResourceView(texture_.Get(), nullptr, &allLevels_), "CreateShaderResourceView");
    levels_.resize(desc.MipLevels);
    for (UINT level = 0; level < desc.MipLevels; ++level) {
        D3D11_SHADER_RESOURCE_VIEW_DESC view{};
        view.Format = desc.Format;
        view.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        view.Texture2D.MostDetailedMip = level;
        view.Texture2D.MipLevels = 1;
        check(device->CreateShaderResourceView(texture_.Get(), &view, &levels_[level]), "CreateShaderResourceView");
    }

    D3D11_BLEND_DESC blend{};
    blend.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    check(device->CreateBlendState(&blend, &opaque_), "CreateBlendState");
}

void ScreenImage::update(ID3D11Texture2D* image) {
    context_->CopySubresourceRegion(texture_.Get(), 0, 0, 0, 0, image, 0, nullptr);
    imageCopied();
}

void ScreenImage::upload(const void* bgra, unsigned stride) {
    context_->UpdateSubresource(texture_.Get(), 0, nullptr, bgra, stride, 0);
    imageCopied();
}

void ScreenImage::imageCopied() {
    context_->GenerateMips(allLevels_.Get());
    hasImage_ = true;
}

void ScreenImage::draw(ImDrawList* drawList, ImVec2 min, ImVec2 max, float rounding, ImU32 background) const {
    // The smallest version still at least as wide as the rectangle. Bilinear filtering handles the rest.
    std::size_t level = 0;
    const float drawWidth = max.x - min.x;
    while (level + 1 < levels_.size() && static_cast<float>(width_ >> (level + 1)) >= drawWidth) {
        ++level;
    }
    const auto texture = static_cast<ImTextureID>(reinterpret_cast<std::uintptr_t>(levels_[level].Get()));

    drawList->AddCallback(setOpaqueBlend, opaque_.Get());
    drawList->AddImage(texture, min, max);
    drawList->AddCallback(ImGui::GetPlatformIO().DrawCallback_ResetRenderState, nullptr);

    // A band of background along the edges whose inner side is a rounded rectangle covers the corners.
    if (rounding > 0.0f) {
        const float band = rounding * 0.5f + 1.0f; // Just wide enough to reach the corners.
        const float half = band * 0.5f;
        drawList->PathRect(ImVec2(min.x - half, min.y - half), ImVec2(max.x + half, max.y + half), rounding + half);
        drawList->PathStroke(background, ImDrawFlags_Closed, band);
    }
}

} // namespace observex
