#include "app/App.h"

#include "app/Snapshot.h"
#include "app/Widgets.h"

#include <imgui.h>
#include <imgui_impl_dx11.h>
#include <imgui_impl_win32.h>

#include <algorithm>
#include <cfloat>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <exception>

namespace observex {

namespace {

constexpr ImGuiWindowFlags kFixedWindow = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                                          ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus |
                                          ImGuiWindowFlags_NoScrollWithMouse;
constexpr char kPickerPopup[] = "screens";
constexpr char kSettingsPopup[] = "settings";
constexpr char kShowPopup[] = "show";
constexpr char kCategoryPopup[] = "What are we looking for?";
constexpr char kRecordingsPopup[] = "recordings";
constexpr char kChooseText[] = "Choose a screen to observe";
constexpr char kHotkeyText[] = "Ctrl+Alt+R";
constexpr int kRecordHotkey = 0; // MainWindow hotkey ids.
constexpr int kMarkHotkey = 1;
// What live recognition looks for: its model folder, live log, Show flag and box tags.
constexpr const Category& kLive = kCategories[0];
// Seconds between saved images while recording. Neighbouring images are near copies, so the user asked for one
// every 3 s (2026-11-01) rather than more to review. Fixed, no setting.
constexpr unsigned kRecordingInterval = 3;
constexpr int kSnapshotStepFrames = 36; // Snapshot frames are 16 ms apart, so each step gets about 0.6 s.

void centredText(const char* text, ImU32 color, float y) {
    const float width = ImGui::CalcTextSize(text).x;
    ImGui::SetCursorPos(ImVec2(std::round((ImGui::GetWindowWidth() - width) * 0.5f), std::round(y)));
    ImGui::PushStyleColor(ImGuiCol_Text, color);
    ImGui::TextUnformatted(text);
    ImGui::PopStyleColor();
}

// "12:34" or "1:02:34".
void formatDuration(char* text, std::size_t size, std::chrono::steady_clock::duration duration) {
    const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(duration).count();
    if (seconds >= 3600) {
        std::snprintf(text, size, "%lld:%02lld:%02lld", seconds / 3600, seconds / 60 % 60, seconds % 60);
    } else {
        std::snprintf(text, size, "%02lld:%02lld", seconds / 60, seconds % 60);
    }
}

// "840 KB", "7.3 MB", "312 MB" or "2.41 GB", counted in 1024s like File Explorer, so the numbers match it.
void formatBytes(char* text, std::size_t size, std::uint64_t bytes) {
    const double kb = static_cast<double>(bytes) / 1024.0;
    const double mb = kb / 1024.0;
    if (mb < 1.0) {
        std::snprintf(text, size, "%.0f KB", kb);
    } else if (mb < 10.0) {
        std::snprintf(text, size, "%.1f MB", mb);
    } else if (mb < 1024.0) {
        std::snprintf(text, size, "%.0f MB", mb);
    } else {
        std::snprintf(text, size, "%.2f GB", mb / 1024.0);
    }
}

// When `file` was last written, in local time: "2026-11-01 09:44". "missing" if there is no such file.
std::string fileTime(const std::filesystem::path& file) {
    WIN32_FILE_ATTRIBUTE_DATA data{};
    FILETIME local{};
    SYSTEMTIME time{};
    if (!GetFileAttributesExW(file.c_str(), GetFileExInfoStandard, &data) ||
        !FileTimeToLocalFileTime(&data.ftLastWriteTime, &local) || !FileTimeToSystemTime(&local, &time)) {
        return "missing";
    }
    char text[24];
    std::snprintf(text, sizeof(text), "%04u-%02u-%02u %02u:%02u", time.wYear, time.wMonth, time.wDay, time.wHour,
                  time.wMinute);
    return text;
}

// When a recording started, for the review's list: "Today, 12:56", "31 Oct, 23:59", or "31 Oct 2025, 23:59" in
// another year. `started` is RecordingInfo::started ("2026-10-31 23:59:49"); shown as it is if it is not that.
std::string describeWhen(const std::string& started) {
    static constexpr const char* kMonths[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                              "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    const auto number = [&started](std::size_t at, std::size_t length) {
        unsigned value = 0;
        const char* begin = started.data() + at;
        const auto [end, error] = std::from_chars(begin, begin + length, value);
        return error == std::errc() && end == begin + length ? value : 0u;
    };
    if (started.size() < 16) {
        return started;
    }
    const unsigned year = number(0, 4), month = number(5, 2), day = number(8, 2);
    const unsigned hour = number(11, 2), minute = number(14, 2);
    if (month < 1 || month > 12 || day == 0) {
        return started;
    }
    SYSTEMTIME today;
    GetLocalTime(&today);
    char text[40];
    if (year == today.wYear && month == today.wMonth && day == today.wDay) {
        std::snprintf(text, sizeof(text), "Today, %02u:%02u", hour, minute);
    } else if (year == today.wYear) {
        std::snprintf(text, sizeof(text), "%u %s, %02u:%02u", day, kMonths[month - 1], hour, minute);
    } else {
        std::snprintf(text, sizeof(text), "%u %s %u, %02u:%02u", day, kMonths[month - 1], year, hour, minute);
    }
    return text;
}

} // namespace

App::~App() {
    shutdown();
}

int App::run() {
    try {
        init();
    } catch (const std::exception& e) {
        if (snapshot_) {
            std::fprintf(stderr, "observex could not start: %s\n", e.what());
        } else {
            MessageBoxA(nullptr, e.what(), "observex could not start", MB_ICONERROR | MB_OK);
        }
        shutdown();
        return 1;
    }

    while (window_->pumpMessages()) {
        // Recording works while the window is minimized or behind a game, so this comes before drawing.
        if (window_->takeHotkeyPress(kRecordHotkey)) {
            toggleRecording();
        }
        if (window_->takeHotkeyPress(kMarkHotkey)) {
            markMoment();
        }
        if (recorder_ && recorder_->failed()) {
            recordingError_ = "Recording stopped. " + recorder_->error();
            stopRecording();
        }

        // Starting and stopping happen here, outside an ImGui frame, so no draw data points at freed images.
        if (closeReview_) {
            review_.reset();
            closeReview_ = false;
        }
        if (review_) {
            review_->update(); // The finder's progress, also while minimized.
        }
        if (chosen_) {
            review_.reset();  // Picking a screen is asking to see it.
            stopRecording();  // A recording is of one screen.
            picker_->close(); // Its capture of the chosen screen goes first; the pipeline takes over.
            observe(*chosen_);
            chosen_.reset();
        } else if (pipeline_.state() == Pipeline::State::NeedsRestart) {
            observeAgain();
        }
        // Recognition goes on whatever is shown, minimized too, except while reviewing (the user asked, 2026-11-01):
        // then the review's finder gets the GPU. With no images the recogniser waits, model loaded, and picks up again
        // the moment the review closes.
        pipeline_.setRecognitionEnabled(recognizer_ && recognizer_->state() != Recognizer::State::Failed && !review_);
        if (!window_->prepareFrame()) {
            pipeline_.setPreviewEnabled(false); // Minimized: nobody sees it.
            picker_->close();
            Sleep(10);
            continue;
        }
        pipeline_.setPreviewEnabled(observedImage_ != nullptr && !review_); // The review covers it.
        if (preview_ && preview_->copyNewImage(gpu_.context.Get(), observedImage_->texture())) {
            observedImage_->imageCopied();
        }

        // While reviewing, the arrow keys, Tab and Space belong to the review, not to moving between buttons.
        ImGuiIO& io = ImGui::GetIO();
        if (review_) {
            io.ConfigFlags &= ~ImGuiConfigFlags_NavEnableKeyboard;
        } else {
            io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
        }
        const bool saveSnapshot = snapshot_ && playSnapshot();
        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();
        drawFrame();
        ImGui::Render();

        window_->bindBackBuffer();
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        if (saveSnapshot) {
            try {
                savePng(gpu_.device.Get(), gpu_.context.Get(), window_->backBuffer().Get(), snapshot_->path.c_str());
            } catch (const std::exception& e) {
                std::fprintf(stderr, "Could not save the snapshot: %s\n", e.what());
                shutdown();
                return 1;
            }
            break;
        }
        if (snapshot_) {
            Sleep(16); // Hidden: nothing to present.
        } else {
            window_->present();
        }
        picker_->endFrame(); // Only after presenting: the frame drew its miniatures.
    }

    shutdown();
    return 0;
}

void App::init() {
    // Per-monitor DPI awareness: sharp UI, and required by the capture API (DuplicateOutput1).
    ImGui_ImplWin32_EnableDpiAwareness();
    dpiScale_ = ImGui_ImplWin32_GetDpiScaleForMonitor(MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY));

