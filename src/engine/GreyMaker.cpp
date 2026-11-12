#include "engine/GreyMaker.h"

#include "engine/ScreenCapture.h"

#include <d3dcompiler.h>

#include <algorithm>
#include <stdexcept>
#include <string>

namespace observex {

namespace {

// BGRA to grey, as GreyImage::fromBgra: (blue * 29 + green * 150 + red * 77) >> 8.
constexpr char kGreyShader[] = R"(
Texture2D<float4> image : register(t0);
RWTexture2D<uint> grey : register(u0);

[numthreads(16, 16, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    uint width, height;
    grey.GetDimensions(width, height);
    if (id.x >= width || id.y >= height) {
        return;
    }
    const uint3 rgb = uint3(round(image.Load(int3(id.xy, 0)).rgb * 255.0));
    grey[id.xy] = (rgb.b * 29 + rgb.g * 150 + rgb.r * 77) >> 8;
}
)";

// One level to the next, half as wide and high, as GreyImage::halve: the rounded mean of each 2 x 2.
constexpr char kHalveShader[] = R"(
Texture2D<uint> above : register(t0);
RWTexture2D<uint> level : register(u0);

[numthreads(16, 16, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    uint width, height;
    level.GetDimensions(width, height);
    if (id.x >= width || id.y >= height) {
        return;
    }
    const int2 at = int2(id.xy) * 2;
    const uint sum = above.Load(int3(at, 0)) + above.Load(int3(at + int2(1, 0), 0)) +
                     above.Load(int3(at + int2(0, 1), 0)) + above.Load(int3(at + int2(1, 1), 0));
    level[id.xy] = (sum + 2) >> 2;
}
)";

ComPtr<ID3D11ComputeShader> compile(ID3D11Device* device, const char* source, std::size_t length, const char* name) {
    ComPtr<ID3DBlob> code;
    ComPtr<ID3DBlob> errors;
    if (FAILED(D3DCompile(source, length, name, nullptr, nullptr, "main", "cs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0,
                          &code, &errors))) {
        throw std::runtime_error(std::string("The grey image shader did not compile: ") +
                                 (errors ? static_cast<const char*>(errors->GetBufferPointer()) : "no reason given"));
    }
    ComPtr<ID3D11ComputeShader> shader;
    check(device->CreateComputeShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &shader),
          "CreateComputeShader (grey image)");
    return shader;
}

} // namespace

GreyMaker::GreyMaker(ID3D11Device* device, unsigned width, unsigned height, int keep)
    : device_(device), kept_(static_cast<std::size_t>(std::max(keep, 1))) {
    D3D11_TEXTURE2D_DESC inputDesc{};
    inputDesc.Width = width;
    inputDesc.Height = height;
    inputDesc.MipLevels = 1;
    inputDesc.ArraySize = 1;
    inputDesc.Format = ScreenCapture::kImageFormat;
    inputDesc.SampleDesc.Count = 1;
    inputDesc.Usage = D3D11_USAGE_DEFAULT;
    inputDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    check(device->CreateTexture2D(&inputDesc, nullptr, &input_), "CreateTexture2D (grey image input)");
    check(device->CreateShaderResourceView(input_.Get(), nullptr, &inputView_), "CreateShaderResourceView (grey input)");

    for (std::size_t l = 0; l < widths_.size(); ++l) {
        widths_[l] = l == 0 ? width : widths_[l - 1] / 2;
        heights_[l] = l == 0 ? height : heights_[l - 1] / 2;
    }
    // The CPU's copies: the levels as one texture's mips (the halvings round down, as a mip chain does).
    D3D11_TEXTURE2D_DESC stagingDesc{};
    stagingDesc.Width = width;
    stagingDesc.Height = height;
    stagingDesc.MipLevels = GreyView::kLevels;
    stagingDesc.ArraySize = 1;
    stagingDesc.Format = DXGI_FORMAT_R8_UINT;
    stagingDesc.SampleDesc.Count = 1;
    stagingDesc.Usage = D3D11_USAGE_STAGING;
    stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    for (Kept& kept : kept_) {
        for (std::size_t l = 0; l < kept.levels.size(); ++l) {
            D3D11_TEXTURE2D_DESC desc{};
            desc.Width = widths_[l];
            desc.Height = heights_[l];
            desc.MipLevels = 1;
            desc.ArraySize = 1;
            desc.Format = DXGI_FORMAT_R8_UINT;
            desc.SampleDesc.Count = 1;
            desc.Usage = D3D11_USAGE_DEFAULT;
            desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
            check(device->CreateTexture2D(&desc, nullptr, &kept.levels[l]), "CreateTexture2D (grey level)");
            check(device->CreateShaderResourceView(kept.levels[l].Get(), nullptr, &kept.levelViews[l]),
                  "CreateShaderResourceView (grey level)");
            check(device->CreateUnorderedAccessView(kept.levels[l].Get(), nullptr, &kept.levelWrites[l]),
                  "CreateUnorderedAccessView (grey level)");
        }
        check(device->CreateTexture2D(&stagingDesc, nullptr, &kept.staging), "CreateTexture2D (grey image copy)");
    }
    grey_ = compile(device, kGreyShader, sizeof(kGreyShader) - 1, "grey");
    halve_ = compile(device, kHalveShader, sizeof(kHalveShader) - 1, "halve");
}

