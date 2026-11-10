#include "engine/ClipPlayer.h"

#include "engine/ScreenCapture.h"

#include <d3d11_4.h> // ID3D11Multithread.
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <exception>
#include <future>
#include <stdexcept>
#include <thread>
#include <utility>

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

namespace observex {

namespace {

constexpr DWORD kVideo = static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM);
constexpr double kTicksPerSecond = 1e7; // Media Foundation's times are in 100 ns units.

std::int64_t qpcNow() {
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return now.QuadPart;
}

double qpcPerSecond() {
    LARGE_INTEGER frequency;
    QueryPerformanceFrequency(&frequency);
    return static_cast<double>(frequency.QuadPart);
}

ComPtr<IMFSourceReader> openSourceReader(const std::filesystem::path& video, IMFAttributes* attributes) {
    ComPtr<IMFSourceReader> reader;
    const HRESULT result = MFCreateSourceReaderFromURL(video.c_str(), attributes, &reader);
    if (FAILED(result)) {
        char why[64];
        std::snprintf(why, sizeof(why), " (0x%08lX)", static_cast<unsigned long>(result));
        if (result == MF_E_UNSUPPORTED_BYTESTREAM_TYPE) {
            std::snprintf(why, sizeof(why), ": not a video file");
        } else if (result == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND) ||
                   result == HRESULT_FROM_WIN32(ERROR_PATH_NOT_FOUND)) {
            std::snprintf(why, sizeof(why), ": no such file");
        }
        throw std::runtime_error("Cannot open " + toUtf8(video.c_str()) + why);
    }
    check(reader->SetStreamSelection(static_cast<DWORD>(MF_SOURCE_READER_ALL_STREAMS), FALSE), "SetStreamSelection");
    if (FAILED(reader->SetStreamSelection(kVideo, TRUE))) {
        throw std::runtime_error(toUtf8(video.c_str()) + " holds no video");
    }
    return reader;
}

// Every frame's time in the file (100 ns units), in the order they are shown, and the keyframes' (where decoding can
// start), read from the file without decoding.
std::vector<long long> readSampleTimes(const std::filesystem::path& video, std::vector<long long>& keyframeTimes) {
    const ComPtr<IMFSourceReader> reader = openSourceReader(video, nullptr);
    std::vector<long long> times;
    for (;;) {
        DWORD flags = 0;
        LONGLONG time = 0;
        ComPtr<IMFSample> sample;
        check(reader->ReadSample(kVideo, 0, nullptr, &flags, &time, &sample), "Reading the video (ReadSample)");
        if ((flags & MF_SOURCE_READERF_ERROR) != 0) {
            throw std::runtime_error("The video cannot be read to its end");
        }
        if ((flags & MF_SOURCE_READERF_ENDOFSTREAM) != 0) {
            break;
        }
        if (sample) {
            times.push_back(time);
            if (MFGetAttributeUINT32(sample.Get(), MFSampleExtension_CleanPoint, FALSE) != FALSE) {
                keyframeTimes.push_back(time);
            }
        }
    }
    if (times.empty()) {
        throw std::runtime_error(toUtf8(video.c_str()) + " holds no frames");
    }
    std::sort(times.begin(),
              times.end()); // Stored in decoding order, which B-frames change (the clip buffer's have none).
    return times;
}

} // namespace

ClipReader::Started::Started() {
    check(MFStartup(MF_VERSION, MFSTARTUP_LITE), "MFStartup");
}

ClipReader::Started::~Started() {
    MFShutdown();
}