    if (snapshot_) {
        settings_.recordingsFolder = !snapshot_->recordingsFolder.empty()
                                         ? snapshot_->recordingsFolder
                                         : std::filesystem::path(snapshot_->path).parent_path() / L"recordings";
    } else {
        settingsFile_ = appFolder() / L"observex.cfg";
        settings_ = Settings::load(settingsFile_);
    }

    gpuAdapter_ = findPrimaryGpu();
    gpu_ = createGpuDevice(gpuAdapter_.Get());
    window_ = std::make_unique<MainWindow>(L"observex", gpu_.device.Get(), dpiScale_, !snapshot_);
    if (!snapshot_) {
        hotkeyRegistered_ = window_->registerHotkey(kRecordHotkey, MOD_CONTROL | MOD_ALT, 'R');
        markHotkeyRegistered_ = window_->registerHotkey(kMarkHotkey, MOD_CONTROL | MOD_ALT, 'M');
    }
    picker_ = std::make_unique<ScreenPicker>(gpu_.device.Get(), gpuAdapter_.Get());

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr; // The layout is fixed; nothing to remember between runs.
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    if (snapshot_) {
        io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange; // Never touch the real cursor.
    }
    fonts_ = theme::apply(dpiScale_);

    ImGui_ImplWin32_Init(window_->handle());
    ImGui_ImplDX11_Init(gpu_.device.Get(), gpu_.context.Get());
    imguiReady_ = true;
}

void App::shutdown() {
    review_.reset();
    stopRecording();
    stopObserving();
    picker_.reset();
    if (imguiReady_) {
        ImGui_ImplDX11_Shutdown();
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext();
        imguiReady_ = false;
    }
    window_.reset();
    gpu_ = {};
}

void App::observe(const Screen& screen) {
    stopObserving();
    observed_ = screen;
    error_.clear();
    // Before the loop starts, so its very first image reaches the UI and the recogniser.
    pipeline_.setPreviewEnabled(true);
    pipeline_.setRecognitionEnabled(true);
    if (!pipeline_.start(screen)) {
        return; // The view shows pipeline_.error().
    }
    try {
        preview_ = std::make_unique<ImageReader>(gpu_.device.Get(), *pipeline_.preview());
        observedImage_ = std::make_unique<ScreenImage>(gpu_.device.Get(), pipeline_.width(), pipeline_.height());
    } catch (const std::exception& e) {
        stopObserving();
        error_ = std::string("Could not show the screen: ") + e.what();
        return;
    }

    // The model tools/train made.
    recognizerError_.clear();
    try {
        recognizer_ = std::make_unique<Recognizer>(screen, *pipeline_.recognition(), modelFolder(kLive.id));
    } catch (const std::exception& e) {
        recognizerError_ = e.what();
    }
}

void App::stopObserving() {
    pauseRecording();     // The recorder reads from the pipeline's channel; it goes first.
    recognizer_.reset();  // So does the recogniser.
    preview_.reset();     // Before the pipeline frees the channel it reads from.
    observedImage_.reset();
    pipeline_.setRecognitionEnabled(false);
    pipeline_.stop();
}

void App::observeAgain() {
    for (const Screen& screen : listScreens(gpuAdapter_.Get())) {
        if (screen.name == observed_->name) {
            observe(screen);
            if (recording_) {
                if (canRecord()) {
                    if (recording_->log) {
                        recording_->log->resumed(); // The new pipeline numbers its images from 1 again.
                    }
                    resumeRecording(); // Same folder, numbering continues.
                } else {
                    stopRecording();
                }
            }
            return;
        }
    }
    stopRecording(); // The screen was disconnected.
    stopObserving();
    observed_.reset();
}

