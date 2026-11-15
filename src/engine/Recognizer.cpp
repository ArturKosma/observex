#include "engine/Recognizer.h"

#include "core/Heuristics.h"
#include "core/Mirror.h"
#include "core/StatWindow.h"
#include "engine/BoxSnapper.h"
#include "engine/ClipRecorder.h"
#include "engine/ImageChannel.h"
#include "engine/LiveLog.h"

#include <algorithm>
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
                       std::shared_ptr<const HeuristicsSource> heuristics, Question* question)
    : modelFolder_(std::move(modelFolder)), heuristics_(std::move(heuristics)), question_(question), width_(channel.width()),
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

void Recognizer::markDead(std::uint32_t id) {
    if (id == 0) {
        return;
    }
    for (const std::atomic<std::uint32_t>& dead : dead_) {
        if (dead.load(std::memory_order_relaxed) == id) {
            return; // Marked already.
        }
    }
    dead_[deadNext_.fetch_add(1, std::memory_order_relaxed) % kDeadKept].store(id, std::memory_order_relaxed);
}

void Recognizer::markDeadThings(std::span<Tracked> things, int deadKind) const {
    for (Tracked& thing : things) {
        bool dead = false;
        for (const std::atomic<std::uint32_t>& id : dead_) {
            dead = dead || id.load(std::memory_order_relaxed) == thing.id;
        }
        if (!dead || (static_cast<int>(thing.kind) == deadKind && kindSureOf(thing) >= kDeadSure)) {
            continue;
        }
        // Its kind before goes second, so a reader still sees what the model made of it.
        if (static_cast<int>(thing.kind) != deadKind) {
            thing.secondKind = thing.kind;
            thing.second = kindSureOf(thing);
        }
        thing.kind = static_cast<std::uint8_t>(deadKind);
        thing.kindSure = std::max(kindSureOf(thing), kDeadSure);
    }
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
    // The "dead" kind's place, for markDead (none: no dead marks).
    int deadKind = -1;
    for (std::size_t k = 0; k < kinds_.size(); ++k) {
        deadKind = kinds_[k] == "dead" ? static_cast<int>(k) : deadKind;
    }
    state_.store(State::Running, std::memory_order_release);

    LARGE_INTEGER frequency;
    QueryPerformanceFrequency(&frequency);
    const double msPerTick = 1000.0 / static_cast<double>(frequency.QuadPart);
    StatWindow work;
    // The Debug tab's steps, in the order they run on each image.
    enum Step { kCopy, kNetwork, kLog, kHeuristics, kSnapper, kClip, kMirror };
    CostMeter meter({"Copy in", "Network", "Live log", "Heuristics", "To snapper", "To clip buffer", "Mirror test"});
    std::uint64_t images = 0;
    std::int64_t windowStart = qpcNow();
    std::uint64_t heuristicsVersion = heuristics_->version();
    std::unique_ptr<Heuristics> heuristics;
    try {
        heuristics = heuristics_->make();
    } catch (const std::exception& e) {
        fail(e.what());
        return;
    }
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
        const bool waited = reader_->waitForImage(kWaitMs);
        if (waited) {
            meter.begin();
        }
        if (waited && reader_->copyNewImage(gpu_.context.Get(), detector->input(), &stamp)) {
            meter.lap(kCopy);
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
            meter.lap(kNetwork);
            if (log) {
                log->looked(stamp, detections.data(), count);
                meter.lap(kLog);
            }
            // Followed by when the screen showed the image, so the speeds are true on-screen speeds.
            const std::int64_t shownQpc = stamp.shownQpc != 0 ? stamp.shownQpc : start;
            const double timeMs = static_cast<double>(shownQpc - timeOrigin) * msPerTick;
            Found& found = found_.writeSlot();
            // What it found, through the heuristics (core/Heuristics: at least kMinSure sure, then the tracker): new
            // ones once the app loaded a rebuilt DLL (if they cannot be made, the ones it has go on).
            if (const std::uint64_t version = heuristics_->version(); version != heuristicsVersion) {
                heuristicsVersion = version;
                try {
                    heuristics = heuristics_->make();
                } catch (const std::exception&) {
                }
            }
            found.count = heuristics->update(timeMs, {detections.data(), static_cast<std::size_t>(count)},
                                             found.things.data(), static_cast<int>(Found::kMax));
            if (deadKind >= 0) {
                markDeadThings({found.things.data(), static_cast<std::size_t>(found.count)}, deadKind);
            }
            found.image = ++images;
            found_.publish();
            meter.lap(kHeuristics);
            // The published slot is not written again until it comes back as a write slot, so it can be read here.
            if (const std::shared_ptr<BoxSnapper> snapper = snapper_.load(std::memory_order_acquire)) {
                snapper->looked(stamp, {found.things.data(), static_cast<std::size_t>(found.count)});
                meter.lap(kSnapper);
            }
            const std::int64_t done = qpcNow();
            work.add(static_cast<float>(static_cast<double>(done - start) * msPerTick));
            if (clip) {
                clip->looked(stamp, done, {detections.data(), static_cast<std::size_t>(count)},
                             {found.things.data(), static_cast<std::size_t>(found.count)});
                meter.lap(kClip);
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
                meter.lap(kMirror);
            }
            meter.end();
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
            stats.costs = meter.take(static_cast<double>(elapsed) * msPerTick / 1000.0);
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
