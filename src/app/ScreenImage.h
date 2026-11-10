#pragma once

#include "engine/Gpu.h"

#include <imgui.h>

#include <vector>

namespace observex {

// A screen image prepared for the UI, which can draw it at any size. Captured images come at full size
// only, so drawn small they look grainy; this copy keeps a chain of halved versions (mipmaps) and draws
// the one closest to the size on screen, which looks smooth. It also draws the image opaque: the alpha
// channel of captured images is undefined.
class ScreenImage {
public:
    // Creates an empty image on the UI's device. Throws on failure.
    ScreenImage(ID3D11Device* device, unsigned width, unsigned height);

    ScreenImage(const ScreenImage&) = delete;
    ScreenImage& operator=(const ScreenImage&) = delete;

    // Copies a captured image (same size, ScreenCapture::kImageFormat) and rebuilds the smaller versions.
    void update(ID3D11Texture2D* image);

    // Copies an image from memory: same size, 8-bit BGRA, rows `stride` bytes apart.
    void upload(const void* bgra, unsigned stride);

    // For copying an image in by other means: the full-size texture (mip 0), and the call to make after.
    ID3D11Texture2D* texture() const { return texture_.Get(); }
    void imageCopied();

    // Whether update() has been called at least once.
    bool hasImage() const { return hasImage_; }

    // Draws the image into the rectangle from `min` to `max`, with corners rounded by `rounding` pixels.
    // `background` must be the colour behind the image: the corners are rounded by painting over them. Only the
    // part from `uvMin` to `uvMax` (0 to 1 across and down the image) if given: the review zoomed in.
    void draw(ImDrawList* drawList, ImVec2 min, ImVec2 max, float rounding, ImU32 background,
              ImVec2 uvMin = ImVec2(0.0f, 0.0f), ImVec2 uvMax = ImVec2(1.0f, 1.0f)) const;

    unsigned width() const { return width_; }
    unsigned height() const { return height_; }

private:
    ComPtr<ID3D11DeviceContext> context_;
    ComPtr<ID3D11Texture2D> texture_;
    ComPtr<ID3D11ShaderResourceView> allLevels_;           // For building the smaller versions.
    std::vector<ComPtr<ID3D11ShaderResourceView>> levels_; // One view per size, full size first.
    ComPtr<ID3D11BlendState> opaque_;
    unsigned width_;
    unsigned height_;
    bool hasImage_ = false;
};

} // namespace observex
