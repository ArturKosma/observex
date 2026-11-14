#include "engine/Detector.h"

#include "core/KindLearner.h"
#include "core/ModelInfo.h"
#include "engine/ScreenCapture.h"
#include "engine/TensorRt.h"

#include <cuda_d3d11_interop.h>
#include <d3dcompiler.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>

namespace observex {

namespace {

namespace fs = std::filesystem;

// The names RF-DETR gives its input and outputs.
constexpr char kInputName[] = "input";
constexpr char kBoxesName[] = "dets";    // Per candidate: centre x, centre y, width, height, as shares of the image.
// Per candidate: one score per slot before the sigmoid. Slot 0 is the category, or with kinds one slot per kind in
// model.json's order; then an unused slot.
constexpr char kScoresName[] = "labels";
// Per candidate: what the model's kind classifier reads (tools/train/export.py, since 2026-11-08; not in older ones).
constexpr char kFeaturesName[] = "features";

// Squeezes a BGRA screen image to the model's square input exactly as RF-DETR does in Python: bilinear with pixel
// centres lined up and no smoothing (torch's resize with antialias off, which the training images also got), colours
// 0 to 1, then normalised with the mean and spread of ImageNet. Writes the red, green and blue planes one after
// another (NCHW), as 32-bit floats; mirrored left to right if asked (squeezing is the same both ways round).
constexpr char kSqueezeShader[] = R"(
Texture2D<float4> image : register(t0);
RWBuffer<float> tensor : register(u0);
cbuffer Size : register(b0) {
    float2 scale; // Image pixels per model pixel.
    uint2 last;   // The image's last column and row.
    uint size;    // The model's input is size x size.
    uint mirror;  // 1: writes the image mirrored left to right.
};
static const float3 kMean = float3(0.485, 0.456, 0.406);
static const float3 kSpread = float3(0.229, 0.224, 0.225);

[numthreads(16, 16, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    if (id.x >= size || id.y >= size) {
        return;
    }
    const float2 source = max((float2(id.xy) + 0.5) * scale - 0.5, 0.0);
    const uint2 a = min(uint2(source), last);
    const uint2 b = min(a + 1, last);
    const float2 t = source - float2(a);
    const float3 top = lerp(image.Load(int3(a.x, a.y, 0)).rgb, image.Load(int3(b.x, a.y, 0)).rgb, t.x);
    const float3 bottom = lerp(image.Load(int3(a.x, b.y, 0)).rgb, image.Load(int3(b.x, b.y, 0)).rgb, t.x);
    const float3 rgb = (lerp(top, bottom, t.y) - kMean) / kSpread;
    const uint plane = size * size;
    const uint at = id.y * size + (mirror != 0 ? size - 1 - id.x : id.x);
    tensor[at] = rgb.r;
    tensor[plane + at] = rgb.g;
    tensor[plane * 2 + at] = rgb.b;
}
)";

struct SqueezeConstants {
    float scale[2];
    std::uint32_t last[2];
    std::uint32_t size;
    std::uint32_t mirror;
    std::uint32_t padding[2]; // Constant buffers come in 16-byte steps.
};

} // namespace

struct Detector::Impl {
    unsigned width = 0;
    unsigned height = 0;
    unsigned size = 0;       // The model's input side.
    int candidates = 0;      // Boxes the model gives per image.
    int scoresPerCandidate = 0;
    int featureSize = 0;     // Features per candidate; 0 for a model without them.
    std::vector<std::string> kinds; // From model.json; empty for a model of one category.

    ComPtr<ID3D11Texture2D> image;
    ComPtr<ID3D11ShaderResourceView> imageView;
    ComPtr<ID3D11Buffer> tensor; // The model's input, written by the shader, read by TensorRT through CUDA.
    ComPtr<ID3D11UnorderedAccessView> tensorView;
    ComPtr<ID3D11Buffer> constants;
    ComPtr<ID3D11Buffer> mirroredConstants;
    ComPtr<ID3D11ComputeShader> squeeze;