ClipReader::ClipReader(ID3D11Device* device, const std::filesystem::path& video) : device_(device) {
    device_->GetImmediateContext(&context_);

    std::vector<long long> keyframeTimes;
    sampleTimes_ = readSampleTimes(video, keyframeTimes);
    times_.reserve(sampleTimes_.size());
    for (const long long time : sampleTimes_) {
        times_.push_back(static_cast<double>(time - sampleTimes_.front()) / kTicksPerSecond);
    }
    keyframes_.push_back(1); // Decoding can always start at the start.
    for (const long long time : keyframeTimes) {
        const auto number = static_cast<std::uint64_t>(
            std::lower_bound(sampleTimes_.begin(), sampleTimes_.end(), time) - sampleTimes_.begin() + 1);
        if (number > keyframes_.back()) {
            keyframes_.push_back(number);
        }
    }

    // The GPU's decoder first; if it cannot decode the first frame, the CPU's. Media Foundation also works the device
    // from its own threads, so only a device made for it (createGpuDevice(adapter, true)) decodes on the GPU; a plain
    // one decodes on the CPU (a test that gave the GPU's decoder a plain device hung).
    ComPtr<ID3D11VideoDevice> videoDevice;
    ComPtr<ID3D11Multithread> multithread;
    const bool gpuCanDecode = SUCCEEDED(device_.As(&videoDevice)) && SUCCEEDED(device_.As(&multithread)) &&
                              multithread->GetMultithreadProtected();
    std::string gpuError = "the device has no video support";
    for (const bool gpu : {true, false}) {
        if (gpu && !gpuCanDecode) {
            continue;
        }
        try {
            open(video, gpu);
            Frame first;
            if (!next(first)) {
                throw std::runtime_error("no frame came out of the decoder");
            }
            rewind();
            onGpu_ = gpu;
            return;
        } catch (const std::exception& e) {
            close();
            if (gpu) {
                gpuError = e.what();
                continue;
            }
            throw std::runtime_error("The video " + toUtf8(video.c_str()) + " cannot be decoded: " + e.what() +
                                     " (on the GPU: " + gpuError + ")");
        }
    }
}

ClipReader::~ClipReader() {
    close();
}

void ClipReader::open(const std::filesystem::path& video, bool gpu) {
    ComPtr<IMFAttributes> attributes;
    check(MFCreateAttributes(&attributes, 4), "MFCreateAttributes");
    if (gpu) {
        UINT token = 0;
        check(MFCreateDXGIDeviceManager(&token, &deviceManager_), "MFCreateDXGIDeviceManager");
        check(deviceManager_->ResetDevice(device_.Get(), token), "IMFDXGIDeviceManager::ResetDevice");
        check(attributes->SetUnknown(MF_SOURCE_READER_D3D_MANAGER, deviceManager_.Get()),
              "MF_SOURCE_READER_D3D_MANAGER");
        check(attributes->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, TRUE),
              "MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS");
        // Converts the decoder's YUV frames to BGRA on the GPU (Direct3D 11's video processor).
        check(attributes->SetUINT32(MF_SOURCE_READER_ENABLE_ADVANCED_VIDEO_PROCESSING, TRUE),
              "MF_SOURCE_READER_ENABLE_ADVANCED_VIDEO_PROCESSING");
    } else {
        check(attributes->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE),
              "MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING");
    }
    reader_ = openSourceReader(video, attributes.Get());

    ComPtr<IMFMediaType> type;
    check(MFCreateMediaType(&type), "MFCreateMediaType");
    check(type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video), "MF_MT_MAJOR_TYPE");
    // ARGB32 is DXGI_FORMAT_B8G8R8A8_UNORM (ScreenCapture::kImageFormat); the CPU's converter makes RGB32 (the same
    // bytes, the fourth unused).
    check(type->SetGUID(MF_MT_SUBTYPE, gpu ? MFVideoFormat_ARGB32 : MFVideoFormat_RGB32), "MF_MT_SUBTYPE");
    check(reader_->SetCurrentMediaType(kVideo, nullptr, type.Get()), "Choosing BGRA frames (SetCurrentMediaType)");
    readType(true);
}