GreyMaker::~GreyMaker() {
    if (context_) {
        for (Kept& kept : kept_) {
            unmap(context_.Get(), kept);
        }
    }
}

void GreyMaker::unmap(ID3D11DeviceContext* context, Kept& kept) {
    if (kept.mapped) {
        for (UINT l = 0; l < GreyView::kLevels; ++l) {
            context->Unmap(kept.staging.Get(), l);
        }
        kept.mapped = false;
    }
}

void GreyMaker::make(ID3D11DeviceContext* context, std::uint64_t image, ID3D11Texture2D* source) {
    context_ = context;
    ID3D11ShaderResourceView* sourceView = inputView_.Get();
    if (source != nullptr) {
        auto known = std::find_if(sources_.begin(), sources_.end(), [&](const Source& s) { return s.texture == source; });
        if (known == sources_.end()) {
            known = std::find_if(sources_.begin(), sources_.end(), [](const Source& s) { return s.texture == nullptr; });
            if (known == sources_.end()) {
                known = sources_.begin(); // More textures than expected: forget the first.
            }
            known->texture = nullptr;
            check(device_->CreateShaderResourceView(source, nullptr, &known->view), "CreateShaderResourceView (grey source)");
            known->texture = source;
        }
        sourceView = known->view.Get();
    }
    Kept& kept = kept_[static_cast<std::size_t>(next_)];
    next_ = (next_ + 1) % static_cast<int>(kept_.size());
    unmap(context, kept);
    kept.copied = false;
    kept.image = image;

    // Grey, then each halving from the level before.
    ID3D11UnorderedAccessView* noWrite = nullptr;
    ID3D11ShaderResourceView* noRead = nullptr;
    for (std::size_t l = 0; l < kept.levels.size(); ++l) {
        context->CSSetUnorderedAccessViews(0, 1, &noWrite, nullptr); // The level before is read now.
        ID3D11ShaderResourceView* read = l == 0 ? sourceView : kept.levelViews[l - 1].Get();
        ID3D11UnorderedAccessView* write = kept.levelWrites[l].Get();
        context->CSSetShader(l == 0 ? grey_.Get() : halve_.Get(), nullptr, 0);
        context->CSSetShaderResources(0, 1, &read);
        context->CSSetUnorderedAccessViews(0, 1, &write, nullptr);
        context->Dispatch((widths_[l] + 15) / 16, (heights_[l] + 15) / 16, 1);
    }
    context->CSSetShaderResources(0, 1, &noRead);
    context->CSSetUnorderedAccessViews(0, 1, &noWrite, nullptr);
}

bool GreyMaker::read(ID3D11DeviceContext* context, std::uint64_t image, std::span<const GreyArea> areas,
                     int levels, int firstLevel) {
    context_ = context;
    const auto kept = std::find_if(kept_.begin(), kept_.end(), [&](const Kept& k) { return k.image == image; });
    if (image == 0 || kept == kept_.end()) {
        return false;
    }
    unmap(context, *kept); // The GPU cannot copy into it while the CPU has it.
    for (const GreyArea& area : areas) {
        for (auto l = static_cast<std::size_t>(std::max(firstLevel, 0));
             l < std::min(kept->levels.size(), static_cast<std::size_t>(std::max(levels, 0))); ++l) {
            const GreyArea part = area.onLevel(static_cast<int>(l), static_cast<int>(widths_[l]),
                                               static_cast<int>(heights_[l]));
            if (part.x0 >= part.x1 || part.y0 >= part.y1) {
                continue;
            }
            const D3D11_BOX box{static_cast<UINT>(part.x0), static_cast<UINT>(part.y0), 0,
                                static_cast<UINT>(part.x1), static_cast<UINT>(part.y1), 1};
            context->CopySubresourceRegion(kept->staging.Get(), static_cast<UINT>(l), box.left, box.top, 0,
                                           kept->levels[l].Get(), 0, &box);
        }
    }
    kept->copied = true;
    return true;
}

void GreyMaker::wait(ID3D11DeviceContext* context) {
    context_ = context;
    for (Kept& kept : kept_) {
        if (!kept.copied) {
            continue;
        }
        kept.copied = false;
        for (std::size_t l = 0; l < kept.levels.size(); ++l) {
            D3D11_MAPPED_SUBRESOURCE mapped{};
            const HRESULT result = context->Map(kept.staging.Get(), static_cast<UINT>(l), D3D11_MAP_READ, 0, &mapped);
            if (FAILED(result)) {
                for (std::size_t done = 0; done < l; ++done) {
                    context->Unmap(kept.staging.Get(), static_cast<UINT>(done));
                }
                check(result, "Map (grey image)"); // The GPU was reset or removed.
            }
            kept.view.levels[l] = {static_cast<const std::uint8_t*>(mapped.pData), static_cast<int>(widths_[l]),
                                   static_cast<int>(heights_[l]), static_cast<std::ptrdiff_t>(mapped.RowPitch)};
        }
        kept.view.image = kept.image;
        kept.mapped = true;
    }
}

const GreyView* GreyMaker::find(std::uint64_t image) const {
    for (const Kept& kept : kept_) {
        if (kept.mapped && image != 0 && kept.image == image) {
            return &kept.view;
        }
    }
    return nullptr;
}

} // namespace observex
