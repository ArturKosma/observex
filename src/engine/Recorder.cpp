#include "engine/Recorder.h"

#include "engine/ImageChannel.h"
#include "engine/ImageFile.h"
#include "engine/ScreenCapture.h"

#include <chrono>
#include <cstdio>
#include <exception>
#include <utility>

namespace observex {

namespace {

// How often the recorder looks for a new image. Recording rates are a few per second, so this is plenty.
constexpr auto kPollInterval = std::chrono::milliseconds(5);

} // namespace

Recorder::Recorder(const Screen& screen, ImageChannel& channel, std::filesystem::path folder, unsigned firstNumber)
    : folder_(std::move(folder)), nextNumber_(firstNumber), width_(channel.width()), height_(channel.height()) {
    std::filesystem::create_directories(folder_);

    const Screen own = findOnOwnAdapter(screen); // So this device cannot hold up the pipeline's.
    gpu_ = createGpuDevice(own.adapter.Get());
    reader_ = std::make_unique<ImageReader>(gpu_.device.Get(), channel);

    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = width_;
    desc.Height = height_;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = ScreenCapture::kImageFormat;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_STAGING;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    check(gpu_.device->CreateTexture2D(&desc, nullptr, &staging_), "CreateTexture2D (recording)");

    thread_ = std::jthread([this](std::stop_token stop) { run(stop); });
}

Recorder::~Recorder() {
    if (thread_.joinable()) {
        thread_.request_stop();
        thread_.join();
    }
}

void Recorder::run(std::stop_token stop) {
    // Below normal: the game and the pipeline always come first.
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    SetThreadDescription(GetCurrentThread(), L"observex recorder");
    const bool com = SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED)); // Once, for every image saved.

    while (!stop.stop_requested()) {
        if (!reader_->copyNewImage(gpu_.context.Get(), staging_.Get())) {
            std::this_thread::sleep_for(kPollInterval);
            continue;
        }
        D3D11_MAPPED_SUBRESOURCE mapped{};
        const HRESULT result = gpu_.context->Map(staging_.Get(), 0, D3D11_MAP_READ, 0, &mapped); // Waits for the copy.
        if (FAILED(result)) {
            fail("Could not read the image back from the GPU (the GPU may have been reset)");
            break;
        }
        char name[32];
        std::snprintf(name, sizeof(name), "%06u.jpg", nextNumber_);
        try {
            writeImageFile(folder_ / name, ImageFormat::Jpeg, mapped.pData, width_, height_, mapped.RowPitch);
        } catch (const std::exception& e) {
            gpu_.context->Unmap(staging_.Get(), 0);
            fail(std::string("Could not save ") + toUtf8((folder_ / name).c_str()) + ": " + e.what());
            break;
        }
        gpu_.context->Unmap(staging_.Get(), 0);
        ++nextNumber_;
        saved_.fetch_add(1, std::memory_order_release);
    }

    if (com) {
        CoUninitialize();
    }
}

void Recorder::fail(std::string message) {
    error_ = std::move(message);
    failed_.store(true, std::memory_order_release);
}

} // namespace observex