    cudaStream_t stream = nullptr;
    cudaGraphicsResource_t tensorResource = nullptr;
    void* deviceBoxes = nullptr;
    void* deviceScores = nullptr;
    float* hostBoxes = nullptr; // Page-locked, so the copies back are fast.
    float* hostScores = nullptr;
    void* deviceFeatures = nullptr; // TensorRT writes every output, so they need room even if never copied back.
    float* hostFeatures = nullptr;  // Page-locked, every candidate's; made only for the kind update or features().
    KindUpdate kindUpdate;          // In use when its head has kinds (setKindUpdate checked its shape).
    CudaGraph graph; // The network and the copies back (see runNetwork).

    bool enqueueNetwork(void* input);
    bool runNetwork(void* input) {
        return graph.run(stream, input, [this](void* at) { return enqueueNetwork(at); });
    }
    std::size_t featuresBytes() const {
        return static_cast<std::size_t>(candidates) * static_cast<std::size_t>(featureSize) * sizeof(float);
    }
    // Makes hostFeatures if it is not there yet. False if it could not.
    bool roomForFeatures();

    TensorRtLogger logger;
    std::unique_ptr<nvinfer1::IRuntime> runtime;
    std::unique_ptr<nvinfer1::ICudaEngine> engine;
    std::unique_ptr<nvinfer1::IExecutionContext> context;