bool App::canRecord() const {
    return observed_ && pipeline_.recording() != nullptr && pipeline_.state() != Pipeline::State::Failed;
}

void App::toggleRecording() {
    if (recording_) {
        stopRecording();
    } else {
        startRecording();
    }
}

void App::startRecording() {
    if (recording_ || !canRecord()) {
        return;
    }
    // Each recording gets its own folder, named by the date and time, e.g. 2026-10-31_23-05-12.
    SYSTEMTIME now;
    GetLocalTime(&now);
    char name[40];
    std::snprintf(name, sizeof(name), "%04u-%02u-%02u_%02u-%02u-%02u", now.wYear, now.wMonth, now.wDay, now.wHour,
                  now.wMinute, now.wSecond);
    std::filesystem::path folder = settings_.recordingsFolder / name;
    std::error_code error;
    for (int i = 2; std::filesystem::exists(folder, error); ++i) {
        folder = settings_.recordingsFolder / (std::string(name) + "_" + std::to_string(i));
    }
    recordingError_.clear();
    // What live recognition sees goes into the recording too (engine/LiveLog), to look at afterwards. A setting, on
    // by default.
    std::shared_ptr<LiveLog> log;
    if (settings_.liveLog) {
        try {
            const std::filesystem::path model = modelFolder(kLive.id) / L"model.onnx";
            char about[600];
            std::snprintf(about, sizeof(about), "Model %s (written %s), screen %s (%s), %u x %u.",
                          toUtf8(model.c_str()).c_str(), fileTime(model).c_str(), observed_->monitor.c_str(),
                          observed_->name.c_str(), pipeline_.width(), pipeline_.height());
            log = std::make_shared<LiveLog>(folder / (std::string(kLive.id) + "-live.txt"), about);
        } catch (const std::exception& e) {
            recordingError_ = std::string("Could not record: ") + e.what();
            return;
        }
    }
    recording_ = Recording{folder, std::chrono::steady_clock::now()};
    recording_->log = std::move(log);
    char started[24];
    std::snprintf(started, sizeof(started), "%04u-%02u-%02u %02u:%02u:%02u", now.wYear, now.wMonth, now.wDay,
                  now.wHour, now.wMinute, now.wSecond);
    RecordingInfo& info = recording_->info;
    info.started = started;
    info.screen = observed_->monitor;
    info.width = pipeline_.width();
    info.height = pipeline_.height();
    info.every = kRecordingInterval;
    resumeRecording();
    // The folder exists once the recorder runs. Without the file the review would not list this recording.
    if (recording_ && !writeRecordingInfo(recording_->folder, recording_->info)) {
        recordingError_ = "Could not write " + toUtf8((recording_->folder / kRecordingFile).c_str());
    }
}

void App::resumeRecording() {
    try {
        recorder_ = std::make_unique<Recorder>(*observed_, *pipeline_.recording(), recording_->folder,
                                               recording_->saved + 1, recording_->log);
        pipeline_.setRecordingInterval(kRecordingInterval);
        if (recognizer_) {
            recognizer_->setLog(recording_->log);
        }
    } catch (const std::exception& e) {
        recordingError_ = std::string("Could not record: ") + e.what();
        recorder_.reset();
        recording_.reset();
    }
}

void App::pauseRecording() {
    pipeline_.setRecordingInterval(0);
    if (recognizer_) {
        recognizer_->setLog(nullptr);
    }
    if (recorder_) {
        if (recording_) {
            recording_->saved += recorder_->saved();
            recording_->savedBytes += recorder_->savedBytes();
        }
        recorder_.reset();
    }
}

void App::stopRecording() {
    pauseRecording();
    if (recording_) {
        RecordingInfo& info = recording_->info;
        info.seconds = static_cast<unsigned>(
            std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - recording_->started)
                .count());
        info.images = recording_->saved;
        if (!writeRecordingInfo(recording_->folder, info)) {
            recordingError_ = "Could not write " + toUtf8((recording_->folder / kRecordingFile).c_str());
        }
    }
    recording_.reset();
}

void App::markMoment() {
    if (!recording_ || !recorder_ || !recording_->log) {
        return; // Marks go into a recording's live log.
    }
    recording_->log->mark(++recording_->marks);
    pipeline_.recordNow(); // The image on screen now, to see what the mark is about.
}

void App::saveSettings() {
    if (!settingsFile_.empty()) {
        settings_.save(settingsFile_);
    }
}

void App::openRecordings() {
    // Read once per opening: a few small files per recording.
    recordings_.clear();
    for (RecordingEntry& entry : listRecordings(settings_.recordingsFolder)) {
        RecordingRow row;
        row.when = describeWhen(entry.info.started);
        row.images = Review::listImages(entry.folder).size();
        for (const Category& category : kCategories) {
            row.reviewed.push_back(Review::countReviewed(entry.folder, category));
        }
        row.entry = std::move(entry);
        recordings_.push_back(std::move(row));
    }
    reviewError_.clear();
    ImGui::OpenPopup(kRecordingsPopup);
}

void App::startReview(const Category& category) {
    try {
        review_ = std::make_unique<Review>(gpu_.device.Get(), reviewFolder_, category);
    } catch (const std::exception& e) {
        reviewError_ = std::string("Could not open the recording: ") + e.what();
    }
}

