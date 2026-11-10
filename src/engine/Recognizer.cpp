#include "engine/Recognizer.h"

#include "core/Mirror.h"
#include "core/StatWindow.h"
#include "engine/ClipRecorder.h"
#include "engine/ImageChannel.h"
#include "engine/LiveLog.h"

#include <exception>
#include <utility>

namespace observex {

namespace {

// Longest wait for a new image. Also how quickly the thread notices a stop request.
constexpr unsigned kWaitMs = 50;

std::int64_t qpcNow() {
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return now.QuadPart;
}

} // namespace

Recognizer::Recognizer(const Screen& screen, ImageChannel& channel, std::filesystem::path modelFolder,
                       Question* question)
    : modelFolder_(std::move(modelFolder)), question_(question), width_(channel.width()),
      height_(channel.height()) {
    const Screen own = findOnOwnAdapter(screen); // So this device cannot hold up the pipeline's.
    adapter_ = own.adapter;
    gpu_ = createGpuDevice(own.adapter.Get());
    reader_ = std::make_unique<ImageReader>(gpu_.device.Get(), channel);
    thread_ = std::jthread([this](std::stop_token stop) { run(stop); });
}

Recognizer::~Recognizer() {
    if (thread_.joinable()) {
        thread_.request_stop();
        thread_.join();
    }
}

const Found& Recognizer::found() {
    found_.fetch();
    return found_.readSlot();
}

const RecognizerStats& Recognizer::stats() {
    stats_.fetch();
    return stats_.readSlot();
}

void Recognizer::run(std::stop_token stop) {
    // Above normal: it mostly waits for the GPU, and what it finds is what observex reacts to.
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
    SetThreadDescription(GetCurrentThread(), L"observex recogniser");

    std::unique_ptr<Detector> detector;
    try {
        detector = std::make_unique<Detector>(gpu_.device.Get(), adapter_.Get(), modelFolder_, width_, height_, stop,
                                              progress_);
    } catch (const std::exception& e) {
        if (!stop.stop_requested()) {
            fail(e.what());
        }
        return;
    }
    kinds_ = detector->kinds(); // Before Running, which makes it readable.
    kindAnswers_.store(detector->kindUpdate().answers, std::memory_order_relaxed);
    state_.store(State::Running, std::memory_order_release);

    LARGE_INTEGER frequency;
    QueryPerformanceFrequency(&frequency);
    const double msPerTick = 1000.0 / static_cast<double>(frequency.QuadPart);
    StatWindow work;
    std::uint64_t images = 0;
    std::int64_t windowStart = qpcNow();
    Tracker tracker;
    std::array<Detection, Found::kMax> detections;
    std::array<Detection, Found::kMax> mirrored;
    const std::int64_t timeOrigin = windowStart;

    ImageStamp stamp;
    while (!stop.stop_requested()) {
        // A new kind update: read here, between looks.
        if (reloadKinds_.exchange(false, std::memory_order_acq_rel)) {
            const bool used = detector->setKindUpdate(loadKindUpdate(modelFolder_));
            kindAnswers_.store(used ? detector->kindUpdate().answers : 0, std::memory_order_relaxed);
        }
        if (reader_->waitForImage(kWaitMs) && reader_->copyNewImage(gpu_.context.Get(), detector->input(), &stamp)) {
            const std::int64_t start = qpcNow();
            const std::shared_ptr<LiveLog> log = log_.load(std::memory_order_acquire);
            const std::shared_ptr<ClipRecorder> clip = clip_.load(std::memory_order_acquire);
            const bool asked = question_ != nullptr && question_->asked();
            // While logging, keeping a clip or asked, also the things it is less sure of, for the log, the clip and the
            // mirror test only.
            const float minSure = log || clip || asked ? LiveLog::kMinSure : kMinSure;
            const int count = detector->detect(gpu_.context.Get(), minSure, detections.data(),
                                               static_cast<int>(Found::kMax));
            if (count < 0) {
                fail("The GPU stopped running the model (it may have been reset)");
                return;
            }
            if (log) {
                log->looked(stamp, detections.data(), count);
            }
            int sure = 0; // Surest first, so what is at least kMinSure sure comes first.
            while (sure < count && detections[static_cast<std::size_t>(sure)].sure >= kMinSure) {
                ++sure;
            }
            // Followed by when the screen showed the image, so the speeds are true on-screen speeds.
            const std::int64_t shownQpc = stamp.shownQpc != 0 ? stamp.shownQpc : start;
            const double timeMs = static_cast<double>(shownQpc - timeOrigin) * msPerTick;
            Found& found = found_.writeSlot();
            found.count =
                tracker.update(timeMs, detections.data(), sure, found.things.data(), static_cast<int>(Found::kMax));
            found.image = ++images;
            found_.publish();
            const std::int64_t done = qpcNow();
            work.add(static_cast<float>(static_cast<double>(done - start) * msPerTick));
            // The published slot is not written again until it comes back as a write slot, so it can be read here.
            if (clip) {
                clip->looked(stamp, done, {detections.data(), static_cast<std::size_t>(count)},
                             {found.things.data(), static_cast<std::size_t>(found.count)});
            }

            // The mirror test, after the UI has what is on the image, so it never holds that up.
            if (asked) {
                const int mirroredCount = detector->detect(gpu_.context.Get(), minSure, mirrored.data(),
                                                           static_cast<int>(Found::kMax), true);
                if (mirroredCount < 0) {
                    fail("The GPU stopped running the model (it may have been reset)");
                    return;
                }
                const std::span<const Detection> plain{detections.data(), static_cast<std::size_t>(count)};
                const std::span<const Detection> turned{mirrored.data(), static_cast<std::size_t>(mirroredCount)};
                const float disagreement = mirrorDisagreement(plain, turned, kMinSure);
                // With kinds, also yes when either look is unsure which kind something is.
                const float closest = closestKinds(plain, turned, kMinSure);
                question_->answer(disagreement >= kMirrorDisagrees || unsureOfKind(closest));
                if (log) {
                    log->mirrored(stamp, disagreement, closest);
                }
            }
        }

        // Once a second, summarise and hand the numbers to the UI (lock-free).
        const std::int64_t now = qpcNow();
        const std::int64_t elapsed = now - windowStart;
        if (elapsed >= frequency.QuadPart) {
            const StatWindow::Summary summary = work.take();
            RecognizerStats& stats = stats_.writeSlot();
            stats.imagesPerSecond =
                static_cast<float>(summary.count / (static_cast<double>(elapsed) * msPerTick / 1000.0));
            stats.workAvgMs = summary.average;
            stats.workMaxMs = summary.max;
            stats_.publish();
            windowStart = now;
        }
    }
}

void Recognizer::fail(std::string message) {
    error_ = std::move(message);
    state_.store(State::Failed, std::memory_order_release);
}

} // namespace observex