    ~Impl() {
        graph.reset();
        context.reset(); // TensorRT first, then the memory it used.
        engine.reset();
        runtime.reset();
        if (tensorResource != nullptr) {
            cudaGraphicsUnregisterResource(tensorResource);
        }
        cudaFree(deviceBoxes);
        cudaFree(deviceScores);
        cudaFree(deviceFeatures);
        cudaFreeHost(hostBoxes);
        cudaFreeHost(hostScores);
        cudaFreeHost(hostFeatures);
        if (stream != nullptr) {
            cudaStreamDestroy(stream);
        }
    }
};

bool Detector::Impl::roomForFeatures() {
    if (hostFeatures == nullptr && featureSize > 0 &&
        cudaMallocHost(reinterpret_cast<void**>(&hostFeatures), featuresBytes()) != cudaSuccess) {
        hostFeatures = nullptr;
    }
    return hostFeatures != nullptr;
}

// Queues the network on `input` and the copies of the candidates back, step by step.
bool Detector::Impl::enqueueNetwork(void* input) {
    const std::size_t boxesBytes = static_cast<std::size_t>(candidates) * 4 * sizeof(float);
    const std::size_t scoresBytes = static_cast<std::size_t>(candidates * scoresPerCandidate) * sizeof(float);
    return context->setTensorAddress(kInputName, input) && context->enqueueV3(stream) &&
           cudaMemcpyAsync(hostBoxes, deviceBoxes, boxesBytes, cudaMemcpyDeviceToHost, stream) == cudaSuccess &&
           cudaMemcpyAsync(hostScores, deviceScores, scoresBytes, cudaMemcpyDeviceToHost, stream) == cudaSuccess;
}

Detector::Detector(ID3D11Device* device, IDXGIAdapter* adapter, const fs::path& modelFolder, unsigned width,
                   unsigned height, std::stop_token stop, std::atomic<float>& progress)
    : impl_(std::make_unique<Impl>()) {
    Impl& d = *impl_;
    d.width = width;
    d.height = height;

    // CUDA on the GPU of the Direct3D device, for this thread.
    int cudaDevice = 0;
    cudaCheck(cudaD3D11GetDevice(&cudaDevice, adapter), "cudaD3D11GetDevice");
    cudaCheck(cudaSetDevice(cudaDevice), "cudaSetDevice");

    // The engine: the saved one if it is newer than the model, else a new one.
    const fs::path onnxFile = modelFolder / L"model.onnx";
    const fs::path engineFile = modelFolder / L"model.engine";
    std::error_code error;
    if (!fs::exists(onnxFile, error)) {
        throw std::runtime_error("No model.onnx in " + toUtf8(modelFolder.c_str()) +
                                 ". tools/train/export.py writes it (train.py does too).");
    }
    if (const fs::path infoFile = modelFolder / L"model.json"; fs::exists(infoFile, error)) {
        const std::vector<char> info = readWholeFile(infoFile);
        d.kinds = parseModelKinds(std::string_view(info.data(), info.size()));
    }
    d.runtime = makeTensorRtRuntime(d.logger);
    d.engine = loadEngine(*d.runtime, onnxFile, engineFile, d.logger, stop, progress);

    // What the model takes and gives: [1, 3, size, size] in, [1, N, 4] boxes and [1, N, C] scores out.
    const nvinfer1::Dims input = d.engine->getTensorShape(kInputName);
    const nvinfer1::Dims boxes = d.engine->getTensorShape(kBoxesName);
    const nvinfer1::Dims scores = d.engine->getTensorShape(kScoresName);
    if (input.nbDims != 4 || input.d[0] != 1 || input.d[1] != 3 || input.d[2] != input.d[3] || boxes.nbDims != 3 ||
        boxes.d[2] != 4 || scores.nbDims != 3 || scores.d[1] != boxes.d[1] || boxes.d[1] > kMaxCandidates ||
        d.engine->getTensorDataType(kInputName) != nvinfer1::DataType::kFLOAT ||
        d.engine->getTensorDataType(kBoxesName) != nvinfer1::DataType::kFLOAT ||
        d.engine->getTensorDataType(kScoresName) != nvinfer1::DataType::kFLOAT) {
        throw std::runtime_error("model.onnx is not an RF-DETR model exported by tools/train/export.py");
    }
    d.size = static_cast<unsigned>(input.d[2]);
    d.candidates = static_cast<int>(boxes.d[1]);
    d.scoresPerCandidate = static_cast<int>(scores.d[2]);
    if (static_cast<int>(d.kinds.size()) > std::min(d.scoresPerCandidate, kMaxKinds)) {
        throw std::runtime_error("model.json names " + std::to_string(d.kinds.size()) + " kinds, but the model has " +
                                 std::to_string(d.scoresPerCandidate) + " scores. Export it again.");
    }
    // And maybe [1, N, F] features.
    if (hasTensor(*d.engine, kFeaturesName)) {
        const nvinfer1::Dims features = d.engine->getTensorShape(kFeaturesName);
        if (features.nbDims != 3 || features.d[1] != boxes.d[1] || features.d[2] <= 0 ||
            d.engine->getTensorDataType(kFeaturesName) != nvinfer1::DataType::kFLOAT) {
            throw std::runtime_error("model.onnx's features are not as tools/train/export.py writes them");
        }
        d.featureSize = static_cast<int>(features.d[2]);
    }
    d.context.reset(d.engine->createExecutionContext());
    if (!d.context) {
        throw std::runtime_error("TensorRT could not make room for the model: " + d.logger.lastError());
    }

    // Direct3D: the image, the model's input buffer, and the shader that fills one from the other.
    D3D11_TEXTURE2D_DESC imageDesc{};
    imageDesc.Width = width;
    imageDesc.Height = height;
    imageDesc.MipLevels = 1;
    imageDesc.ArraySize = 1;
    imageDesc.Format = ScreenCapture::kImageFormat;
    imageDesc.SampleDesc.Count = 1;
    imageDesc.Usage = D3D11_USAGE_DEFAULT;
    imageDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    check(device->CreateTexture2D(&imageDesc, nullptr, &d.image), "CreateTexture2D (detector)");
    check(device->CreateShaderResourceView(d.image.Get(), nullptr, &d.imageView), "CreateShaderResourceView (detector)");

    const UINT values = 3 * d.size * d.size;
    D3D11_BUFFER_DESC tensorDesc{};
    tensorDesc.ByteWidth = values * sizeof(float);
    tensorDesc.Usage = D3D11_USAGE_DEFAULT;
    tensorDesc.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
    check(device->CreateBuffer(&tensorDesc, nullptr, &d.tensor), "CreateBuffer (model input)");
    D3D11_UNORDERED_ACCESS_VIEW_DESC viewDesc{};
    viewDesc.Format = DXGI_FORMAT_R32_FLOAT;
    viewDesc.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
    viewDesc.Buffer.NumElements = values;
    check(device->CreateUnorderedAccessView(d.tensor.Get(), &viewDesc, &d.tensorView),
          "CreateUnorderedAccessView (model input)");

    SqueezeConstants constants{};
    constants.scale[0] = static_cast<float>(width) / static_cast<float>(d.size);
    constants.scale[1] = static_cast<float>(height) / static_cast<float>(d.size);
    constants.last[0] = width - 1;
    constants.last[1] = height - 1;
    constants.size = d.size;
    D3D11_BUFFER_DESC constantsDesc{};
    constantsDesc.ByteWidth = sizeof(constants);
    constantsDesc.Usage = D3D11_USAGE_IMMUTABLE;
    constantsDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    const D3D11_SUBRESOURCE_DATA constantsData{&constants, 0, 0};
    check(device->CreateBuffer(&constantsDesc, &constantsData, &d.constants), "CreateBuffer (detector constants)");
    constants.mirror = 1;
    check(device->CreateBuffer(&constantsDesc, &constantsData, &d.mirroredConstants),
          "CreateBuffer (detector constants, mirrored)");

    ComPtr<ID3DBlob> code;
    ComPtr<ID3DBlob> errors;
    if (FAILED(D3DCompile(kSqueezeShader, sizeof(kSqueezeShader) - 1, "squeeze", nullptr, nullptr, "main", "cs_5_0",
                          D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors))) {
        throw std::runtime_error(std::string("The image shader did not compile: ") +
                                 (errors ? static_cast<const char*>(errors->GetBufferPointer()) : "no reason given"));
    }
    check(device->CreateComputeShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &d.squeeze),
          "CreateComputeShader (detector)");

