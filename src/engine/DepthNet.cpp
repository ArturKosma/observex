#include "engine/DepthNet.h"

#include "engine/TensorRt.h"

#include <cuda_d3d11_interop.h>
#include <d3dcompiler.h>

#include <array>
#include <stdexcept>
#include <string>
#include <system_error>

namespace observex {

namespace {

namespace fs = std::filesystem;

// The names tools/depth/export.py gives the network's input and output.
constexpr char kImageName[] = "image";
constexpr char kDepthName[] = "depth";

// Squeezes a BGRA screen image to the network's input as its image processor does (it shrinks with smoothing, so
// every screen pixel counts): each input pixel the mean of 3 x 3 bilinear samples spread over the screen pixels it
// covers, colours 0 to 1, normalised with ImageNet's mean and spread. Writes the red, green and blue planes one after
// another (NCHW) as 32-bit floats.
constexpr char kSqueezeShader[] = R"(
Texture2D<float4> image : register(t0);
SamplerState smooth : register(s0);
RWBuffer<float> tensor : register(u0);
cbuffer Size : register(b0) {
    float2 step; // One input pixel in the image's 0 to 1 coordinates.
    uint2 size;  // The input's width and height.
};
static const float3 kMean = float3(0.485, 0.456, 0.406);
static const float3 kSpread = float3(0.229, 0.224, 0.225);

[numthreads(16, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    if (id.x >= size.x || id.y >= size.y) {
        return;
    }
    const float2 corner = float2(id.xy) * step;
    float3 sum = 0.0;
    [unroll] for (int j = 0; j < 3; ++j) {
        [unroll] for (int i = 0; i < 3; ++i) {
            sum += image.SampleLevel(smooth, corner + (float2(i, j) + 0.5) / 3.0 * step, 0).rgb;
        }
    }
    const float3 rgb = (sum / 9.0 - kMean) / kSpread;
    const uint plane = size.x * size.y;
    const uint at = id.y * size.x + id.x;
    tensor[at] = rgb.r;
    tensor[plane + at] = rgb.g;
    tensor[plane * 2 + at] = rgb.b;
}
)";

struct SqueezeConstants {
    float step[2];
    std::uint32_t size[2];
};

} // namespace

struct DepthNet::Impl {
    int width = 0; // The map's.
    int height = 0;

    // A view per screen texture it was given (an ImageReader's three), made once each.
    struct View {
        ComPtr<ID3D11Texture2D> texture;
        ComPtr<ID3D11ShaderResourceView> view;
    };
    std::array<View, 8> views;
    ComPtr<ID3D11SamplerState> sampler;
    ComPtr<ID3D11Buffer> tensor; // The network's input, written by the shader, read by TensorRT through CUDA.
    ComPtr<ID3D11UnorderedAccessView> tensorView;
    ComPtr<ID3D11Buffer> constants;
    ComPtr<ID3D11ComputeShader> squeeze;

    cudaStream_t stream = nullptr;
    cudaGraphicsResource_t tensorResource = nullptr;
    void* deviceDepth = nullptr;
    std::uint16_t* hostDepth = nullptr; // Page-locked, so the copy back is fast.
    CudaGraph graph;                    // The network and the copy back.

    TensorRtLogger logger;
    std::unique_ptr<nvinfer1::IRuntime> runtime;
    std::unique_ptr<nvinfer1::ICudaEngine> engine;
    std::unique_ptr<nvinfer1::IExecutionContext> context;

    std::size_t depthBytes() const {
        return static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * sizeof(std::uint16_t);
    }
    bool enqueueNetwork(void* input) {
        return context->setTensorAddress(kImageName, input) && context->enqueueV3(stream) &&
               cudaMemcpyAsync(hostDepth, deviceDepth, depthBytes(), cudaMemcpyDeviceToHost, stream) == cudaSuccess;
    }
    ID3D11ShaderResourceView* viewOf(ID3D11Device* device, ID3D11Texture2D* texture) {
        for (const View& view : views) {
            if (view.texture.Get() == texture) {
                return view.view.Get();
            }
        }
        View* free = nullptr;
        for (View& view : views) {
            if (!view.texture) {
                free = &view;
                break;
            }
        }
        if (free == nullptr) { // More textures than ever expected: start again.
            views = {};
            free = &views[0];
        }
        check(device->CreateShaderResourceView(texture, nullptr, &free->view), "CreateShaderResourceView (depth)");
        free->texture = texture;
        return free->view.Get();
    }