bool App::playSnapshot() {
    // Step 0 lets the UI settle, then one input step each, then the mouse rests and the last frame is saved.
    ImGuiIO& io = ImGui::GetIO();
    const int step = snapshotFrame_ / kSnapshotStepFrames - 1;
    const int frame = snapshotFrame_ % kSnapshotStepFrames;
    ++snapshotFrame_;
    const int steps = static_cast<int>(snapshot_->steps.size());
    if (step < 0) {
        return false;
    }
    if (step < steps) {
        const Snapshot::Step& input = snapshot_->steps[static_cast<std::size_t>(step)];
        if (input.key != ImGuiKey_None) {
            if (frame == 1 || frame == 2) {
                io.AddKeyEvent(input.key, frame == 1);
            }
            return false;
        }
        // Press at `from`, move to `to` over a few frames (none for a click), let go.
        const bool drag = input.from.x != input.to.x || input.from.y != input.to.y;
        const int moveFrames = drag ? 10 : 0;
        if (frame == 0) {
            io.AddMousePosEvent(input.from.x, input.from.y);
        } else if (frame == 1) {
            io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
        } else if (frame <= 1 + moveFrames) {
            const float t = static_cast<float>(frame - 1) / static_cast<float>(moveFrames);
            io.AddMousePosEvent(input.from.x + (input.to.x - input.from.x) * t,
                                input.from.y + (input.to.y - input.from.y) * t);
        } else if (frame == 2 + moveFrames) {
            io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
        }
        return false;
    }
    // After the steps: the mouse rests, and the frame is saved after the usual step time plus `wait`.
    const int rest = snapshotFrame_ - 1 - (steps + 1) * kSnapshotStepFrames;
    if (rest == 0) {
        const ImVec2 mouse = snapshot_->mouse.value_or(ImVec2(-FLT_MAX, -FLT_MAX));
        io.AddMousePosEvent(mouse.x, mouse.y);
    }
    return rest == kSnapshotStepFrames - 1 + static_cast<int>(snapshot_->wait * 1000.0f / 16.0f);
}

void App::drawFrame() {
    if (ImGui::IsKeyPressed(ImGuiKey_F3, false)) {
        showStats_ = !showStats_;
    }
    picker_->setObserved(observed_ ? observed_->name : std::string(), observedImage_.get());

    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const float barHeight = std::round(52.0f * dpiScale_);
    drawTopBar(barHeight);

    ImGui::SetNextWindowPos(ImVec2(viewport->Pos.x, viewport->Pos.y + barHeight));
    ImGui::SetNextWindowSize(ImVec2(viewport->Size.x, viewport->Size.y - barHeight));
    ImGui::Begin("##view", nullptr, kFixedWindow);
    if (review_) {
        review_->draw(fonts_);
    } else if (observed_) {
        drawObservedScreen();
    } else {
        drawChooseScreen();
    }
    // Errors stack at the top right.
    const float inset = std::round(12.0f * dpiScale_);
    ImVec2 corner(viewport->Pos.x + viewport->Size.x - inset, viewport->Pos.y + barHeight + inset);
    for (const std::string* error : {&recordingError_, &reviewError_}) {
        if (!error->empty()) {
            theme::drawLabel(ImGui::GetWindowDrawList(), corner, ImVec2(1.0f, 0.0f), error->c_str());
            corner.y += ImGui::GetTextLineHeight() + std::round(16.0f * dpiScale_);
        }
    }
    ImGui::End();
}

