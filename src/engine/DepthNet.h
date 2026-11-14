#pragma once

#include "engine/Gpu.h"

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <stop_token>

namespace observex {

// The depth network (models/depth/model.onnx, written by tools/depth/export.py: Depth Anything V2 Small) on the GPU
// through TensorRT, for the Show flag Depth. From one screen image it guesses how near each pixel is: relative inverse
// depth, larger nearer, with a scale and offset of each image's own (core/Depth). Like engine/Detector the image never
// leaves the GPU: a compute shader squeezes it to the network's input (an average of 3 x 3 samples per input pixel, as
// the network's own image processor smooths a screen it shrinks), TensorRT runs the network on a buffer CUDA shares
// with Direct3D 11, and only the map comes back (half precision, 924 x 518: 0.96 MB). Its CUDA stream has the lowest
// priority, so the model's looks (engine/Detector, the highest) go first on the GPU. Use it from one thread.
class DepthNet {
public:
    // Loads the network in `modelFolder` on `device`, which was made from `adapter`. The first time after a new
    // model.onnx, TensorRT builds an engine for this GPU and saves it next to it as model.engine (about a minute),
    // while `progress` goes from 0 to 1; a stop request ends it. Throws on failure.
    DepthNet(ID3D11Device* device, IDXGIAdapter* adapter, const std::filesystem::path& modelFolder,
             std::stop_token stop, std::atomic<float>& progress);
    ~DepthNet();

    DepthNet(const DepthNet&) = delete;
    DepthNet& operator=(const DepthNet&) = delete;

    // The map's size (the network's input size).
    int width() const;
    int height() const;

    // Queues squeezing `image` (BGRA, any size, on the device given above) into the network's input: stretched to
    // it, so a screen of another shape than the input's (16:9) comes out stretched. The image may be handed back
    // (ImageReader::endUse) as soon as this returns.
    void squeeze(ID3D11DeviceContext* context, ID3D11Texture2D* image);
    // Runs the network on what squeeze() queued and copies its map back: width() x height() half-precision floats,
    // row by row, valid until the next run. Waits for the GPU. Null if the GPU failed.
    const std::uint16_t* run();

private:
    struct Impl; // Keeps TensorRT and CUDA out of this header.
    std::unique_ptr<Impl> impl_;
};

} // namespace observex