void ClipReader::keepEveryFrame() {
    // The GPU's video processor would make the frame rate even, repeating and skipping frames of a variable frame
    // rate video (2026-11-11: of 1499 frames it repeated 501 and skipped 390, and gave them times of its own).
    ComPtr<IMFSourceReaderEx> readerEx;
    check(reader_.As(&readerEx), "IMFSourceReaderEx");
    bool found = false;
    GUID category{};
    ComPtr<IMFTransform> transform;
    for (DWORD i = 0; SUCCEEDED(readerEx->GetTransformForStream(kVideo, i, &category, &transform)); ++i) {
        ComPtr<IMFAttributes> attributes;
        if (category == MFT_CATEGORY_VIDEO_PROCESSOR && SUCCEEDED(transform->GetAttributes(&attributes))) {
            check(attributes->SetUINT32(MF_XVP_DISABLE_FRC, TRUE), "MF_XVP_DISABLE_FRC");
            found = true;
        }
        transform.Reset();
    }
    if (!found) {
        throw std::runtime_error("no video processor to keep every frame with");
    }
}

void ClipReader::readType(bool first) {
    ComPtr<IMFMediaType> current;
    check(reader_->GetCurrentMediaType(kVideo, &current), "GetCurrentMediaType");
    UINT32 frameWidth = 0;
    UINT32 frameHeight = 0;
    check(MFGetAttributeSize(current.Get(), MF_MT_FRAME_SIZE, &frameWidth, &frameHeight), "MF_MT_FRAME_SIZE");
    // The picture can be smaller than the frames: decoders work in blocks of 16 pixels, so 1080 rows come as 1088.
    unsigned width = frameWidth;
    unsigned height = frameHeight;
    MFVideoArea area{};
    if (SUCCEEDED(
            current->GetBlob(MF_MT_MINIMUM_DISPLAY_APERTURE, reinterpret_cast<UINT8*>(&area), sizeof(area), nullptr)) &&
        area.OffsetX.value == 0 && area.OffsetY.value == 0 && area.Area.cx > 0 && area.Area.cy > 0) {
        width = std::min(width, static_cast<unsigned>(area.Area.cx));
        height = std::min(height, static_cast<unsigned>(area.Area.cy));
    }
    if (first) {
        width_ = width;
        height_ = height;
    } else if (width != width_ || height != height_) {
        throw std::runtime_error("The video changes size");
    }
    if (deviceManager_) {
        keepEveryFrame(); // Whenever the frames change type too, in case the video processor is made anew.
    }
    UINT32 stride = 0;
    if (SUCCEEDED(current->GetUINT32(MF_MT_DEFAULT_STRIDE, &stride))) {
        cpuStride_ = static_cast<LONG>(stride);
    } else {
        LONG computed = 0;
        check(MFGetStrideForBitmapInfoHeader(MFVideoFormat_RGB32.Data1, frameWidth, &computed),
              "MFGetStrideForBitmapInfoHeader");
        cpuStride_ = computed;
    }
    cpuRows_ = frameHeight;
}

void ClipReader::close() {
    sample_.Reset();
    decoded_.Reset();
    reader_.Reset();
    deviceManager_.Reset();
}

void ClipReader::rewind() {
    seek(1);
}

std::uint64_t ClipReader::keyframeAtOrBefore(std::uint64_t number) const {
    return *(std::upper_bound(keyframes_.begin(), keyframes_.end(), number) - 1); // keyframes_ starts with 1.
}

std::uint64_t ClipReader::seek(std::uint64_t number) {
    sample_.Reset();
    decoded_.Reset();
    const std::uint64_t keyframe = keyframeAtOrBefore(number);
    PROPVARIANT start;
    PropVariantInit(&start);
    start.vt = VT_I8;
    start.hVal.QuadPart = sampleTimes_[static_cast<std::size_t>(keyframe - 1)];
    check(reader_->SetCurrentPosition(GUID_NULL, start), "Going to a frame (SetCurrentPosition)");
    nextNumber_ = keyframe;
    return keyframe;
}

