#pragma once

#include "engine/Gpu.h"

#include <atomic>
#include <filesystem>
#include <memory>
#include <stop_token>

namespace observex {

// Something a model found on a screen image: a box in image pixels, and how sure the model is (0 to 1).
struct Detection {
    float x = 0.0f;
    float y = 0.0f;
    float width = 0.0f;
    float height = 0.0f;
    float sure = 0.0f;
};

// One trained model (models/<category>/model.onnx, written by tools/train) running on the GPU through TensorRT.
// The image never leaves the GPU: a compute shader squeezes it to the model's size into a buffer that CUDA shares
// with Direct3D 11, TensorRT runs the network on that buffer, and only the candidate boxes come back to the CPU.
// Use it from one thread.
class Detector {
public:
    // The most candidates a model gives per image (RF-DETR: 300).
    static constexpr int kMaxCandidates = 300;

    // Loads the model in `modelFolder` for images of `width` x `height` on `device`, which was made from `adapter`.
    // The first time after a new model.onnx, TensorRT builds an engine for this GPU and saves it next to it as
    // model.engine: about a minute or two, during which `progress` goes from 0 to 1 and a stop request ends it.
    // Later starts load the saved engine in a second or two. Throws on failure, saying why.
    Detector(ID3D11Device* device, IDXGIAdapter* adapter, const std::filesystem::path& modelFolder, unsigned width,
             unsigned height, std::stop_token stop, std::atomic<float>& progress);
    ~Detector();

    Detector(const Detector&) = delete;
    Detector& operator=(const Detector&) = delete;

    // The texture to put each image in before detect(): BGRA, the size given above, on the device given above.
    ID3D11Texture2D* input() const;

    // Runs the model on input(). Writes the things it found at least `minSure` sure into `found`, surest first, at
    // most `capacity`, and returns how many; -1 if the GPU failed. Waits for the GPU. No heap allocation.
    int detect(ID3D11DeviceContext* context, float minSure, Detection* found, int capacity);

    // The model's input size: images are squeezed to this many pixels square.
    unsigned modelSize() const;

private:
    struct Impl; // Keeps TensorRT and CUDA out of this header.
    std::unique_ptr<Impl> impl_;
};

} // namespace observex
