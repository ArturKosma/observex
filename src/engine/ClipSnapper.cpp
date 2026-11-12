#include "engine/ClipSnapper.h"

#include "engine/ClipPlayer.h"
#include "engine/GreyMaker.h"

#include <algorithm>
#include <exception>
#include <utility>

namespace observex {

namespace {

// The grey images of a replay, made from the clip's video as the snapper makes them from the screen. The video is
// decoded in order once: each image comes from its own frame, or the frame before when the video lacks it (and from
// the first frame before the video starts).
class VideoImages final : public SnapImages {
public:
    VideoImages(ID3D11Device* device, const std::filesystem::path& video, const ClipText& clip)
        : reader_(device, video), greys_(device, reader_.width(), reader_.height()), clip_(clip) {
        device->GetImmediateContext(&context_);
    }

    void make(unsigned long long image) override {
        const std::size_t frames = std::min(clip_.frameImage.size(), reader_.times().size());
        while (taken_ < frames && (taken_ == 0 || clip_.frameImage[taken_] <= image)) {
            ClipReader::Frame frame;
            if (!reader_.next(frame)) {
                taken_ = frames;
                break;
            }
            context_->CopySubresourceRegion(greys_.input(), 0, 0, 0, 0, frame.texture, frame.subresource, nullptr);
            ++taken_;
        }
        greys_.make(context_.Get(), image);
    }
    void read(unsigned long long image, std::span<const GreyArea> areas) override {
        greys_.read(context_.Get(), image, areas);
    }
    void wait() override { greys_.wait(context_.Get()); }
    const GreyView* find(unsigned long long image) const override { return greys_.find(image); }

private:
    ClipReader reader_;
    GreyMaker greys_;
    const ClipText& clip_;
    ComPtr<ID3D11DeviceContext> context_;
    std::size_t taken_ = 0; // Video frames copied in so far.
};

static_assert(SnapImages::kKept == GreyMaker::kKept);

} // namespace

ClipSnapper::ClipSnapper(IDXGIAdapter1* gpu, std::filesystem::path video, std::shared_ptr<const ClipText> clip,
                         Guesses today, std::unique_ptr<Snapping> snapping)
    : video_(std::move(video)), clip_(std::move(clip)), today_(std::move(today)), snapping_(std::move(snapping)) {
    thread_ = std::jthread([this, adapter = ComPtr<IDXGIAdapter1>(gpu)](std::stop_token stop) { run(stop, adapter); });
}

ClipSnapper::~ClipSnapper() {
    if (thread_.joinable()) {
        thread_.request_stop();
        thread_.join();
    }
}

Guesses ClipSnapper::snap(ID3D11Device* device, const std::filesystem::path& video, const ClipText& clip,
                          const Guesses& today, Snapping& snapping, std::stop_token stop, std::atomic<float>* progress) {
    VideoImages images(device, video, clip);
    return snapGuesses(clip, today, snapping, images, stop, progress);
}

void ClipSnapper::run(std::stop_token stop, ComPtr<IDXGIAdapter1> gpu) {
    SetThreadDescription(GetCurrentThread(), L"observex clip snapper");
    // Below the UI and the player: it only has to be done in a few seconds.
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    const bool com = SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED)); // Media Foundation needs it.
    try {
        const GpuDevice device = createGpuDevice(gpu.Get(), true);
        guesses_ = snap(device.device.Get(), video_, *clip_, today_, *snapping_, stop, &progress_);
        state_.store(State::Done, std::memory_order_release);
    } catch (const std::exception& e) {
        error_ = e.what();
        state_.store(State::Failed, std::memory_order_release);
    }
    if (com) {
        CoUninitialize();
    }
}

} // namespace observex