void App::drawTopBar(float height) {
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->Pos);
    ImGui::SetNextWindowSize(ImVec2(viewport->Size.x, height));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::Begin("##bar", nullptr, kFixedWindow);
    ImGui::PopStyleVar();

    const ImVec2 pos = ImGui::GetWindowPos();
    ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(pos.x, pos.y + height - 1.0f),
                                              ImVec2(pos.x + viewport->Size.x, pos.y + height), theme::kBorder);

    const float margin = std::round(12.0f * dpiScale_);
    const float buttonY = pos.y + std::round((height - BarButton::kHeight * dpiScale_) * 0.5f);
    const float popupGap = std::round(6.0f * dpiScale_);

    // Right: Settings, and Record to its left. Drawn first so their popups and tooltips sit on top.
    BarButton settingsButton;
    settingsButton.icon = Icon::Gear;
    settingsButton.open = ImGui::IsPopupOpen(kSettingsPopup);
    float x = pos.x + viewport->Size.x - margin - settingsButton.width(fonts_);
    ImGui::SetCursorScreenPos(ImVec2(x, buttonY));
    if (settingsButton.draw("##settings", fonts_)) {
        ImGui::OpenPopup(kSettingsPopup);
    }
    if (ImGui::IsItemHovered() && !settingsButton.open) {
        ImGui::SetTooltip("Settings");
    }
    const ImVec2 settingsCorner(ImGui::GetItemRectMax().x, ImGui::GetItemRectMax().y + popupGap);

    BarButton recordButton;
    char detail[96];
    if (recording_) {
        const unsigned saved = recording_->saved + (recorder_ ? recorder_->saved() : 0);
        const std::uint64_t savedBytes = recording_->savedBytes + (recorder_ ? recorder_->savedBytes() : 0);
        char duration[16];
        formatDuration(duration, sizeof(duration), std::chrono::steady_clock::now() - recording_->started);
        char bytes[16];
        formatBytes(bytes, sizeof(bytes), savedBytes);
        const int length = std::snprintf(detail, sizeof(detail), "%s  \xC2\xB7  %u %s  \xC2\xB7  %s", duration, saved,
                                         saved == 1 ? "image" : "images", bytes);
        if (recording_->marks > 0 && length > 0) {
            std::snprintf(detail + length, sizeof(detail) - static_cast<std::size_t>(length), "  \xC2\xB7  %u %s",
                          recording_->marks, recording_->marks == 1 ? "mark" : "marks");
        }
        recordButton.icon = Icon::Stop;
        recordButton.text = "Stop";
        recordButton.detail = detail;
        recordButton.open = true;
        recordButton.highlight = theme::kRecord;
    } else {
        recordButton.icon = Icon::Record;
        recordButton.text = "Record";
        recordButton.detail = hotkeyRegistered_ || snapshot_ ? kHotkeyText : nullptr;
        recordButton.enabled = canRecord();
    }
    x -= std::round(8.0f * dpiScale_) + recordButton.width(fonts_);
    ImGui::SetCursorScreenPos(ImVec2(x, buttonY));
    if (recordButton.draw("##record", fonts_)) {
        toggleRecording();
    }
    if (ImGui::IsItemHovered() && !recordButton.enabled) {
        ImGui::SetTooltip("Choose a screen to record it");
    } else if (ImGui::IsItemHovered() && recording_ && recording_->log) {
        ImGui::SetTooltip("Saving an image every %u s, and what the model sees on every image it looks at.\n"
                          "Ctrl+Alt+M marks a moment to talk about later (it saves the image shown then too).",
                          kRecordingInterval);
    } else if (ImGui::IsItemHovered() && recording_) {
        ImGui::SetTooltip("Saving an image every %u s.", kRecordingInterval);
    }

    // Review, left of Record: opens a recording to answer what observex found in it, or closes the review.
    BarButton reviewButton;
    if (review_) {
        reviewButton.icon = Icon::Close;
        reviewButton.text = "Close review";
    } else {
        reviewButton.icon = Icon::Picture;
        reviewButton.text = "Review";
        reviewButton.open = ImGui::IsPopupOpen(kRecordingsPopup) || ImGui::IsPopupOpen(kCategoryPopup);
    }
    x -= std::round(8.0f * dpiScale_) + reviewButton.width(fonts_);
    ImGui::SetCursorScreenPos(ImVec2(x, buttonY));
    if (reviewButton.draw("##review", fonts_)) {
        if (review_) {
            closeReview_ = true;
        } else {
            openRecordings();
        }
    }
    if (ImGui::IsItemHovered() && !review_ && !reviewButton.open) {
        ImGui::SetTooltip("Open a recording and answer what observex found in it");
    }
    ImGui::SetNextWindowPos(ImVec2(ImGui::GetItemRectMax().x, ImGui::GetItemRectMax().y + popupGap), ImGuiCond_Always,
                            ImVec2(1.0f, 0.0f));
    drawRecordings();
    if (openCategory_) { // Here, not inside the list's popup, so the chooser's name means the same popup.
        openCategory_ = false;
        ImGui::OpenPopup(kCategoryPopup);
    }
    drawCategoryChooser();
    if (review_) {
        drawReviewSteps(height);
    }

    ImGui::SetNextWindowPos(settingsCorner, ImGuiCond_Always, ImVec2(1.0f, 0.0f));
    if (ImGui::BeginPopup(kSettingsPopup)) {
        drawSettings();
        ImGui::EndPopup();
    }

    // Left: the screen button names the observed screen and opens the miniatures to pick another.
    BarButton screenButton;
    screenButton.icon = Icon::Monitor;
    screenButton.arrow = true;
    screenButton.open = ImGui::IsPopupOpen(kPickerPopup);
    if (observed_) {
        screenButton.caption = "Observing";
        screenButton.text = observed_->monitor.c_str();
    } else {
        screenButton.text = "Choose a screen";
    }
    ImGui::SetCursorScreenPos(ImVec2(pos.x + margin, buttonY));
    if (screenButton.draw("##screen", fonts_)) {
        ImGui::OpenPopup(kPickerPopup);
    }
    const ImVec2 screenMin = ImGui::GetItemRectMin();
    const ImVec2 screenMax = ImGui::GetItemRectMax();

    // Right of it, while the screen shows: Show, the flags for what is drawn over it (like view flags in Unreal).
    if (observed_ && !review_) {
        BarButton showButton;
        showButton.icon = Icon::Eye;
        showButton.text = "Show";
        showButton.arrow = true;
        showButton.open = ImGui::IsPopupOpen(kShowPopup);
        ImGui::SetCursorScreenPos(ImVec2(screenMax.x + std::round(8.0f * dpiScale_), buttonY));
        if (showButton.draw("##show", fonts_)) {
            ImGui::OpenPopup(kShowPopup);
        }
        ImGui::SetNextWindowPos(ImVec2(ImGui::GetItemRectMin().x, ImGui::GetItemRectMax().y + popupGap));
        if (ImGui::BeginPopup(kShowPopup)) {
            drawShowFlags();
            ImGui::EndPopup();
        }
    }

    ImGui::SetNextWindowPos(ImVec2(screenMin.x, screenMax.y + popupGap));
    if (ImGui::BeginPopup(kPickerPopup)) {
        ImGui::TextDisabled(kChooseText);
        ImGui::Dummy(ImVec2(0.0f, 2.0f * dpiScale_));
        const ImVec2 maxSize(std::min(960.0f * dpiScale_, viewport->Size.x - 80.0f * dpiScale_),
                             std::min(380.0f * dpiScale_, viewport->Size.y * 0.6f));
        const ImVec2 size = picker_->fit(maxSize);
        if (const Screen* chosen = picker_->draw(ImVec2(size.x, maxSize.y), theme::kSurface)) {
            chosen_ = *chosen;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
    ImGui::End();
}

void App::drawSettings() {
    const float width = std::round(460.0f * dpiScale_);
    const float sectionGap = std::round(10.0f * dpiScale_);

    ImGui::PushFont(fonts_.medium, 0.0f);
    ImGui::TextUnformatted("Settings");
    ImGui::PopFont();
    ImGui::Dummy(ImVec2(0.0f, sectionGap * 0.5f));

    // Where recordings are saved. Typing a path is not needed: the folder picker sets it.
    ImGui::TextDisabled("Recordings folder");
    std::string folder = toUtf8(settings_.recordingsFolder.c_str());
    ImGui::SetNextItemWidth(width);
    ImGui::InputText("##folder", folder.data(), folder.size() + 1, ImGuiInputTextFlags_ReadOnly);
    if (ImGui::Button("Change\xE2\x80\xA6")) {
        if (const std::optional<std::filesystem::path> picked =
                pickFolder(window_->handle(), settings_.recordingsFolder, L"Choose where recordings are saved")) {
            settings_.recordingsFolder = *picked;
            saveSettings();
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Open")) {
        showInExplorer(settings_.recordingsFolder);
    }
    if (settings_.recordingsFolder != defaultRecordingsFolder()) {
        ImGui::SameLine();
        if (ImGui::Button("Use default")) {
            settings_.recordingsFolder = defaultRecordingsFolder();
            saveSettings();
        }
    }
    if (recording_) {
        ImGui::TextDisabled("A new folder applies from the next recording.");
    }
    ImGui::Dummy(ImVec2(0.0f, sectionGap));

    // The live log (engine/LiveLog): what the model sees, kept with each recording to look at afterwards.
    if (ImGui::Checkbox("Log what the model sees while recording", &settings_.liveLog)) {
        saveSettings();
    }
    ImGui::TextDisabled("Every box and how sure it is, on every image it looks at (about 85 MB an hour).");
    if (recording_) {
        ImGui::TextDisabled("Applies from the next recording.");
    }
    ImGui::Dummy(ImVec2(0.0f, sectionGap));

    ImGui::TextDisabled(hotkeyRegistered_ || snapshot_
                            ? "Ctrl+Alt+R starts and stops recording, even while a game is in front."
                            : "Ctrl+Alt+R is taken by another app, so only the Record button works.");
    ImGui::TextDisabled(markHotkeyRegistered_ || snapshot_
                            ? "Ctrl+Alt+M marks a moment in the log while recording, to talk about later."
                            : "Ctrl+Alt+M is taken by another app, so moments cannot be marked.");
}

void App::drawShowFlags() {
    // Each flag only says what is drawn: hiding characters does not stop looking for them.
    ImGui::TextDisabled("Draw over the screen");
    ImGui::Dummy(ImVec2(0.0f, 2.0f * dpiScale_));
    if (ImGui::Checkbox(kLive.name, &settings_.showCharacters)) {
        saveSettings();
    }

    // Under it, how looking for them goes.
    char text[256];
    if (!recognizer_) {
        std::snprintf(text, sizeof(text), "%s",
                      recognizerError_.empty() ? "Not looking: the screen is not captured."
                                               : recognizerError_.c_str());
    } else if (recognizer_->state() == Recognizer::State::Starting) {
        const float preparing = recognizer_->preparing();
        if (preparing > 0.0f) {
            std::snprintf(text, sizeof(text), "Preparing the model for this GPU: %.0f%% (once per new model)",
                          static_cast<double>(preparing * 100.0f));
        } else {
            std::snprintf(text, sizeof(text), "Loading the model\xE2\x80\xA6");
        }
    } else if (recognizer_->state() == Recognizer::State::Failed) {
        std::snprintf(text, sizeof(text), "%s", recognizer_->error().c_str());
    } else {
        const RecognizerStats& stats = recognizer_->stats();
        const int found = recognizer_->found().count;
        std::snprintf(text, sizeof(text), "%d found  \xC2\xB7  %.0f images a second  \xC2\xB7  %.1f ms each", found,
                      static_cast<double>(stats.imagesPerSecond), static_cast<double>(stats.workAvgMs));
    }
    const float indent = ImGui::GetFrameHeight() + ImGui::GetStyle().ItemInnerSpacing.x; // Under the flag.
    ImGui::Indent(indent);
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + std::round(340.0f * dpiScale_));
    ImGui::TextDisabled("%s", text);
    ImGui::PopTextWrapPos();
    ImGui::Unindent(indent);
}

void App::drawReviewSteps(float height) {
    // In the middle of the bar: back, where we are, forward.
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const float gap = std::round(12.0f * dpiScale_);
    BarButton back;
    back.icon = Icon::Back;
    back.enabled = review_->canGoBack();
    BarButton forward;
    forward.icon = Icon::Forward;
    forward.enabled = review_->canGoOn();

    char position[64];
    std::snprintf(position, sizeof(position), "Image %zu of %zu", review_->index() + 1, review_->imageCount());
    char reviewed[64];
    std::snprintf(reviewed, sizeof(reviewed), "%zu reviewed", review_->reviewedCount());
    const float fontSize = ImGui::GetFontSize();
    const float positionWidth = fonts_.medium->CalcTextSizeA(fontSize, FLT_MAX, 0.0f, position).x;
    const float reviewedWidth = ImGui::CalcTextSize(reviewed).x;
    const float textGap = std::round(8.0f * dpiScale_);
    const float total =
        back.width(fonts_) + gap + positionWidth + textGap + reviewedWidth + gap + forward.width(fonts_);

    const ImVec2 pos = ImGui::GetWindowPos();
    const float buttonY = pos.y + std::round((height - BarButton::kHeight * dpiScale_) * 0.5f);
    const float textY = pos.y + std::round((height - fontSize) * 0.5f);
    float x = std::round(pos.x + (viewport->Size.x - total) * 0.5f);
    ImGui::SetCursorScreenPos(ImVec2(x, buttonY));
    if (back.draw("##back", fonts_)) {
        review_->previous();
    }
    if (ImGui::IsItemHovered() && back.enabled) {
        ImGui::SetTooltip("Previous image (Left arrow)");
    }
    x += back.width(fonts_) + gap;
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    drawList->AddText(fonts_.medium, fontSize, ImVec2(x, textY), theme::kText, position);
    x += positionWidth + textGap;
    drawList->AddText(ImVec2(x, textY), theme::kTextMuted, reviewed);
    x += reviewedWidth + gap;
    ImGui::SetCursorScreenPos(ImVec2(x, buttonY));
    if (forward.draw("##forward", fonts_)) {
        review_->next();
    }
    if (ImGui::IsItemHovered() && forward.enabled) {
        ImGui::SetTooltip("Next image (Right arrow)");
    } else if (ImGui::IsItemHovered() && review_->unansweredCount() > 0) {
        ImGui::SetTooltip("Answer every mark on this image first (%d left)", review_->unansweredCount());
    }
}

void App::drawRecordings() {
    // The Review button's list: every recording in the recordings folder, newest first. Picking one asks what to
    // look for in it next.
    if (!ImGui::BeginPopup(kRecordingsPopup)) {
        return;
    }
    const float dpi = dpiScale_;
    const ImVec2 tileSize(std::round(400.0f * dpi), std::round(58.0f * dpi));
    ImGui::PushFont(fonts_.medium, 18.0f);
    ImGui::TextUnformatted("Review a recording");
    ImGui::PopFont();
    // Wrapped to the list, so a long folder path does not stretch the popup.
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + tileSize.x + ImGui::GetStyle().ScrollbarSize);
    ImGui::TextDisabled("%s", toUtf8(settings_.recordingsFolder.c_str()).c_str());
    ImGui::PopTextWrapPos();
    ImGui::Dummy(ImVec2(0.0f, 4.0f * dpi));

    if (recordings_.empty()) {
        ImGui::TextUnformatted("No recordings yet.");
        ImGui::TextDisabled("Observe a screen and press Record (%s).", kHotkeyText);
        ImGui::EndPopup();
        return;
    }
    const ImVec2 padding(std::round(14.0f * dpi), std::round(10.0f * dpi));
    const float spacing = ImGui::GetStyle().ItemSpacing.y;
    const float listHeight = std::min(static_cast<float>(recordings_.size()) * (tileSize.y + spacing) - spacing,
                                      std::round(ImGui::GetMainViewport()->Size.y * 0.6f));
    ImGui::BeginChild("##recordings", ImVec2(tileSize.x + ImGui::GetStyle().ScrollbarSize, listHeight));
    for (std::size_t i = 0; i < recordings_.size(); ++i) {
        const RecordingRow& row = recordings_[i];
        const ImVec2 min = ImGui::GetCursorScreenPos();
        ImGui::PushID(static_cast<int>(i));
        const bool clicked = ImGui::InvisibleButton("##recording", tileSize);
        ImGui::PopID();
        const bool hovered = ImGui::IsItemHovered();
        if (hovered) {
            ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        }
        const ImVec2 max = ImGui::GetItemRectMax();
        ImDrawList* drawList = ImGui::GetWindowDrawList();
        drawList->AddRectFilled(min, max, hovered ? theme::kSurfaceActive : theme::kSurfaceHover, 8.0f * dpi);
        drawList->AddRect(min, max, hovered ? theme::kAccent : theme::kBorderHover, 8.0f * dpi, 0, 1.0f);
        drawList->AddText(fonts_.medium, ImGui::GetFontSize(), ImVec2(min.x + padding.x, min.y + padding.y),
                          theme::kText, row.when.c_str());

        // How long, how many images, and how far each category's review got: "00:34 · 12 images · Characters 11
        // of 12 reviewed".
        std::string detail;
        if (row.entry.info.seconds > 0) {
            char duration[16];
            formatDuration(duration, sizeof(duration), std::chrono::seconds(row.entry.info.seconds));
            detail = std::string(duration) + "  \xC2\xB7  ";
        }
        detail += std::to_string(row.images) + (row.images == 1 ? " image" : " images");
        bool reviewed = false;
        for (std::size_t c = 0; c < row.reviewed.size(); ++c) {
            if (row.reviewed[c] > 0) {
                char text[96];
                std::snprintf(text, sizeof(text), "  \xC2\xB7  %s %zu of %zu reviewed", kCategories[c].name,
                              row.reviewed[c], row.images);
                detail += text;
                reviewed = true;
            }
        }
        if (!reviewed) {
            detail += "  \xC2\xB7  not reviewed yet";
        }
        drawList->AddText(ImVec2(min.x + padding.x, max.y - padding.y - ImGui::GetTextLineHeight()), theme::kTextMuted,
                          detail.c_str());
        if (clicked) {
            if (row.images == 0) {
                reviewError_ = "No images in the recording of " + row.when;
            } else {
                reviewFolder_ = row.entry.folder;
                reviewWhen_ = row.when;
                reviewFolderImages_ = row.images;
                openCategory_ = true;
            }
            ImGui::CloseCurrentPopup();
        }
    }
    ImGui::EndChild();
    ImGui::EndPopup();
}

