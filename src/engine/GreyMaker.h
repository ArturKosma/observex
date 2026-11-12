#pragma once

#include "core/Heuristics.h"
#include "engine/Gpu.h"

#include <array>
#include <cstdint>
#include <span>
#include <vector>

namespace observex {

// Grey copies of screen images and their halvings (core/Heuristics' GreyView) for snapping (engine/BoxSnapper; the
// bench's --clip-guesses --snap) and for finding the game's interface (engine/UiFinder), made on the GPU: a compute shader turns the BGRA image grey and halves it three
// times, exactly as heuristics/Snapper's GreyImage does on the CPU. Only the parts the snapping reads (GreyArea) are
// then copied to memory the CPU can read: copying all four levels back (about 4.9 MB for a 2560 x 1440 screen) slowed
// each of the model's looks from 3.3 to 5.1 ms, as the copies hold up the GPU it runs on, while the shaders cost it
// nothing (2026-11-11; through CUDA's copy engine instead, the copies waited behind the model's whole look). On the
// CPU making them took 2.8 ms an image, most of it copying the BGRA image across first (14.7 MB). The last few
// images (kKept for the snapper) stay on the GPU and readable, so a look a few images late can still learn from its
// own image. Use it from one thread, with the device's immediate context.
class GreyMaker {
public:
    static constexpr int kKept = 8;

    // On `device`, for images of `width` x `height`, keeping the last `keep`. Throws on failure.
    GreyMaker(ID3D11Device* device, unsigned width, unsigned height, int keep = kKept);
    ~GreyMaker();

    GreyMaker(const GreyMaker&) = delete;
    GreyMaker& operator=(const GreyMaker&) = delete;

    // Where the next image goes, as BGRA (ScreenCapture::kImageFormat) of the size given, e.g. as
    // ImageReader::copyNewImage's target.
    ID3D11Texture2D* input() const { return input_.Get(); }

    // Makes the grey levels of what input() holds (or `source`, a BGRA texture of that size on the device that a
    // shader can read, e.g. ImageReader::useNewImage's), as screen image `image`, on the GPU (none readable yet). It
    // replaces the oldest kept. Throws on failure.
    void make(ID3D11DeviceContext* context, std::uint64_t image, ID3D11Texture2D* source = nullptr);

    // Copies `areas` of image `image` (on its first `levels` levels: all, or e.g. 1 for the full-size one alone; from
    // `firstLevel` on, e.g. 1 for all but the full-size one) to where the CPU reads them, once wait() is called. False
    // if the image is not kept any more.
    bool read(ID3D11DeviceContext* context, std::uint64_t image, std::span<const GreyArea> areas,
              int levels = GreyView::kLevels, int firstLevel = 0);

    // Waits for the GPU until everything read() since the last wait is readable. Throws if the GPU was lost.
    void wait(ID3D11DeviceContext* context);

    // The levels of image `image` once read, if it is still kept (parts not read hold anything), else null. Valid
    // until the next read() or make().
    const GreyView* find(std::uint64_t image) const;

private:
    struct Kept {
        std::array<ComPtr<ID3D11Texture2D>, GreyView::kLevels> levels; // On the GPU.
        std::array<ComPtr<ID3D11ShaderResourceView>, GreyView::kLevels> levelViews;
        std::array<ComPtr<ID3D11UnorderedAccessView>, GreyView::kLevels> levelWrites;
        ComPtr<ID3D11Texture2D> staging; // The four levels as mips, for the CPU.
        std::uint64_t image = 0;         // 0: none.
        bool copied = false;             // Read since it was last mapped: to map on wait().
        bool mapped = false;
        GreyView view; // While mapped.
    };

    void unmap(ID3D11DeviceContext* context, Kept& kept);

    ComPtr<ID3D11Device> device_;
    ComPtr<ID3D11DeviceContext> context_; // The one last used, to unmap.
    ComPtr<ID3D11Texture2D> input_;
    ComPtr<ID3D11ShaderResourceView> inputView_;
    struct Source {
        ID3D11Texture2D* texture = nullptr; // Not held: its owner keeps it while it may come again.
        ComPtr<ID3D11ShaderResourceView> view;
    };
    std::array<Source, 4> sources_; // The textures given to make() (an image channel has three), and their views.
    std::array<unsigned, GreyView::kLevels> widths_{};
    std::array<unsigned, GreyView::kLevels> heights_{};
    ComPtr<ID3D11ComputeShader> grey_;
    ComPtr<ID3D11ComputeShader> halve_;
    std::vector<Kept> kept_;
    int next_ = 0;
};

} // namespace observex