bool ClipReader::next(Frame& frame) {
    sample_.Reset();
    decoded_.Reset();
    while (!sample_) {
        DWORD flags = 0;
        LONGLONG time = 0;
        check(reader_->ReadSample(kVideo, 0, nullptr, &flags, &time, &sample_), "Decoding the video (ReadSample)");
        if ((flags & MF_SOURCE_READERF_ERROR) != 0) {
            throw std::runtime_error("The video cannot be decoded further");
        }
        if ((flags & MF_SOURCE_READERF_ENDOFSTREAM) != 0) {
            return false;
        }
        if ((flags & MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED) != 0) {
            readType(false);
        }
    }

    ComPtr<IMFMediaBuffer> buffer;
    check(sample_->GetBufferByIndex(0, &buffer), "IMFSample::GetBufferByIndex");
    ComPtr<IMFDXGIBuffer> gpuBuffer;
    if (SUCCEEDED(buffer.As(&gpuBuffer))) {
        check(gpuBuffer->GetResource(IID_PPV_ARGS(&decoded_)), "IMFDXGIBuffer::GetResource");
        UINT subresource = 0;
        check(gpuBuffer->GetSubresourceIndex(&subresource), "IMFDXGIBuffer::GetSubresourceIndex");
        frame.texture = fit(decoded_.Get(), subresource);
        frame.subresource = subresource;
    } else {
        frame.texture = upload(buffer.Get());
        frame.subresource = 0;
    }
    // Counted rather than looked up by the decoded frame's time: decoders hand out every frame once, in order, but
    // their times are only as good as their settings (2026-11-11: with the video processor's frame rate conversion on,
    // or MF_LOW_LATENCY, they were not the file's; as set up here they are, within 0.2 ms).
    if (nextNumber_ > times_.size()) {
        throw std::runtime_error("The decoder gave more frames than the video holds");
    }
    frame.number = nextNumber_++;
    frame.seconds = times_[static_cast<std::size_t>(frame.number - 1)];
    return true;
}

ID3D11Texture2D* ClipReader::ownTexture() {
    if (!own_) {
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = width_;
        desc.Height = height_;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = ScreenCapture::kImageFormat;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT;
        check(device_->CreateTexture2D(&desc, nullptr, &own_), "CreateTexture2D (clip frame)");
    }
    return own_.Get();
}

ID3D11Texture2D* ClipReader::fit(ID3D11Texture2D* texture, UINT& subresource) {
    D3D11_TEXTURE2D_DESC desc{};
    texture->GetDesc(&desc);
    if (desc.Format != ScreenCapture::kImageFormat) {
        throw std::runtime_error("the GPU's frames are not BGRA");
    }
    if (desc.Width == width_ && desc.Height == height_) {
        return texture;
    }
    if (desc.Width < width_ || desc.Height < height_) {
        throw std::runtime_error("the GPU's frames are smaller than the video");
    }
    // Decoders work in blocks of 16 pixels (1080 rows decode as 1088): cut to the video's size.
    const D3D11_BOX box{0, 0, 0, width_, height_, 1};
    context_->CopySubresourceRegion(ownTexture(), 0, 0, 0, 0, texture, subresource, &box);
    subresource = 0;
    return own_.Get();
}

ID3D11Texture2D* ClipReader::upload(IMFMediaBuffer* buffer) {
    BYTE* data = nullptr;
    LONG stride = cpuStride_;
    ComPtr<IMF2DBuffer> rows;
    const bool twoD = SUCCEEDED(buffer->QueryInterface(IID_PPV_ARGS(&rows))) && SUCCEEDED(rows->Lock2D(&data, &stride));
    if (!twoD) {
        check(buffer->Lock(&data, nullptr, nullptr), "IMFMediaBuffer::Lock");
        if (stride < 0) {
            data += static_cast<std::ptrdiff_t>(-stride) * (cpuRows_ - 1); // Locked memory starts at the bottom row.
        }
    }
    // `data` is the top row now; rows follow `stride` bytes apart (negative: upwards in memory).
    const BYTE* top = data;
    UINT pitch = static_cast<UINT>(stride);
    if (stride < 0) {
        const std::size_t rowBytes = static_cast<std::size_t>(width_) * 4;
        rows_.resize(rowBytes * height_);
        for (unsigned y = 0; y < height_; ++y) {
            std::memcpy(rows_.data() + rowBytes * y, data + static_cast<std::ptrdiff_t>(stride) * y, rowBytes);
        }
        top = rows_.data();
        pitch = static_cast<UINT>(rowBytes);
    }
    context_->UpdateSubresource(ownTexture(), 0, nullptr, top, pitch, 0);
    if (twoD) {
        rows->Unlock2D();
    } else {
        buffer->Unlock();
    }
    return own_.Get();
}