void App::drawCategoryChooser() {
    // After the recording: what to look for. Only characters for now; more categories are more tiles.
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(
        ImVec2(viewport->Pos.x + viewport->Size.x * 0.5f, viewport->Pos.y + viewport->Size.y * 0.4f), ImGuiCond_Always,
        ImVec2(0.5f, 0.5f));
    if (!ImGui::BeginPopupModal(kCategoryPopup, nullptr,
                                ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_AlwaysAutoResize |
                                    ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings)) {
        return;
    }
    const float dpi = dpiScale_;
    ImGui::PushFont(fonts_.medium, 18.0f);
    ImGui::TextUnformatted(kCategoryPopup);
    ImGui::PopFont();
    char where[300];
    std::snprintf(where, sizeof(where), "Recorded %s  \xC2\xB7  %zu images", reviewWhen_.c_str(), reviewFolderImages_);
    ImGui::TextDisabled("%s", where);
    ImGui::Dummy(ImVec2(0.0f, 6.0f * dpi));

    const ImVec2 tileSize(std::round(340.0f * dpi), std::round(60.0f * dpi));
    const ImVec2 padding(std::round(14.0f * dpi), std::round(11.0f * dpi));
    for (const Category& category : kCategories) {
        const ImVec2 min = ImGui::GetCursorScreenPos();
        ImGui::PushID(category.id);
        const bool clicked = ImGui::InvisibleButton("##category", tileSize);
        ImGui::PopID();
        const bool hovered = ImGui::IsItemHovered();
        if (hovered) {
            ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        }
        const ImVec2 max = ImGui::GetItemRectMax();
        ImDrawList* drawList = ImGui::GetWindowDrawList();
        drawList->AddRectFilled(min, max, hovered ? theme::kSurfaceActive : theme::kSurfaceHover, 8.0f * dpi);
        drawList->AddRect(min, max, hovered ? theme::kAccent : theme::kBorderHover, 8.0f * dpi, 0, 1.0f);
        drawList->AddText(fonts_.medium, ImGui::GetFontSize(), ImVec2(min.x + padding.x, min.y + padding.y),
                          theme::kText, category.name);
        drawList->AddText(ImVec2(min.x + padding.x, max.y - padding.y - ImGui::GetTextLineHeight()), theme::kTextMuted,
                          category.description);
        if (clicked) {
            ImGui::CloseCurrentPopup();
            startReview(category);
        }
    }
    ImGui::Dummy(ImVec2(0.0f, 2.0f * dpi));
    ImGui::TextDisabled("More kinds later: party members, terrain, the game's UI.");
    ImGui::Dummy(ImVec2(0.0f, 4.0f * dpi));
    if (ImGui::Button("Cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

void App::drawChooseScreen() {
    // Logo, title and the miniatures, centred in the window as one block.
    const ImVec2 area = ImGui::GetContentRegionAvail();
    const ImVec2 maxSize(std::min(area.x, 1100.0f * dpiScale_), std::max(area.y * 0.5f, 1.0f));
    const ImVec2 size = picker_->fit(maxSize);
    const float logoSize = std::round(64.0f * dpiScale_);
    const float gap = std::round(24.0f * dpiScale_);
    ImGui::PushFont(fonts_.medium, 20.0f);
    const float titleHeight = ImGui::GetTextLineHeight();
    const float blockHeight = logoSize + gap * 0.5f + titleHeight + gap + size.y;
    const float top = std::max(ImGui::GetCursorPosY(), (ImGui::GetWindowHeight() - blockHeight) * 0.45f);

    const ImVec2 window = ImGui::GetWindowPos();
    theme::drawLogo(ImGui::GetWindowDrawList(),
                    ImVec2(window.x + ImGui::GetWindowWidth() * 0.5f, window.y + top + logoSize * 0.5f), logoSize);
    centredText(kChooseText, theme::kText, top + logoSize + gap * 0.5f);
    ImGui::PopFont();

    ImGui::SetCursorPos(ImVec2(std::round((ImGui::GetWindowWidth() - maxSize.x) * 0.5f),
                               std::round(top + logoSize + gap * 0.5f + titleHeight + gap)));
    if (const Screen* chosen = picker_->draw(ImVec2(maxSize.x, size.y), theme::kBackground)) {
        chosen_ = *chosen;
    }
}

void App::drawObservedScreen() {
    const std::string& error =
        error_.empty() && pipeline_.state() == Pipeline::State::Failed ? pipeline_.error() : error_;
    const float middle = ImGui::GetWindowHeight() * 0.5f;
    if (!error.empty()) {
        centredText("This screen cannot be observed.", theme::kText, middle - ImGui::GetTextLineHeightWithSpacing());
        centredText(error.c_str(), theme::kTextMuted, middle);
        return;
    }
    if (!observedImage_ || !observedImage_->hasImage()) {
        centredText("Waiting for the first image\xE2\x80\xA6", theme::kTextMuted, middle);
        return;
    }

    // The screen, as large as fits, keeping its shape.
    const ImVec2 area = ImGui::GetContentRegionAvail();
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const float aspect = static_cast<float>(observedImage_->width()) / static_cast<float>(observedImage_->height());
    const ImVec2 size = area.x / area.y > aspect ? ImVec2(area.y * aspect, area.y) : ImVec2(area.x, area.x / aspect);
    const ImVec2 min(std::round(origin.x + (area.x - size.x) * 0.5f), std::round(origin.y + (area.y - size.y) * 0.5f));
    const ImVec2 max(min.x + std::round(size.x), min.y + std::round(size.y));
    const float rounding = 8.0f * dpiScale_;
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    observedImage_->draw(drawList, min, max, rounding, theme::kBackground);
    drawList->PathRect(ImVec2(min.x - 0.5f, min.y - 0.5f), ImVec2(max.x + 0.5f, max.y + 0.5f), rounding + 0.5f);
    drawList->PathStroke(theme::kBorder, ImDrawFlags_Closed, 1.0f);
    if (settings_.showCharacters && recognizer_ && recognizer_->state() == Recognizer::State::Running) {
        drawFound(drawList, min, max);
    }

    const float inset = 12.0f * dpiScale_;
    if (pipeline_.state() == Pipeline::State::Reconnecting) {
        theme::drawLabel(drawList, ImVec2((min.x + max.x) * 0.5f, min.y + inset), ImVec2(0.5f, 0.0f),
                         "Reconnecting\xE2\x80\xA6");
    }
    if (showStats_) {
        const PipelineStats& stats = pipeline_.stats();
        char text[128];
        std::snprintf(text, sizeof(text),
                      "%.0f images/s  \xC2\xB7  %.2f ms latency  \xC2\xB7  %.2f ms work  \xC2\xB7  UI %.0f fps",
                      static_cast<double>(stats.imagesPerSecond), static_cast<double>(stats.latencyAvgMs),
                      static_cast<double>(stats.workAvgMs), static_cast<double>(ImGui::GetIO().Framerate));
        char detail[128] = {};
        if (recognizer_ && recognizer_->state() == Recognizer::State::Running) {
            const RecognizerStats& found = recognizer_->stats();
            std::snprintf(detail, sizeof(detail),
                          "%s: %.0f images/s  \xC2\xB7  %.2f ms each  \xC2\xB7  %.2f ms slowest", kLive.name,
                          static_cast<double>(found.imagesPerSecond), static_cast<double>(found.workAvgMs),
                          static_cast<double>(found.workMaxMs));
        }
        theme::drawLabel(drawList, ImVec2(min.x + inset, max.y - inset), ImVec2(0.0f, 1.0f), text,
                         detail[0] != '\0' ? detail : nullptr);
    }
}

void App::drawFound(ImDrawList* drawList, ImVec2 min, ImVec2 max) {
    const Found& found = recognizer_->found();
    const float scale = (max.x - min.x) / static_cast<float>(pipeline_.width());
    const float thickness = 1.5f * dpiScale_;
    drawList->PushClipRect(min, max, true);
    for (int i = found.count - 1; i >= 0; --i) { // Surest last, so it is on top.
        const Detection& thing = found.things[static_cast<std::size_t>(i)];
        const ImVec2 boxMin(std::round(min.x + thing.x * scale), std::round(min.y + thing.y * scale));
        const ImVec2 boxMax(std::round(min.x + (thing.x + thing.width) * scale),
                            std::round(min.y + (thing.y + thing.height) * scale));
        theme::drawBox(drawList, boxMin, boxMax, theme::kAccent, thickness);
        char tag[32];
        std::snprintf(tag, sizeof(tag), "%s %.0f%%", kLive.yes, static_cast<double>(thing.sure * 100.0f));
        theme::drawTag(drawList, fonts_.medium, boxMin, thickness, min, max, tag, theme::kAccent,
                       theme::kBackground);
    }
    drawList->PopClipRect();
}

} // namespace observex