    // CUDA: a top-priority stream, the input buffer shared from Direct3D, and room for the outputs.
    int lowest = 0;
    int highest = 0;
    cudaCheck(cudaDeviceGetStreamPriorityRange(&lowest, &highest), "cudaDeviceGetStreamPriorityRange");
    cudaCheck(cudaStreamCreateWithPriority(&d.stream, cudaStreamNonBlocking, highest), "cudaStreamCreate");
    cudaCheck(cudaGraphicsD3D11RegisterResource(&d.tensorResource, d.tensor.Get(), cudaGraphicsRegisterFlagsNone),
              "cudaGraphicsD3D11RegisterResource");
    const std::size_t boxesBytes = static_cast<std::size_t>(d.candidates) * 4 * sizeof(float);
    const std::size_t scoresBytes = static_cast<std::size_t>(d.candidates * d.scoresPerCandidate) * sizeof(float);
    cudaCheck(cudaMalloc(&d.deviceBoxes, boxesBytes), "cudaMalloc");
    cudaCheck(cudaMalloc(&d.deviceScores, scoresBytes), "cudaMalloc");
    cudaCheck(cudaMallocHost(reinterpret_cast<void**>(&d.hostBoxes), boxesBytes), "cudaMallocHost");
    cudaCheck(cudaMallocHost(reinterpret_cast<void**>(&d.hostScores), scoresBytes), "cudaMallocHost");
    if (d.featureSize > 0) {
        cudaCheck(cudaMalloc(&d.deviceFeatures,
                             static_cast<std::size_t>(d.candidates) * static_cast<std::size_t>(d.featureSize) *
                                 sizeof(float)),
                  "cudaMalloc");
    }
    if (!d.context->setTensorAddress(kBoxesName, d.deviceBoxes) ||
        !d.context->setTensorAddress(kScoresName, d.deviceScores) ||
        (d.featureSize > 0 && !d.context->setTensorAddress(kFeaturesName, d.deviceFeatures))) {
        throw std::runtime_error("TensorRT refused the output buffers: " + d.logger.lastError());
    }