    ~Impl() {
        graph.reset();
        context.reset(); // TensorRT first, then the memory it used.
        engine.reset();
        runtime.reset();
        if (tensorResource != nullptr) {
            cudaGraphicsUnregisterResource(tensorResource);
        }
        cudaFree(deviceDepth);
        cudaFreeHost(hostDepth);
        if (stream != nullptr) {
            cudaStreamDestroy(stream);
        }
    }
};

DepthNet::DepthNet(ID3D11Device* device, IDXGIAdapter* adapter, const fs::path& modelFolder, std::stop_token stop,
                   std::atomic<float>& progress)
    : impl_(std::make_unique<Impl>()) {
    Impl& d = *impl_;

    // CUDA on the GPU of the Direct3D device, for this thread.
    int cudaDevice = 0;
    cudaCheck(cudaD3D11GetDevice(&cudaDevice, adapter), "cudaD3D11GetDevice");
    cudaCheck(cudaSetDevice(cudaDevice), "cudaSetDevice");

    const fs::path onnxFile = modelFolder / L"model.onnx";
    std::error_code error;
    if (!fs::exists(onnxFile, error)) {
        throw std::runtime_error("No model.onnx in " + toUtf8(modelFolder.c_str()) +
                                 ". tools/depth/export.py writes it.");
    }
    d.runtime = makeTensorRtRuntime(d.logger);
    d.engine = loadEngine(*d.runtime, onnxFile, modelFolder / L"model.engine", d.logger, stop, progress);

    // What it takes and gives: [1, 3, height, width] 32-bit in, [1, height, width] half out.
    const nvinfer1::Dims input = d.engine->getTensorShape(kImageName);
    const nvinfer1::Dims depth = d.engine->getTensorShape(kDepthName);
    if (!hasTensor(*d.engine, kImageName) || !hasTensor(*d.engine, kDepthName) || input.nbDims != 4 ||
        input.d[0] != 1 || input.d[1] != 3 || depth.nbDims != 3 || depth.d[0] != 1 || depth.d[1] != input.d[2] ||
        depth.d[2] != input.d[3] || d.engine->getTensorDataType(kImageName) != nvinfer1::DataType::kFLOAT ||
        d.engine->getTensorDataType(kDepthName) != nvinfer1::DataType::kHALF) {
        throw std::runtime_error("models/depth/model.onnx is not a depth network written by tools/depth/export.py");
    }
    d.height = static_cast<int>(input.d[2]);
    d.width = static_cast<int>(input.d[3]);
    d.context.reset(d.engine->createExecutionContext());
    if (!d.context) {
        throw std::runtime_error("TensorRT could not make room for the depth network: " + d.logger.lastError());
    }

    // Direct3D: the network's input buffer, and the shader that fills it from a screen image.
    const UINT values = 3u * static_cast<UINT>(d.width) * static_cast<UINT>(d.height);
    D3D11_BUFFER_DESC tensorDesc{};
    tensorDesc.ByteWidth = values * sizeof(float);
    tensorDesc.Usage = D3D11_USAGE_DEFAULT;
    tensorDesc.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
    check(device->CreateBuffer(&tensorDesc, nullptr, &d.tensor), "CreateBuffer (depth input)");
    D3D11_UNORDERED_ACCESS_VIEW_DESC viewDesc{};
    viewDesc.Format = DXGI_FORMAT_R32_FLOAT;
    viewDesc.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
    viewDesc.Buffer.NumElements = values;
    check(device->CreateUnorderedAccessView(d.tensor.Get(), &viewDesc, &d.tensorView),
          "CreateUnorderedAccessView (depth input)");

    SqueezeConstants constants{};
    constants.step[0] = 1.0f / static_cast<float>(d.width);
    constants.step[1] = 1.0f / static_cast<float>(d.height);
    constants.size[0] = static_cast<std::uint32_t>(d.width);
    constants.size[1] = static_cast<std::uint32_t>(d.height);
    D3D11_BUFFER_DESC constantsDesc{};
    constantsDesc.ByteWidth = sizeof(constants);
    constantsDesc.Usage = D3D11_USAGE_IMMUTABLE;
    constantsDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    const D3D11_SUBRESOURCE_DATA constantsData{&constants, 0, 0};
    check(device->CreateBuffer(&constantsDesc, &constantsData, &d.constants), "CreateBuffer (depth constants)");

    D3D11_SAMPLER_DESC samplerDesc{};
    samplerDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    samplerDesc.AddressU = samplerDesc.AddressV = samplerDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    samplerDesc.MaxLOD = D3D11_FLOAT32_MAX;
    check(device->CreateSamplerState(&samplerDesc, &d.sampler), "CreateSamplerState (depth)");

    ComPtr<ID3DBlob> code;
    ComPtr<ID3DBlob> errors;
    if (FAILED(D3DCompile(kSqueezeShader, sizeof(kSqueezeShader) - 1, "depth squeeze", nullptr, nullptr, "main",
                          "cs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors))) {
        throw std::runtime_error(std::string("The depth image shader did not compile: ") +
                                 (errors ? static_cast<const char*>(errors->GetBufferPointer()) : "no reason given"));
    }
    check(device->CreateComputeShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &d.squeeze),
          "CreateComputeShader (depth)");

    // CUDA: a stream of the lowest priority, the input buffer shared from Direct3D, and room for the map.
    int lowest = 0;
    int highest = 0;
    cudaCheck(cudaDeviceGetStreamPriorityRange(&lowest, &highest), "cudaDeviceGetStreamPriorityRange");
    cudaCheck(cudaStreamCreateWithPriority(&d.stream, cudaStreamNonBlocking, lowest), "cudaStreamCreate");
    cudaCheck(cudaGraphicsD3D11RegisterResource(&d.tensorResource, d.tensor.Get(), cudaGraphicsRegisterFlagsNone),
              "cudaGraphicsD3D11RegisterResource");
    cudaCheck(cudaMalloc(&d.deviceDepth, d.depthBytes()), "cudaMalloc");
    cudaCheck(cudaMallocHost(reinterpret_cast<void**>(&d.hostDepth), d.depthBytes()), "cudaMallocHost");
    if (!d.context->setTensorAddress(kDepthName, d.deviceDepth)) {
        throw std::runtime_error("TensorRT refused the depth output buffer: " + d.logger.lastError());
    }
}

DepthNet::~DepthNet() = default;

int DepthNet::width() const {
    return impl_->width;
}

int DepthNet::height() const {
    return impl_->height;
}

void DepthNet::squeeze(ID3D11DeviceContext* context, ID3D11Texture2D* image) {
    Impl& d = *impl_;
    ComPtr<ID3D11Device> device;
    context->GetDevice(&device);
    ID3D11ShaderResourceView* imageView = d.viewOf(device.Get(), image);
    ID3D11UnorderedAccessView* tensorView = d.tensorView.Get();
    ID3D11Buffer* constants = d.constants.Get();
    ID3D11SamplerState* sampler = d.sampler.Get();
    context->CSSetShader(d.squeeze.Get(), nullptr, 0);
    context->CSSetShaderResources(0, 1, &imageView);
    context->CSSetSamplers(0, 1, &sampler);
    context->CSSetUnorderedAccessViews(0, 1, &tensorView, nullptr);
    context->CSSetConstantBuffers(0, 1, &constants);
    context->Dispatch((static_cast<UINT>(d.width) + 15) / 16, (static_cast<UINT>(d.height) + 7) / 8, 1);
    ID3D11ShaderResourceView* noImage = nullptr;
    ID3D11UnorderedAccessView* noTensor = nullptr;
    context->CSSetShaderResources(0, 1, &noImage);
    context->CSSetUnorderedAccessViews(0, 1, &noTensor, nullptr);
    context->Flush(); // Sends the work now; mapping in run() makes CUDA wait for it on the GPU.
}

const std::uint16_t* DepthNet::run() {
    Impl& d = *impl_;
    if (cudaGraphicsMapResources(1, &d.tensorResource, d.stream) != cudaSuccess) {
        return nullptr;
    }
    void* input = nullptr;
    std::size_t inputBytes = 0;
    bool ok = cudaGraphicsResourceGetMappedPointer(&input, &inputBytes, d.tensorResource) == cudaSuccess &&
              d.graph.run(d.stream, input, [&d](void* at) { return d.enqueueNetwork(at); });
    ok = cudaGraphicsUnmapResources(1, &d.tensorResource, d.stream) == cudaSuccess && ok;
    if (cudaStreamSynchronize(d.stream) != cudaSuccess || !ok) {
        return nullptr;
    }
    return d.hostDepth;
}

} // namespace observex
