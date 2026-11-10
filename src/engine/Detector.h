#pragma once

#include "core/Detection.h"
#include "core/ModelInfo.h"
#include "engine/Gpu.h"

#include <atomic>
#include <filesystem>
#include <memory>
#include <span>
#include <stop_token>
#include <string>
#include <vector>

namespace observex {

// One trained model (models/<category>/model.onnx, written by tools/train) running on the GPU through TensorRT.
// The image never leaves the GPU: a compute shader squeezes it to the model's size into a buffer that CUDA shares
// with Direct3D 11, TensorRT runs the network on that buffer, and only the candidate boxes come back to the CPU.
// With a kind update (kinds-update.json next to the model, valid for it: core/ModelInfo's loadKindUpdate), each found
// thing's kinds come from the update's classifier (core/KindLearner's pickKinds); how sure it is of a character, and
// so what it finds and in which order, stays the network's. That costs one more copy back (every candidate's
// features) and a few dot products per found thing. Use it from one thread.
class Detector {
public:
    // The most candidates a model gives per image (RF-DETR: 300).
    static constexpr int kMaxCandidates = 300;

    // Loads the model in `modelFolder` for images of `width` x `height` on `device`, which was made from `adapter`.
    // The first time after a new model.onnx, TensorRT builds an engine for this GPU and saves it next to it as
    // model.engine: about 35 s on an RTX 4090, during which `progress` goes from 0 to 1 and a stop request ends it.
    // Later starts load the saved engine in under a second. Detectors load or prepare one at a time: a second one
    // waits (a stop request ends the wait). Also loads the kind update there, if there is a valid one. Throws on
    // failure, saying why.
    Detector(ID3D11Device* device, IDXGIAdapter* adapter, const std::filesystem::path& modelFolder, unsigned width,
             unsigned height, std::stop_token stop, std::atomic<float>& progress);
    ~Detector();

    Detector(const Detector&) = delete;
    Detector& operator=(const Detector&) = delete;

    // The texture to put each image in before detect(): BGRA, the size given above, on the device given above.
    ID3D11Texture2D* input() const;

    // Runs the model on input(), or on its mirror image left to right if `mirrored` (the boxes still come in
    // input()'s own pixels). Writes the things it found at least `minSure` sure into `found`, surest first, at most
    // `capacity`, and returns how many; -1 if the GPU failed. Waits for the GPU. No heap allocation.
    int detect(ID3D11DeviceContext* context, float minSure, Detection* found, int capacity, bool mirrored = false);

    // How many numbers per candidate the model's kind classifier reads (the "features" output of a model.onnx
    // exported since 2026-11-08, 256 for RF-DETR Medium); 0 for an older model, which has none.
    int featureSize() const;
    // Copies the features of `found` (things the last detect() found) into `out`, featureSize() numbers each, in
    // their order. Waits for the GPU, unless detect() already copied them for the kind update. False if the GPU failed
    // or the model has none. Only for the finder: live recognition never copies them without a kind update (the
    // network writes them on the GPU either way, at no measurable cost).
    bool features(std::span<const Detection> found, float* out);

    // The kind update it picks kinds with; its head is empty when it uses none (the network's own kinds).
    const KindUpdate& kindUpdate() const;
    // Picks kinds with `update` from the next detect() on, or with the network's own if its head is empty or not of
    // the model's kinds and features (then it is dropped). Allocates (room for every candidate's features, page-locked,
    // once); call it between looks, not on the hot path. True if it uses the update.
    bool setKindUpdate(KindUpdate update);

    // The model's input size: images are squeezed to this many pixels square.
    unsigned modelSize() const;

    // The kinds it tells apart (model.json, e.g. "monster", "player", "npc", "own"): each Detection::kind indexes
    // this. Empty for a model trained on one category (then every kind is 0).
    const std::vector<std::string>& kinds() const;

private:
    struct Impl; // Keeps TensorRT and CUDA out of this header.
    std::unique_ptr<Impl> impl_;
};

} // namespace observex