    // The kind update, if there is one for this model (without one nothing changes).
    if (KindUpdate update = loadKindUpdate(modelFolder); update.head.kinds() > 0) {
        setKindUpdate(std::move(update));
    }
}

Detector::~Detector() = default;

const std::vector<std::string>& Detector::kinds() const {
    return impl_->kinds;
}

ID3D11Texture2D* Detector::input() const {
    return impl_->image.Get();
}

unsigned Detector::modelSize() const {
    return impl_->size;
}

int Detector::featureSize() const {
    return impl_->featureSize;
}

bool Detector::features(std::span<const Detection> found, float* out) {
    Impl& d = *impl_;
    const std::size_t size = static_cast<std::size_t>(d.featureSize);
    if (size == 0 || !d.roomForFeatures()) {
        return false;
    }
    // With a kind update, detect() copied them already.
    if (d.kindUpdate.head.kinds() == 0 &&
        (cudaMemcpyAsync(d.hostFeatures, d.deviceFeatures, d.featuresBytes(), cudaMemcpyDeviceToHost, d.stream) !=
             cudaSuccess ||
         cudaStreamSynchronize(d.stream) != cudaSuccess)) {
        return false;
    }
    for (std::size_t i = 0; i < found.size(); ++i) {
        const float* from = d.hostFeatures + static_cast<std::size_t>(found[i].candidate) * size;
        std::copy(from, from + size, out + i * size);
    }
    return true;
}

const KindUpdate& Detector::kindUpdate() const {
    return impl_->kindUpdate;
}

bool Detector::setKindUpdate(KindUpdate update) {
    Impl& d = *impl_;
    const KindHead& head = update.head;
    if (head.kinds() == 0 || head.kinds() != static_cast<int>(d.kinds.size()) || head.features() != d.featureSize ||
        !d.roomForFeatures()) {
        d.kindUpdate = {};
        return false;
    }
    d.kindUpdate = std::move(update);
    return true;
}