// The UI's thread and the player's share only atomics: the UI turns Playing into Paused and Paused or Ended into
// Playing (and asks for a restart) and sets `wake`; the player's thread turns Playing into Ended, sets Failed and the
// position. Neither ever waits for the other.
struct ClipPlayer::Impl {
    GpuDevice gpu;
    std::unique_ptr<ImageChannel> channel;
    unsigned width = 0;
    unsigned height = 0;
    double seconds = 0.0;
    std::atomic<State> state{State::Playing};
    std::atomic<bool> restart{false}; // play() after Ended: start again from the first frame.
    std::atomic<double> position{0.0};
    std::string error;
    HANDLE wake = nullptr; // Set by play(), pause() and stopping, so the thread looks at the state again.
    std::jthread thread;   // Last, so it stops before the members it uses are destroyed.

    Impl() {
        wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (wake == nullptr) {
            check(HRESULT_FROM_WIN32(GetLastError()), "CreateEvent (clip player)");
        }
    }

    ~Impl() {
        if (thread.joinable()) {
            thread.request_stop();
            SetEvent(wake);
            thread.join();
        }
        CloseHandle(wake);
    }

    void run(std::stop_token stop, ComPtr<IDXGIAdapter1> adapter, std::filesystem::path video,
             std::promise<void> opened);
    void playFrames(std::stop_token stop, ClipReader& reader);
};

void ClipPlayer::Impl::run(std::stop_token stop, ComPtr<IDXGIAdapter1> adapter, std::filesystem::path video,
                           std::promise<void> opened) {
    SetThreadDescription(GetCurrentThread(), L"observex clip player");
    const bool com = SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED)); // Media Foundation needs it.
    std::unique_ptr<ClipReader> reader;
    try {
        gpu = createGpuDevice(adapter.Get(), true);
        reader = std::make_unique<ClipReader>(gpu.device.Get(), video);
        width = reader->width();
        height = reader->height();
        seconds = reader->times().back();
        channel = std::make_unique<ImageChannel>(gpu.device.Get(), width, height, ScreenCapture::kImageFormat);
    } catch (...) {
        reader.reset();
        opened.set_exception(std::current_exception());
        if (com) {
            CoUninitialize();
        }
        return;
    }
    opened.set_value();

    try {
        playFrames(stop, *reader);
    } catch (const std::exception& e) {
        error = e.what();
        state.store(State::Failed, std::memory_order_release);
    }
    reader.reset();
    if (com) {
        CoUninitialize();
    }
}

