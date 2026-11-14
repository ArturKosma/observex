#pragma once

#include <NvInfer.h>
#include <cuda_runtime_api.h>

#include <atomic>
#include <filesystem>
#include <memory>
#include <mutex>
#include <stop_token>
#include <string>
#include <vector>

namespace observex {

// TensorRT for the networks observex runs on the GPU: the model (engine/Detector) and depth (engine/DepthNet).
// TensorRT's DLLs are loaded at run time, from next to observex.exe or from deps/ (cmake/TensorRT.cmake), and only
// through the few C functions they export, so observex needs no import libraries and runs (without them) where
// TensorRT is missing. Only for the engine's .cpp files: this header brings TensorRT's and CUDA's in.

// Keeps TensorRT's last error for the messages thrown, and shows warnings in a debugger's output.
class TensorRtLogger final : public nvinfer1::ILogger {
public:
    void log(Severity severity, const char* message) noexcept override;
    std::string lastError() const;

private:
    mutable std::mutex mutex_; // TensorRT may log from its own threads while building.
    std::string lastError_;
};

// Throws "`what` failed: why" unless `result` is cudaSuccess.
void cudaCheck(cudaError_t result, const char* what);

// The bytes of `path`. Throws if it cannot be read.
std::vector<char> readWholeFile(const std::filesystem::path& path);

// A TensorRT runtime logging to `logger`. Loads TensorRT the first time. Throws if it cannot.
std::unique_ptr<nvinfer1::IRuntime> makeTensorRtRuntime(TensorRtLogger& logger);

// The engine of `onnxFile` for this GPU: `engineFile` if it is there and newer, else one built from the ONNX file
// (the precision is the file's: TensorRT 11 takes it from there) and saved as `engineFile`, while `progress` goes from
// 0 to 1; a stop request ends the build. Networks load or build one at a time per process: a second one waits (a stop
// request ends the wait), then loads the engine the first saved instead of building it again. Throws on failure.
std::unique_ptr<nvinfer1::ICudaEngine> loadEngine(nvinfer1::IRuntime& runtime, const std::filesystem::path& onnxFile,
                                                  const std::filesystem::path& engineFile, TensorRtLogger& logger,
                                                  std::stop_token stop, std::atomic<float>& progress);

// Whether `engine` has an input or output called `name`.
bool hasTensor(const nvinfer1::ICudaEngine& engine, const char* name);

// Queues a network's run as one CUDA graph: a single launch replays its hundreds of GPU steps, so the CPU does not
// launch each one and the GPU does not wait between them. Recorded on the first run (after a plain one, which TensorRT
// needs first) and again only if the input moves; if recording fails, every run goes step by step.
class CudaGraph {
public:
    CudaGraph() = default;
    ~CudaGraph() { reset(); }

    CudaGraph(const CudaGraph&) = delete;
    CudaGraph& operator=(const CudaGraph&) = delete;

    // Queues `enqueue(input)` on `stream` (a callable that queues the steps and returns false if one fails), or the
    // graph recorded from it. False if the steps failed.
    template <typename Enqueue> bool run(cudaStream_t stream, void* input, const Enqueue& enqueue) {
        if (graph_ != nullptr && input == input_) {
            return cudaGraphLaunch(graph_, stream) == cudaSuccess;
        }
        if (!enqueue(input)) {
            return false;
        }
        if (noGraph_) {
            return true;
        }
        reset();
        // Recording only notes the steps; this run's results come from the plain run above.
        cudaGraph_t recorded = nullptr;
        bool ok = cudaStreamBeginCapture(stream, cudaStreamCaptureModeThreadLocal) == cudaSuccess;
        if (ok) {
            ok = enqueue(input);
            ok = cudaStreamEndCapture(stream, &recorded) == cudaSuccess && ok && recorded != nullptr;
        }
        ok = ok && cudaGraphInstantiate(&graph_, recorded, 0) == cudaSuccess;
        if (recorded != nullptr) {
            cudaGraphDestroy(recorded);
        }
        if (ok) {
            input_ = input;
        } else {
            cudaGetLastError(); // Clears the failure; the plain run above already worked.
            graph_ = nullptr;
            noGraph_ = true;
        }
        return true;
    }

    void reset() {
        if (graph_ != nullptr) {
            cudaGraphExecDestroy(graph_);
            graph_ = nullptr;
        }
    }

private:
    cudaGraphExec_t graph_ = nullptr;
    void* input_ = nullptr;
    bool noGraph_ = false;
};

} // namespace observex