int Detector::detect(ID3D11DeviceContext* context, float minSure, Detection* found, int capacity, bool mirrored) {
    Impl& d = *impl_;

    // Squeeze the image into the model's input on the GPU.
    ID3D11ShaderResourceView* imageView = d.imageView.Get();
    ID3D11UnorderedAccessView* tensorView = d.tensorView.Get();
    ID3D11Buffer* constants = mirrored ? d.mirroredConstants.Get() : d.constants.Get();
    context->CSSetShader(d.squeeze.Get(), nullptr, 0);
    context->CSSetShaderResources(0, 1, &imageView);
    context->CSSetUnorderedAccessViews(0, 1, &tensorView, nullptr);
    context->CSSetConstantBuffers(0, 1, &constants);
    const UINT groups = (d.size + 15) / 16;
    context->Dispatch(groups, groups, 1);
    ID3D11ShaderResourceView* noImage = nullptr;
    ID3D11UnorderedAccessView* noTensor = nullptr;
    context->CSSetShaderResources(0, 1, &noImage);
    context->CSSetUnorderedAccessViews(0, 1, &noTensor, nullptr);
    context->Flush(); // Sends the work now; mapping below makes CUDA wait for it on the GPU.

    // Run the network on it and copy the candidates back.
    if (cudaGraphicsMapResources(1, &d.tensorResource, d.stream) != cudaSuccess) {
        return -1;
    }
    void* input = nullptr;
    std::size_t inputBytes = 0;
    // With a kind update, every candidate's features come back too: one copy on the same stream after the network,
    // outside its CUDA graph, so without one the graph and its copies stay as they were.
    const bool kindUpdate = d.kindUpdate.head.kinds() > 0;
    bool ok = cudaGraphicsResourceGetMappedPointer(&input, &inputBytes, d.tensorResource) == cudaSuccess &&
              d.runNetwork(input) &&
              (!kindUpdate || cudaMemcpyAsync(d.hostFeatures, d.deviceFeatures, d.featuresBytes(),
                                              cudaMemcpyDeviceToHost, d.stream) == cudaSuccess);
    ok = cudaGraphicsUnmapResources(1, &d.tensorResource, d.stream) == cudaSuccess && ok;
    if (cudaStreamSynchronize(d.stream) != cudaSuccess || !ok) {
        return -1;
    }

    // Candidates at least `minSure` sure, as boxes in image pixels (as RF-DETR's PostProcess: clamped to the image),
    // turned back if the input was mirrored. With kinds, how sure is eitherKind of its two surest kinds, which is at
    // most what the surest alone would be if the next were as sure; a score under that limit's inverse sigmoid
    // cannot pass, so most skip the exp.
    const int slots = std::max(1, static_cast<int>(d.kinds.size()));
    const float minFirst = slots > 1 ? 1.0f - std::sqrt(1.0f - minSure) : minSure;
    const float minScore = std::log(minFirst / (1.0f - minFirst));
    const float width = static_cast<float>(d.width);
    const float height = static_cast<float>(d.height);
    std::array<Detection, kMaxCandidates> all;
    int count = 0;
    for (int i = 0; i < d.candidates; ++i) {
        const float* scores = d.hostScores + i * d.scoresPerCandidate;
        const int kind = static_cast<int>(std::max_element(scores, scores + slots) - scores);
        if (scores[kind] < minScore) {
            continue;
        }
        const float kindSure = 1.0f / (1.0f + std::exp(-scores[kind]));
        float sure = kindSure;
        // And the next surest kind, to tell when it is unsure which kind it is (kindsClose).
        int secondKind = -1;
        for (int slot = 0; slot < slots; ++slot) {
            if (slot != kind && (secondKind < 0 || scores[slot] > scores[secondKind])) {
                secondKind = slot;
            }
        }
        const float second = secondKind >= 0 ? 1.0f / (1.0f + std::exp(-scores[secondKind])) : -1.0f;
        if (secondKind >= 0) {
            sure = eitherKind(kindSure, second);
            if (sure < minSure) {
                continue;
            }
        }
        const float* box = d.hostBoxes + i * 4;
        const float centre = mirrored ? 1.0f - box[0] : box[0];
        const float left = std::clamp((centre - box[2] * 0.5f) * width, 0.0f, width);
        const float right = std::clamp((centre + box[2] * 0.5f) * width, 0.0f, width);
        const float top = std::clamp((box[1] - box[3] * 0.5f) * height, 0.0f, height);
        const float bottom = std::clamp((box[1] + box[3] * 0.5f) * height, 0.0f, height);
        Detection& thing = all[static_cast<std::size_t>(count++)];
        thing = {left, top, right - left, bottom - top, sure, static_cast<std::uint8_t>(kind)};
        thing.candidate = static_cast<std::uint16_t>(i);
        if (secondKind >= 0) {
            thing.secondKind = static_cast<std::uint8_t>(secondKind);
            thing.second = second;
            thing.kindSure = kindSure;
        }
    }
    const int kept = std::min(count, capacity);
    std::partial_sort(all.begin(), all.begin() + kept, all.begin() + count,
                      [](const Detection& a, const Detection& b) { return a.sure > b.sure; });
    std::copy(all.begin(), all.begin() + kept, found);
    // Their kinds from the kind update; how sure of a character each is, and so what is kept and its order, as above.
    if (kindUpdate) {
        const std::size_t size = static_cast<std::size_t>(d.featureSize);
        for (int i = 0; i < kept; ++i) {
            Detection& thing = found[i];
            pickKinds(d.kindUpdate.head, {d.hostFeatures + thing.candidate * size, size}, thing);
        }
    }
    return kept;
}

} // namespace observex