void ClipPlayer::Impl::playFrames(std::stop_token stop, ClipReader& reader) {
    // A high resolution timer wakes within about half a millisecond; the plain one within a scheduler tick or so.
    HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    if (timer == nullptr) {
        timer = CreateWaitableTimerW(nullptr, FALSE, nullptr);
    }
    if (timer == nullptr) {
        check(HRESULT_FROM_WIN32(GetLastError()), "CreateWaitableTimer (clip player)");
    }
    const HANDLE waits[2] = {wake, timer};
    const double ticksPerSecond = qpcPerSecond();
    const std::vector<double>& times = reader.times();

    // The video's clock: it read `clockSeconds` at `clockQpc` and runs with real time while playing.
    double clockSeconds = 0.0;
    std::int64_t clockQpc = 0;
    bool clockRunning = false;
    ClipReader::Frame frame;
    bool holding = false; // `frame` is decoded and waits for its time.
    bool atEnd = false;

    try {
        while (!stop.stop_requested()) {
            if (state.load(std::memory_order_acquire) != State::Playing) {
                if (clockRunning) { // Paused: the clock stops where it is.
                    clockSeconds += static_cast<double>(qpcNow() - clockQpc) / ticksPerSecond;
                    clockRunning = false;
                }
                WaitForSingleObject(wake, INFINITE);
                continue;
            }
            if (restart.exchange(false, std::memory_order_acquire)) {
                reader.rewind();
                holding = false;
                atEnd = false;
                clockSeconds = 0.0;
                clockRunning = false;
            }
            if (!holding && !atEnd) {
                if (clockRunning) {
                    // Decoding fell a keyframe behind: go on from the newest keyframe due, skipping the frames before
                    // it, rather than slow down. (Frames only a little late are handed over at once; the UI shows the
                    // newest.)
                    const double now = clockSeconds + static_cast<double>(qpcNow() - clockQpc) / ticksPerSecond;
                    const auto due =
                        static_cast<std::uint64_t>(std::upper_bound(times.begin(), times.end(), now) - times.begin());
                    if (due > 0 && reader.keyframeAtOrBefore(due) > reader.nextNumber()) {
                        reader.seek(due);
                    }
                }
                holding = reader.next(frame);
                atEnd = !holding;
            }
            if (atEnd) {
                State playing = State::Playing;
                state.compare_exchange_strong(playing, State::Ended, std::memory_order_acq_rel);
                continue;
            }
            if (!clockRunning) { // Starts with the first frame decoded, so decoding it never makes it late.
                clockQpc = qpcNow();
                clockRunning = true;
            }

            const double now = clockSeconds + static_cast<double>(qpcNow() - clockQpc) / ticksPerSecond;
            if (now < frame.seconds) { // Not yet: wait for its time (or play(), pause(), stopping).
                LARGE_INTEGER due;
                due.QuadPart = -std::max<LONGLONG>(1, static_cast<LONGLONG>((frame.seconds - now) * kTicksPerSecond));
                SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE);
                WaitForMultipleObjects(2, waits, FALSE, INFINITE);
                continue;
            }
            if (state.load(std::memory_order_acquire) != State::Playing) {
                continue; // Paused while it decoded: keep it for later.
            }
            channel->write(gpu.context.Get(), frame.texture, ImageStamp{frame.number, 0}, frame.subresource);
            gpu.context->Flush(); // Start the copy now; the UI's device waits for it through the keyed mutex.
            position.store(frame.seconds, std::memory_order_release);
            holding = false;
        }
    } catch (...) {
        CloseHandle(timer);
        throw;
    }
    CloseHandle(timer);
}

ClipPlayer::ClipPlayer(IDXGIAdapter1* gpu, const std::filesystem::path& video) : impl_(std::make_unique<Impl>()) {
    // The player's device gets an adapter object of its own, so it cannot hold up the UI's or the pipeline's.
    ComPtr<IDXGIAdapter1> adapter = findOwnAdapter(gpu);
    std::promise<void> opened;
    std::future<void> ready = opened.get_future();
    impl_->thread = std::jthread([impl = impl_.get(), adapter = std::move(adapter), video,
                                  opened = std::move(opened)](std::stop_token stop) mutable {
        impl->run(stop, std::move(adapter), std::move(video), std::move(opened));
    });
    ready.get(); // Throws what opening threw.
}

ClipPlayer::~ClipPlayer() = default;

unsigned ClipPlayer::width() const {
    return impl_->width;
}

unsigned ClipPlayer::height() const {
    return impl_->height;
}

double ClipPlayer::seconds() const {
    return impl_->seconds;
}

ImageChannel& ClipPlayer::frames() {
    return *impl_->channel;
}

void ClipPlayer::play() {
    State was = impl_->state.load(std::memory_order_acquire);
    if (was == State::Ended) {
        impl_->restart.store(true, std::memory_order_relaxed);
    }
    if ((was == State::Paused || was == State::Ended) &&
        impl_->state.compare_exchange_strong(was, State::Playing, std::memory_order_acq_rel)) {
        SetEvent(impl_->wake);
    }
}

void ClipPlayer::pause() {
    State was = State::Playing;
    if (impl_->state.compare_exchange_strong(was, State::Paused, std::memory_order_acq_rel)) {
        SetEvent(impl_->wake);
    }
}

ClipPlayer::State ClipPlayer::state() const {
    return impl_->state.load(std::memory_order_acquire);
}

const std::string& ClipPlayer::error() const {
    return impl_->error;
}

double ClipPlayer::position() const {
    return impl_->position.load(std::memory_order_acquire);
}

} // namespace observex
