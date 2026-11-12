#include "app/App.h"

#include "app/Overlay.h"
#include "app/Snapshot.h"
#include "app/Widgets.h"

#include <imgui.h>
#include <imgui_impl_dx11.h>
#include <imgui_impl_win32.h>

#include <shellapi.h>

#include <algorithm>
#include <cctype>
#include <cfloat>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <exception>
#include <initializer_list>

namespace observex {

namespace {

constexpr ImGuiWindowFlags kFixedWindow = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                                          ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus |
                                          ImGuiWindowFlags_NoScrollWithMouse;
constexpr char kPickerPopup[] = "screens";
constexpr char kSettingsPopup[] = "settings";
constexpr char kShowPopup[] = "show";
constexpr char kGuessesPopup[] = "guesses";
constexpr char kCategoryPopup[] = "What are we looking for?";
constexpr char kRecordPopup[] = "What do we record?";
constexpr char kRecordingsPopup[] = "recordings";
constexpr char kReviewPopup[] = "What do we review?";
constexpr char kClipsPopup[] = "clips";
constexpr char kRemoveClipPopup[] = "Remove this clip?";
constexpr char kChooseText[] = "Choose a screen to observe";
// How long Save last 10 s says "Saved 10.3 s" after a save.
constexpr auto kSavedShownFor = std::chrono::seconds(3);
// What live recognition looks for: its model folder, live log, Show flag and box tags.
constexpr const Category& kLive = kCategories[0];
// Seconds between saved images recording every 3 s. Neighbouring images are near copies, so the user asked for one
// every 3 s (2026-11-01) rather than more to review. Fixed, no setting. Only unsure asks every kUnsureEvery.
constexpr unsigned kRecordingInterval = 3;
constexpr int kSnapshotStepFrames = 36; // Snapshot frames are 16 ms apart, so each step gets about 0.6 s.

// The category's kinds by the words its answers files use ("monster", ...), in order.
std::vector<std::string> kindIdsOf(const Category& category) {
    std::vector<std::string> ids;
    for (const Kind& kind : category.kinds) {
        ids.emplace_back(kind.id);
    }
    return ids;
}

// "10,234": a count with its thousands apart.
std::string formatCount(std::size_t count) {
    std::string digits = std::to_string(count);
    for (std::size_t at = digits.size(); at > 3; at -= 3) {
        digits.insert(at - 3, 1, ',');
    }
    return digits;
}

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

// One choice in a chooser popup, as a tile: its name and, under it, what it means. Drawn at the cursor; returns true
// when clicked. A disabled one is dimmed and cannot be clicked.
bool choiceTile(const char* id, const char* name, const char* description, ImVec2 size, const theme::Fonts& fonts,
                float dpi, bool enabled = true) {
    const ImVec2 padding(std::round(14.0f * dpi), std::round(11.0f * dpi));
    const ImVec2 min = ImGui::GetCursorScreenPos();
    ImGui::PushID(id);
    ImGui::BeginDisabled(!enabled);
    const bool clicked = ImGui::InvisibleButton("##choice", size);
    ImGui::EndDisabled();
    ImGui::PopID();
    const bool hovered = enabled && ImGui::IsItemHovered();
    if (hovered) {
        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    }
    const ImVec2 max = ImGui::GetItemRectMax();
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    drawList->AddRectFilled(min, max, hovered ? theme::kSurfaceActive : theme::kSurfaceHover, 8.0f * dpi);
    drawList->AddRect(min, max, hovered ? theme::kAccent : theme::kBorderHover, 8.0f * dpi, 0, 1.0f);
    drawList->AddText(fonts.medium, ImGui::GetFontSize(), ImVec2(min.x + padding.x, min.y + padding.y),
                      enabled ? theme::kText : theme::kTextMuted, name);
    drawList->AddText(ImVec2(min.x + padding.x, max.y - padding.y - ImGui::GetTextLineHeight()), theme::kTextMuted,
                      description);
    return clicked;
}

// One row of a list popup (the recordings, the clips) as a tile: on the left a picture of the image file `picture`
// (an empty frame until it is read: only rows in view are read), right of it `title` and under it the `details`
// (empty ones left out), muted and cut at the tile's edge, together in the middle of the row. Drawn at the cursor;
// returns true when clicked.
bool pictureRow(int id, ImVec2 tileSize, Thumbnails& thumbnails, const std::filesystem::path& picture,
                std::string_view title, std::initializer_list<std::string_view> details, const theme::Fonts& fonts,
                float dpi) {
    const float inset = std::round(8.0f * dpi); // Around the picture.
    const ImVec2 pictureSize(std::round((tileSize.y - inset * 2.0f) * 16.0f / 9.0f), tileSize.y - inset * 2.0f);
    const float textGap = std::round(14.0f * dpi); // Between the picture and the text, and after the text.
    const float lineGap = std::round(6.0f * dpi);
    const ImVec2 min = ImGui::GetCursorScreenPos();
    ImGui::PushID(id);
    const bool clicked = ImGui::InvisibleButton("##row", tileSize);
    ImGui::PopID();
    const bool hovered = ImGui::IsItemHovered();
    if (hovered) {
        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    }
    const ImVec2 max = ImGui::GetItemRectMax();
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    const ImU32 tileColor = hovered ? theme::kSurfaceActive : theme::kSurfaceHover;
    drawList->AddRectFilled(min, max, tileColor, 8.0f * dpi);
    drawList->AddRect(min, max, hovered ? theme::kAccent : theme::kBorderHover, 8.0f * dpi, 0, 1.0f);

    // The picture, fitted into its frame keeping its shape; the frame stays empty if it cannot be read.
    const ImVec2 frameMin(min.x + inset, min.y + inset);
    const ImVec2 frameMax(frameMin.x + pictureSize.x, frameMin.y + pictureSize.y);
    const float pictureRounding = 5.0f * dpi;
    drawList->AddRectFilled(frameMin, frameMax, theme::kBackground, pictureRounding);
    const ScreenImage* image = ImGui::IsItemVisible() ? thumbnails.get(picture) : nullptr;
    if (image) {
        const float scale = std::min(pictureSize.x / static_cast<float>(image->width()),
                                     pictureSize.y / static_cast<float>(image->height()));
        const ImVec2 size(std::round(static_cast<float>(image->width()) * scale),
                          std::round(static_cast<float>(image->height()) * scale));
        const ImVec2 imageMin(std::round(frameMin.x + (pictureSize.x - size.x) * 0.5f),
                              std::round(frameMin.y + (pictureSize.y - size.y) * 0.5f));
        image->draw(drawList, imageMin, ImVec2(imageMin.x + size.x, imageMin.y + size.y), pictureRounding, tileColor);
    }

    const float textX = frameMax.x + textGap;
    const auto lines = static_cast<float>(
        std::count_if(details.begin(), details.end(), [](std::string_view detail) { return !detail.empty(); }));
    const float fontSize = ImGui::GetFontSize();
    const float lineHeight = ImGui::GetTextLineHeight();
    const float textTop = std::round(min.y + (tileSize.y - fontSize - (lineGap + lineHeight) * lines) * 0.5f);
    drawList->AddText(fonts.medium, fontSize, ImVec2(textX, textTop), theme::kText, title.data(),
                      title.data() + title.size());
    drawList->PushClipRect(ImVec2(textX, min.y), ImVec2(max.x - textGap, max.y), true);
    float y = textTop + fontSize + lineGap;
    for (const std::string_view detail : details) {
        if (!detail.empty()) {
            drawList->AddText(ImVec2(textX, y), theme::kTextMuted, detail.data(), detail.data() + detail.size());
            y += lineHeight + lineGap;
        }
    }
    drawList->PopClipRect();
    return clicked;
}

// A small round X with its top right corner at `corner`, as on a list row; returns true when clicked.
bool removeButton(int id, ImVec2 corner, float dpi) {
    const float size = std::round(24.0f * dpi);
    const float inset = std::round(8.0f * dpi);
    const ImVec2 min(corner.x - inset - size, corner.y + inset);
    ImGui::SetCursorScreenPos(min);
    ImGui::PushID(id);
    const bool clicked = ImGui::InvisibleButton("##remove", ImVec2(size, size));
    ImGui::PopID();
    const bool hovered = ImGui::IsItemHovered();
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    const ImVec2 centre(min.x + size * 0.5f, min.y + size * 0.5f);
    drawList->AddCircleFilled(centre, size * 0.5f, hovered ? theme::kRecord : theme::kSurface);
    const float half = std::round(4.0f * dpi);
    const ImU32 color = hovered ? theme::kText : theme::kTextMuted;
    const float stroke = 1.5f * dpi;
    drawList->AddLine(ImVec2(centre.x - half, centre.y - half), ImVec2(centre.x + half, centre.y + half), color, stroke);
    drawList->AddLine(ImVec2(centre.x - half, centre.y + half), ImVec2(centre.x + half, centre.y - half), color, stroke);
    return clicked;
}

// Moves `path` (a file or a folder) to the Recycle Bin, without asking. Returns an error message, empty if it went.
std::string moveToRecycleBin(const std::filesystem::path& path) {
    std::wstring from = path.wstring();
    from.push_back(L'\0'); // A list of paths, ended by an empty one.
    SHFILEOPSTRUCTW operation{};
    operation.wFunc = FO_DELETE;
    operation.pFrom = from.c_str();
    operation.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_NOERRORUI | FOF_SILENT;
    const int result = SHFileOperationW(&operation);
    if (result != 0 || operation.fAnyOperationsAborted) {
        return "Windows could not move it to the Recycle Bin (error " + std::to_string(result) + ")";
    }
    return {};
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

// The live model and when it was written: "...\models\character\model.onnx (written 2026-11-10 00:18)".
std::string liveModelAbout() {
    const std::filesystem::path model = modelFolder(kLive.id) / L"model.onnx";
    return toUtf8(model.c_str()) + " (written " + fileTime(model) + ")";
}

// `time` as a folder name, "2026-10-31_23-05-12", and as text, "2026-10-31 23:05:12".
std::string folderName(const SYSTEMTIME& time) {
    char name[40];
    std::snprintf(name, sizeof(name), "%04u-%02u-%02u_%02u-%02u-%02u", time.wYear, time.wMonth, time.wDay, time.wHour,
                  time.wMinute, time.wSecond);
    return name;
}

std::string timeText(const SYSTEMTIME& time) {
    char text[40];
    std::snprintf(text, sizeof(text), "%04u-%02u-%02u %02u:%02u:%02u", time.wYear, time.wMonth, time.wDay, time.wHour,
                  time.wMinute, time.wSecond);
    return text;
}

// `parent`/`name`, or `name`_2, _3, ... if that is taken.
std::filesystem::path freeFolder(const std::filesystem::path& parent, const std::string& name) {
    std::filesystem::path folder = parent / name;
    std::error_code error;
    for (int i = 2; std::filesystem::exists(folder, error); ++i) {
        folder = parent / (name + "_" + std::to_string(i));
    }
    return folder;
}

// A folder's name, for notes: "2026-11-10_18-22-05".
std::string nameOf(const std::filesystem::path& folder) {
    return toUtf8(folder.filename().c_str());
}

// A clip's time: "0:03.2", minutes, seconds and tenths.
void formatClipTime(char* text, std::size_t size, double seconds) {
    const auto tenths = static_cast<long long>(std::max(seconds, 0.0) * 10.0 + 0.5);
    std::snprintf(text, size, "%lld:%02lld.%lld", tenths / 600, tenths / 10 % 60, tenths % 10);
}

// `text` with every digit a 0: about as wide as any number of its shape, so text that counts does not move what is
// laid out by its width.
std::string zeroDigits(std::string text) {
    for (char& c : text) {
        if (c >= '1' && c <= '9') {
            c = '0';
        }
    }
    return text;
}

// When a recording started or was last reviewed, for the review's list: "Today, 12:56", "Yesterday, 22:05",
// "31 Oct, 23:59", or "31 Oct 2025, 23:59" in another year. `started` is local time as in RecordingInfo::started
// ("2026-10-31 23:59:49"); shown as it is if it is not that.
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
    SYSTEMTIME now;
    GetLocalTime(&now);
    namespace chrono = std::chrono;
    const chrono::year_month_day date{chrono::year(static_cast<int>(year)), chrono::month(month), chrono::day(day)};
    const chrono::year_month_day today{chrono::year(now.wYear), chrono::month(now.wMonth), chrono::day(now.wDay)};
    const chrono::year_month_day yesterday{chrono::sys_days(today) - chrono::days(1)};
    char text[40];
    if (date == today) {
        std::snprintf(text, sizeof(text), "Today, %02u:%02u", hour, minute);
    } else if (date == yesterday) {
        std::snprintf(text, sizeof(text), "Yesterday, %02u:%02u", hour, minute);
    } else if (year == now.wYear) {
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
        if (recorder_ && recorder_->failed()) {
            recordingError_ = "Recording stopped. " + recorder_->error();
            stopRecording("recording stopped by an error");
        }
        if (recording_ && recorder_) {
            const unsigned saved = recording_->saved + recorder_->saved();
            const double elapsed =
                std::chrono::duration<double>(std::chrono::steady_clock::now() - recording_->started).count();
            if (recordingDone(recording_->info.kind, saved, elapsed)) {
                const bool unsure = recording_->info.kind == RecordKind::Unsure;
                stopRecording("recording done");
                recordingError_ = "Recording done: " + std::to_string(saved) + (saved == 1 ? " image" : " images") +
                                  (unsure ? " in a minute." : ". Record again in a new place.");
            }
        }
        // How the last clip save went: the button says it is saved, or the error shows at the top right.
        if (clipSaving_ && clipRecorder_) {
            const ClipRecorder::SaveState state = clipRecorder_->saveState();
            if (state == ClipRecorder::SaveState::Saved) {
                clipSaving_ = false;
                clipSavedAt_ = std::chrono::steady_clock::now();
                noteInClip("clip saved: " + nameOf(clipRecorder_->savedFolder()));
            } else if (state == ClipRecorder::SaveState::Failed) {
                clipSaving_ = false;
                clipSaveError_ = "Could not save the clip: " + clipRecorder_->saveError();
            }
        }

        // Starting and stopping happen here, outside an ImGui frame, so no draw data points at freed images.
        if (closeReview_) {
            closeReview();
            closeReview_ = false;
        }
        if (closeClip_) {
            closeClip();
            closeClip_ = false;
        }
        if (clipToRemove_) {
            removeClip(*clipToRemove_);
            clipToRemove_.reset();
        }
        if (clipToOpen_) {
            openClip(*clipToOpen_);
            clipToOpen_.reset();
        }
        if (review_) {
            review_->update(); // The finder's progress, also while minimized.
            // Then the GPU is free for the other reviewed recordings' guesses, until the review closes.
            if (!review_->finding() && !refresher_) {
                const Category& category = review_->category();
                refresher_ = std::make_unique<Refresher>(settings_.recordingsFolder, category.id, kindIdsOf(category),
                                                         modelFolder(category.id), review_->folder());
            }
        }
        // A new kind update: live recognition picks kinds with it from its next look.
        if (kindUpdater_ && kindUpdater_->state() != KindUpdater::State::Running) {
            if (kindUpdater_->state() == KindUpdater::State::Done && recognizer_) {
                recognizer_->reloadKinds();
                noteInClip("kind update made: the recogniser reloads it");
            }
            kindUpdater_.reset();
        }
        if (chosen_) {
            closeReview(); // Picking a screen is asking to see it.
            closeClip();
            stopRecording();  // A recording is of one screen.
            picker_->close(); // Its capture of the chosen screen goes first; the pipeline takes over.
            observe(*chosen_);
            chosen_.reset();
        } else if (pipeline_.state() == Pipeline::State::NeedsRestart) {
            observeAgain();
        }
        // Recognition goes on whatever is shown, minimized too, except while reviewing (the user asked, 2026-11-01:
        // the review's finder gets the GPU) or while a clip is open (the user asked, 2026-11-11: it is offline data,
        // and the model on every image kept the GPU 66-93% busy). With no images the recogniser waits, model loaded,
        // and picks up again the moment the review or the clip closes. Recording only unsure saves nothing meanwhile:
        // nobody answers its questions.
        // observex_heuristics.dll rebuilt (the user asked, 2026-11-11: to see a heuristics change without closing the
        // app): the recogniser takes it up between two looks, and an open clip's Today is made again with it.
        if (heuristics_->poll()) {
            const std::string about = heuristics_->describe();
            noteInClip("heuristics loaded again: " + about);
            if (clipView_) {
                clipView_->setHeuristics(*todaysHeuristics(), todaysSnapping(), todaysGround(), about);
            }
        }
        const bool away = review_ || clipView_;
        pipeline_.setRecognitionEnabled(recognizer_ && recognizer_->state() != Recognizer::State::Failed && !away);
        pipeline_.setSnapEnabled(snapper_ && snapper_->state() != BoxSnapper::State::Failed && !away);
        // The UI finder goes on with it, the Show flag UI on or off (the user asked, 2026-11-12: the flag only draws
        // what it found).
        pipeline_.setUiEnabled(uiFinder_ && uiFinder_->state() != UiFinder::State::Failed && !away);
        // The ground finder only while its flag is on (nothing else needs it yet), afresh each time it starts again.
        const bool following =
            groundFinder_ && groundFinder_->state() != GroundFinder::State::Failed && !away && settings_.showGround;
        if (following && !groundFollowing_) {
            groundFinder_->restart();
        }
        groundFollowing_ = following;
        pipeline_.setGroundEnabled(following);
        if (following) {
            hintGround();
        }
        // The clip buffer pauses with it (its encoding took 21% of the video encoder): a clip is for what the
        // recogniser saw. Meanwhile it repeats the last image, so a clip saved right after still plays (still, with no
        // looks).
        pipeline_.setClipEnabled(clipRecorder_ && clipRecorder_->state() != ClipRecorder::State::Failed && !away);
        if (!window_->prepareFrame()) {
            pipeline_.setPreviewEnabled(false); // Minimized: nobody sees it.
            groundDrawn_.drawn = false;          // Nor the grid.
            noteGroundInClip();
            picker_->close();
            Sleep(10);
            continue;
        }
        // The review and a clip cover it.
        pipeline_.setPreviewEnabled(observedImage_ != nullptr && !review_ && !clipView_);
        if (ImageStamp stamp;
            preview_ && preview_->copyNewImage(gpu_.context.Get(), observedImage_->texture(), &stamp)) {
            observedImage_->imageCopied();
            previewMs_ = groundMs(stamp.shownQpc);
            previewQpc_ = stamp.shownQpc;
            previewImage_ = stamp.number;
        }
        if (clipView_) {
            clipView_->update();
        }

        // While reviewing or playing a clip, the arrow keys, Tab and Space belong to them, not to moving between
        // buttons.
        ImGuiIO& io = ImGui::GetIO();
        if (review_ || clipView_) {
            io.ConfigFlags &= ~ImGuiConfigFlags_NavEnableKeyboard;
        } else {
            io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
        }
        const bool saveSnapshot = snapshot_ && playSnapshot();
        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();
        groundDrawn_.drawn = false; // Until the live view draws the grid.
        drawFrame();
        noteGroundInClip();
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

    // observex_heuristics.dll next to the exe (the Release build puts both in the project folder).
    heuristics_ = std::make_shared<HeuristicsSource>(appFolder() / L"observex_heuristics.dll");

    gpuAdapter_ = findPrimaryGpu();
    gpu_ = createGpuDevice(gpuAdapter_.Get());
    window_ = std::make_unique<MainWindow>(L"observex", gpu_.device.Get(), dpiScale_, !snapshot_);
    picker_ = std::make_unique<ScreenPicker>(gpu_.device.Get(), gpuAdapter_.Get());
    thumbnails_ = std::make_unique<Thumbnails>(gpu_.device.Get());

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
    refresher_.reset();   // Its finder writes nothing.
    review_.reset();      // Closing observex while reviewing makes no kind update.
    clipView_.reset();    // Its image and reader are on the UI's device, which goes below.
    kindUpdater_.reset(); // One still at work writes nothing.
    stopRecording();
    stopObserving();
    picker_.reset();
    thumbnails_.reset();
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

    // The clip buffer: the last 10 s, all the time (the user asked, 2026-11-10).
    clipError_.clear();
    try {
        clipRecorder_ = std::make_shared<ClipRecorder>(screen, *pipeline_.clip());
        pipeline_.setClipEnabled(true);
    } catch (const std::exception& e) {
        clipError_ = e.what();
    }

    // The model tools/train made.
    recognizerError_.clear();
    try {
        // It answers recording only unsure's questions.
        recognizer_ = std::make_unique<Recognizer>(screen, *pipeline_.recognition(), modelFolder(kLive.id),
                                                   heuristics_, &pipeline_.question());
        recognizer_->setClip(clipRecorder_); // Every look goes into the clip buffer too.
    } catch (const std::exception& e) {
        recognizerError_ = e.what();
    }

    // The snapper: the recogniser's boxes put onto every new image (the user asked, 2026-11-11: boxes snapped to
    // the characters in a camera turn).
    snapperError_.clear();
    if (recognizer_) {
        try {
            snapper_ = std::make_shared<BoxSnapper>(screen, *pipeline_.snap(), heuristics_);
            snapper_->setClip(clipRecorder_); // So does every image's boxes.
            recognizer_->setSnapper(snapper_);
            pipeline_.setSnapEnabled(true);
        } catch (const std::exception& e) {
            snapperError_ = e.what();
        }
    }

    // The UI finder: where the game's interface is, for the Show flag UI (the user asked, 2026-11-12).
    uiFinderError_.clear();
    try {
        uiFinder_ = std::make_unique<UiFinder>(screen.adapter.Get(), *pipeline_.ui(), modelFolder("ui"));
    } catch (const std::exception& e) {
        uiFinderError_ = e.what();
    }

    // The ground finder: the game's camera, for the Show flag Ground (the user asked, 2026-11-12).
    groundFinderError_.clear();
    groundFollowing_ = false;
    try {
        groundFinder_ = std::make_unique<GroundFinder>(screen.adapter.Get(), *pipeline_.ground(), heuristics_);
    } catch (const std::exception& e) {
        groundFinderError_ = e.what();
    }
}

void App::stopObserving() {
    pauseRecording();    // The recorder reads from the pipeline's channel; it goes first.
    recognizer_.reset(); // So does the recogniser (it hands looks to the clip buffer and the snapper, so before them).
    pipeline_.setSnapEnabled(false);
    snapper_.reset(); // Reads from the pipeline's snap channel.
    pipeline_.setUiEnabled(false);
    uiFinder_.reset(); // Reads from its ui channel.
    pipeline_.setGroundEnabled(false);
    groundFinder_.reset(); // Reads from its ground channel.
    groundFollowing_ = false;
    pipeline_.setClipEnabled(false);
    clipRecorder_.reset(); // Reads from the pipeline's clip channel; a save still being written finishes first.
    clipSaving_ = false;
    preview_.reset(); // Before the pipeline frees the channel it reads from.
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

bool App::canRecordUnsure() const {
    return recognizer_ && recognizer_->state() != Recognizer::State::Failed;
}

void App::startRecording(RecordKind kind) {
    if (recording_ || !canRecord()) {
        return;
    }
    // Each recording gets its own folder, named by the date and time, e.g. 2026-10-31_23-05-12.
    SYSTEMTIME now;
    GetLocalTime(&now);
    const std::filesystem::path folder = freeFolder(settings_.recordingsFolder, folderName(now));
    recordingError_.clear();
    // What live recognition sees goes into the recording too (engine/LiveLog), to look at afterwards. A setting, on
    // by default.
    std::shared_ptr<LiveLog> log;
    if (settings_.liveLog) {
        try {
            char about[600];
            std::snprintf(about, sizeof(about), "Model %s, screen %s (%s), %u x %u.", liveModelAbout().c_str(),
                          observed_->monitor.c_str(), observed_->name.c_str(), pipeline_.width(), pipeline_.height());
            log = std::make_shared<LiveLog>(folder / (std::string(kLive.id) + "-live.txt"), about);
        } catch (const std::exception& e) {
            recordingError_ = std::string("Could not record: ") + e.what();
            return;
        }
    }
    recording_ = Recording{folder, std::chrono::steady_clock::now()};
    recording_->log = std::move(log);
    RecordingInfo& info = recording_->info;
    info.started = timeText(now);
    info.screen = observed_->monitor;
    info.width = pipeline_.width();
    info.height = pipeline_.height();
    info.every = kind == RecordKind::Unsure ? kUnsureEvery : kRecordingInterval;
    info.kind = kind;
    resumeRecording();
    // The folder exists once the recorder runs. Without the file the review would not list this recording.
    if (recording_ && !writeRecordingInfo(recording_->folder, recording_->info)) {
        recordingError_ = "Could not write " + toUtf8((recording_->folder / kRecordingFile).c_str());
    }
    if (recording_) {
        noteInClip(std::string("recording started: ") +
                   (kind == RecordKind::Unsure ? "only unsure" : "every " + std::to_string(kRecordingInterval) + " s") +
                   ", into " + nameOf(recording_->folder));
    }
}

void App::resumeRecording() {
    try {
        recorder_ = std::make_unique<Recorder>(*observed_, *pipeline_.recording(), recording_->folder,
                                               recording_->saved + 1, recording_->log);
        pipeline_.setRecordOnlyQuestions(recording_->info.kind == RecordKind::Unsure);
        pipeline_.setRecordingInterval(recording_->info.every);
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

void App::stopRecording(const char* why) {
    pauseRecording();
    if (recording_) {
        RecordingInfo& info = recording_->info;
        info.seconds = static_cast<unsigned>(
            std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - recording_->started)
                .count());
        info.images = recording_->saved;
        info.reviewed = readRecordingInfo(recording_->folder).reviewed; // If it was reviewed while it ran.
        if (!writeRecordingInfo(recording_->folder, info)) {
            recordingError_ = "Could not write " + toUtf8((recording_->folder / kRecordingFile).c_str());
        }
        noteInClip(std::string(why) + ": " + nameOf(recording_->folder) + ", " + std::to_string(info.images) +
                   " images");
    }
    recording_.reset();
}

bool App::canSaveClip() const {
    return clipRecorder_ && clipRecorder_->state() == ClipRecorder::State::Running &&
           clipRecorder_->saveState() != ClipRecorder::SaveState::Saving;
}

void App::saveClip() {
    if (!canSaveClip()) {
        return;
    }
    // Each clip gets its own folder in the clips folder, named by the date and time, e.g. 2026-11-10_18-22-05.
    SYSTEMTIME now;
    GetLocalTime(&now);
    std::filesystem::path folder = freeFolder(settings_.recordingsFolder / kClipsFolder, folderName(now));
    ClipAbout about;
    about.saved = timeText(now);
    about.screen = observed_->monitor;
    // So clip.txt says which model saw what it holds, and its kind numbers can be read.
    if (recognizer_ && recognizer_->state() == Recognizer::State::Running) {
        about.model = liveModelAbout();
        about.kinds = recognizer_->kinds();
    }
    noteInClip("save last 10 s");
    clipSaveError_.clear();
    if (clipRecorder_->save(std::move(folder), std::move(about))) {
        clipSaving_ = true;
    } else {
        clipSaveError_ = "Could not save the clip: it holds nothing yet.";
    }
}

void App::noteInClip(std::string_view text) {
    if (clipRecorder_) {
        clipRecorder_->note(text);
    }
}

void App::noteGroundInClip() {
    if (!clipRecorder_ || previewQpc_ == 0) {
        return;
    }
    // A line whenever it changes: drawn over a new image or from a new camera, or no longer drawn.
    const GroundDrawn& now = groundDrawn_;
    if (now.drawn == groundNoted_.drawn &&
        (!now.drawn || (now.image == groundNoted_.image && now.followed == groundNoted_.followed))) {
        return;
    }
    groundNoted_ = now;
    clipRecorder_->grounded(previewQpc_, previewImage_, now.drawn ? now.followed : 0,
                            now.drawn ? now.camera : GroundCamera{});
}

void App::saveSettings() {
    if (!settingsFile_.empty()) {
        settings_.save(settingsFile_);
    }
}

void App::openReviewChooser() {
    // Read once per opening (a few small files per recording and clip), so the chooser can count them.
    readRecordings();
    readClips();
    reviewError_.clear();
    ImGui::OpenPopup(kReviewPopup);
}

void App::readRecordings() {
    recordings_.clear();
    for (RecordingEntry& entry : listRecordings(settings_.recordingsFolder)) {
        RecordingRow row;
        row.when = describeWhen(entry.info.started);
        const std::vector<std::string> images = listImages(entry.folder);
        row.images = images.size();
        for (const std::string& image : images) { // UTF-8.
            std::error_code error;
            const std::uintmax_t size = std::filesystem::file_size(
                entry.folder / std::u8string(reinterpret_cast<const char8_t*>(image.data()), image.size()), error);
            row.bytes += error ? 0 : size;
        }
        if (!images.empty()) {
            const std::string& first = images.front(); // UTF-8.
            row.firstImage = entry.folder / std::u8string(reinterpret_cast<const char8_t*>(first.data()), first.size());
        }
        for (const Category& category : kCategories) {
            row.reviewed.push_back(Review::countReviewed(entry.folder, category));
        }
        // The latest of its categories' (the times sort as text).
        std::string reviewed;
        for (const auto& [category, time] : entry.info.reviewed) {
            reviewed = std::max(reviewed, time);
        }
        if (!reviewed.empty()) {
            std::string when = describeWhen(reviewed);
            when[0] = static_cast<char>(std::tolower(static_cast<unsigned char>(when[0]))); // "today"; a date: "6 Nov".
            row.lastReviewed = "Last reviewed " + when;
        }
        row.entry = std::move(entry);
        recordings_.push_back(std::move(row));
    }
}

void App::readClips() {
    clips_.clear();
    for (ClipEntry& entry : listClips(settings_.recordingsFolder / kClipsFolder)) {
        ClipRow row;
        row.when = describeWhen(entry.info.saved);
        std::error_code error;
        for (const auto& file : std::filesystem::directory_iterator(entry.folder, error)) {
            std::error_code sizeError;
            if (const std::uintmax_t size = file.file_size(sizeError); !sizeError && file.is_regular_file(sizeError)) {
                row.bytes += size;
            }
        }
        row.entry = std::move(entry);
        clips_.push_back(std::move(row));
    }
}

void App::startReview(const Category& category) {
    try {
        review_ = std::make_unique<Review>(gpu_.device.Get(), reviewFolder_, category);
        noteInClip("review opened: " + nameOf(reviewFolder_) + ", " + category.name);
        if (clipView_) {
            closeClip_ = true; // Between frames; the review is drawn instead meanwhile.
        }
    } catch (const std::exception& e) {
        reviewError_ = std::string("Could not open the recording: ") + e.what();
    }
}

void App::closeReview() {
    if (!review_) {
        return;
    }
    const Category& category = review_->category();
    noteInClip("review closed: " + nameOf(review_->folder()));
    refresher_.reset(); // Its finder writes nothing.
    review_.reset();    // Saves when, if it changed the answers.
    // Not in snapshot mode: it would write into models/, which tests must leave alone.
    if (!snapshot_) {
        kindUpdater_.reset(); // One still at work stops first: the new one learns from everything it would.
        kindUpdater_ = std::make_unique<KindUpdater>(settings_.recordingsFolder, category.id, kindIdsOf(category),
                                                     modelFolder(category.id));
    }
}

void App::openClip(const ClipEntry& clip) {
    closeReview(); // One thing at a time is shown instead of the screen.
    closeClip();
    clipView_ = std::make_unique<ClipView>(gpuAdapter_.Get(), gpu_.device.Get(), clip.folder, clip.info,
                                           *todaysHeuristics(), todaysSnapping(), todaysGround(), modelFolder("ui"));
    noteInClip("clip opened: " + nameOf(clip.folder));
}

std::unique_ptr<Heuristics> App::todaysHeuristics() const {
    try {
        return heuristics_->make();
    } catch (const std::exception&) {
        return HeuristicsSource().make();
    }
}

std::unique_ptr<Snapping> App::todaysSnapping() const {
    try {
        return heuristics_->makeSnapping();
    } catch (const std::exception&) {
        return HeuristicsSource().makeSnapping();
    }
}

std::unique_ptr<Grounding> App::todaysGround() const {
    try {
        return heuristics_->makeGround();
    } catch (const std::exception&) {
        return HeuristicsSource().makeGround();
    }
}

void App::closeClip() {
    if (!clipView_) {
        return;
    }
    noteInClip("clip closed: " + nameOf(clipView_->folder()));
    clipView_.reset();
}

void App::removeClip(const std::filesystem::path& folder) {
    if (clipView_ && clipView_->folder() == folder) {
        closeClip(); // Its player has the video open.
    }
    if (const std::string error = moveToRecycleBin(folder); !error.empty()) {
        reviewError_ = "Could not remove the clip " + nameOf(folder) + ": " + error;
    } else {
        noteInClip("clip removed: " + nameOf(folder));
    }
    readClips();
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
        if (!input.text.empty()) {
            if (frame == 1) {
                io.AddInputCharactersUTF8(input.text.c_str());
            }
            return false;
        }
        if (input.key != ImGuiKey_None) {
            if (frame == 1 || frame == 2) {
                const bool down = frame == 1;
                if (input.modifier != ImGuiMod_None && down) {
                    io.AddKeyEvent(input.modifier, true);
                }
                io.AddKeyEvent(input.key, down);
                if (input.modifier != ImGuiMod_None && !down) {
                    io.AddKeyEvent(input.modifier, false);
                }
            }
            return false;
        }
        if (input.wheel != 0.0f) {
            if (frame == 0) {
                io.AddMousePosEvent(input.from.x, input.from.y);
            } else if (frame == 1) {
                io.AddMouseWheelEvent(0.0f, input.wheel);
            }
            return false;
        }
        // Press at `from`, move to `to` over a few frames (none for a click), let go.
        const bool drag = input.from.x != input.to.x || input.from.y != input.to.y;
        const int moveFrames = drag ? 10 : 0;
        if (frame == 0) {
            io.AddMousePosEvent(input.from.x, input.from.y);
        } else if (frame == 1) {
            io.AddMouseButtonEvent(input.button, true);
        } else if (frame <= 1 + moveFrames) {
            const float t = static_cast<float>(frame - 1) / static_cast<float>(moveFrames);
            io.AddMousePosEvent(input.from.x + (input.to.x - input.from.x) * t,
                                input.from.y + (input.to.y - input.from.y) * t);
        } else if (frame == 2 + moveFrames) {
            io.AddMouseButtonEvent(input.button, false);
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
    } else if (clipView_) {
        // 1 and 2 pick the guesser in the Guesses popup's order (drawing characters), 3 turns Characters off or on: to
        // flip between them on one frame.
        if (!ImGui::GetIO().WantTextInput) {
            for (const auto [key, guesser] : {std::pair(ImGuiKey_1, ClipView::Guesser::Recorded),
                                              std::pair(ImGuiKey_2, ClipView::Guesser::Today)}) {
                if (ImGui::IsKeyPressed(key, false)) {
                    pickGuesser(guesser);
                }
            }
            if (ImGui::IsKeyPressed(ImGuiKey_3, false)) {
                showCharacters(!settings_.showCharacters);
            }
        }
        clipView_->draw(fonts_, settings_.showCharacters, settings_.showKinds, settings_.showUi, settings_.showGround,
                        dpiScale_);
    } else if (observed_) {
        drawObservedScreen();
    } else {
        drawChooseScreen();
    }
    // Errors stack at the top right.
    const float inset = std::round(12.0f * dpiScale_);
    ImVec2 corner(viewport->Pos.x + viewport->Size.x - inset, viewport->Pos.y + barHeight + inset);
    for (const std::string* error : {&recordingError_, &reviewError_, &clipSaveError_}) {
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
    char detail[128];
    if (recording_) {
        // How far it is and how long is left, so the user knows how much more to walk around (the user asked,
        // 2026-11-02 and 03): "02:15 · 38 of 110 images · about 4 min left", or for only unsure, which records for a
        // minute, "00:23 · 12 images · 37 s left".
        const RecordKind kind = recording_->info.kind;
        const unsigned saved = recording_->saved + (recorder_ ? recorder_->saved() : 0);
        const auto elapsed = std::chrono::steady_clock::now() - recording_->started;
        const double elapsedSeconds = std::chrono::duration<double>(elapsed).count();
        char duration[16];
        formatDuration(duration, sizeof(duration), elapsed);
        const double secondsLeft = recordingSecondsLeft(kind, recording_->info.every, saved, elapsedSeconds);
        if (kind == RecordKind::Unsure) {
            std::snprintf(detail, sizeof(detail), "%s  \xC2\xB7  %u %s  \xC2\xB7  %.0f s left", duration, saved,
                          saved == 1 ? "image" : "images", std::ceil(secondsLeft));
        } else {
            char left[32];
            if (secondsLeft < 60.0) {
                std::snprintf(left, sizeof(left), "under a minute left");
            } else {
                std::snprintf(left, sizeof(left), "about %.0f min left", secondsLeft / 60.0);
            }
            std::snprintf(detail, sizeof(detail), "%s  \xC2\xB7  %u of %u images  \xC2\xB7  %s", duration, saved,
                          kRecordImages, left);
        }
        recordButton.progress = recordingProgress(kind, saved, elapsedSeconds);
        recordButton.icon = Icon::Stop;
        recordButton.text = "Stop";
        recordButton.detail = detail;
        recordButton.open = true;
        recordButton.highlight = theme::kRecord;
    } else {
        recordButton.icon = Icon::Record;
        recordButton.text = "Record";
        recordButton.enabled = canRecord();
        recordButton.open = ImGui::IsPopupOpen(kRecordPopup);
    }
    x -= std::round(8.0f * dpiScale_) + recordButton.width(fonts_);
    ImGui::SetCursorScreenPos(ImVec2(x, buttonY));
    if (recordButton.draw("##record", fonts_)) {
        if (recording_) {
            stopRecording();
        } else {
            ImGui::OpenPopup(kRecordPopup); // Which kind, then it starts.
        }
    }
    if (ImGui::IsItemHovered() && !recordButton.enabled) {
        ImGui::SetTooltip("Choose a screen to record it");
    } else if (ImGui::IsItemHovered() && recording_) {
        char bytes[16];
        formatBytes(bytes, sizeof(bytes), recording_->savedBytes + (recorder_ ? recorder_->savedBytes() : 0));
        if (recording_->info.kind == RecordKind::Unsure) {
            ImGui::SetTooltip("Saving an image when the model is unsure, at most one a second, for a minute "
                              "(%s so far)",
                              bytes);
        } else {
            ImGui::SetTooltip("Saving an image every %u s, until %u (%s so far)", kRecordingInterval, kRecordImages,
                              bytes);
        }
    }
    ImGui::SetNextWindowPos(ImVec2(ImGui::GetItemRectMax().x, ImGui::GetItemRectMax().y + popupGap), ImGuiCond_Always,
                            ImVec2(1.0f, 0.0f));
    if (ImGui::BeginPopup(kRecordPopup)) {
        drawRecordChooser();
        ImGui::EndPopup();
    }

    // Save last 10 s, left of Record: saves what the clip buffer holds as a clip (the user asked, 2026-11-10). After a
    // save it says how long the clip is for a few seconds.
    BarButton saveButton;
    saveButton.icon = Icon::Replay;
    const ClipRecorder::SaveState saveState =
        clipRecorder_ ? clipRecorder_->saveState() : ClipRecorder::SaveState::None;
    const bool saving = saveState == ClipRecorder::SaveState::Saving;
    const bool justSaved =
        saveState == ClipRecorder::SaveState::Saved && std::chrono::steady_clock::now() - clipSavedAt_ < kSavedShownFor;
    char saveText[32];
    if (saving) {
        saveButton.text = "Saving\xE2\x80\xA6";
        saveButton.open = true;
        saveButton.enabled = false;
    } else if (justSaved) {
        std::snprintf(saveText, sizeof(saveText), "Saved %.1f s", clipRecorder_->savedInfo().seconds);
        saveButton.text = saveText;
        saveButton.open = true;
        saveButton.enabled = canSaveClip();
    } else {
        std::snprintf(saveText, sizeof(saveText), "Save last %.0f s", kClipSeconds);
        saveButton.text = saveText;
        saveButton.enabled = canSaveClip();
    }
    x -= std::round(8.0f * dpiScale_) + saveButton.width(fonts_);
    ImGui::SetCursorScreenPos(ImVec2(x, buttonY));
    if (saveButton.draw("##saveclip", fonts_)) {
        saveClip();
    }
    if (ImGui::IsItemHovered()) {
        if (saving) {
            ImGui::SetTooltip("Saving the last %.0f s into %s", kClipSeconds,
                              toUtf8(clipRecorder_->savedFolder().c_str()).c_str());
        } else if (!observed_) {
            ImGui::SetTooltip("Choose a screen first: while one is observed, its last %.0f s are kept to save",
                              kClipSeconds);
        } else if (!clipRecorder_) {
            ImGui::SetTooltip("%s", clipError_.empty() ? "The screen is not captured, so there is nothing to save"
                                                       : ("The clip buffer could not start: " + clipError_).c_str());
        } else if (clipRecorder_->state() == ClipRecorder::State::Failed) {
            ImGui::SetTooltip("The clip buffer stopped: %s", clipRecorder_->error().c_str());
        } else if (clipRecorder_->state() == ClipRecorder::State::Starting) {
            ImGui::SetTooltip("The clip buffer is starting\xE2\x80\xA6");
        } else {
            ImGui::SetTooltip("Saves the last %.0f s as a clip, to watch in Review.", kClipSeconds);
        }
    }

    // Review, left of Save: asks what to review, training recordings (answer what observex found in one) or clips
    // (play one); or closes the review or the clip.
    BarButton reviewButton;
    if (review_) {
        reviewButton.icon = Icon::Close;
        reviewButton.text = "Close review";
    } else if (clipView_) {
        reviewButton.icon = Icon::Close;
        reviewButton.text = "Close clip";
    } else {
        reviewButton.icon = Icon::Picture;
        reviewButton.text = "Review";
        reviewButton.open = ImGui::IsPopupOpen(kReviewPopup) || ImGui::IsPopupOpen(kRecordingsPopup) ||
                            ImGui::IsPopupOpen(kClipsPopup) || ImGui::IsPopupOpen(kCategoryPopup);
    }
    x -= std::round(8.0f * dpiScale_) + reviewButton.width(fonts_);
    ImGui::SetCursorScreenPos(ImVec2(x, buttonY));
    if (reviewButton.draw("##review", fonts_)) {
        if (review_) {
            closeReview_ = true;
        } else if (clipView_) {
            closeClip_ = true;
        } else {
            openReviewChooser();
        }
    }
    if (ImGui::IsItemHovered() && !review_ && !clipView_ && !reviewButton.open) {
        ImGui::SetTooltip("Answer what observex found in a training recording, or play a clip");
    }
    // The chooser and both lists hang under the button.
    const ImVec2 reviewCorner(ImGui::GetItemRectMax().x, ImGui::GetItemRectMax().y + popupGap);
    ImGui::SetNextWindowPos(reviewCorner, ImGuiCond_Always, ImVec2(1.0f, 0.0f));
    drawReviewChooser();
    if (openList_ != nullptr) { // Here, not inside the chooser's popup, so the list's name means the same popup.
        ImGui::OpenPopup(openList_);
        openList_ = nullptr;
    }
    ImGui::SetNextWindowPos(reviewCorner, ImGuiCond_Always, ImVec2(1.0f, 0.0f));
    drawRecordings();
    ImGui::SetNextWindowPos(reviewCorner, ImGuiCond_Always, ImVec2(1.0f, 0.0f));
    drawClips();
    if (openCategory_) { // Here, not inside the list's popup, so the chooser's name means the same popup.
        openCategory_ = false;
        ImGui::OpenPopup(kCategoryPopup);
    }
    drawCategoryChooser();
    const float stepsRight = x - std::round(16.0f * dpiScale_); // Left of the Review button.
    if (review_) {
        drawReviewSteps(height, stepsRight);
    } else if (clipView_) {
        drawClipSteps(height, stepsRight);
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
    if (observed_ && !review_ && !clipView_) {
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

    // Over a clip instead: Guesses, whose guesses are drawn over it (the user asked, 2026-11-11, like Show).
    if (clipView_) {
        BarButton guessesButton;
        guessesButton.icon = Icon::Guesses;
        guessesButton.caption = "Guesses";
        guessesButton.text = !settings_.showCharacters && !settings_.showGround   ? "Hidden"
                             : clipView_->guesser() == ClipView::Guesser::Recorded ? "Recorded"
                                                                                   : "Today";
        guessesButton.arrow = true;
        guessesButton.open = ImGui::IsPopupOpen(kGuessesPopup);
        ImGui::SetCursorScreenPos(ImVec2(screenMax.x + std::round(8.0f * dpiScale_), buttonY));
        if (guessesButton.draw("##guesses", fonts_)) {
            ImGui::OpenPopup(kGuessesPopup);
        }
        guessesRight_ = ImGui::GetItemRectMax().x; // For the clip's buttons in the middle, next frame.
        if (ImGui::IsItemHovered() && !guessesButton.open) {
            ImGui::SetTooltip("What is drawn over the clip: recorded or today's characters (1, 2), or none (3)");
        }
        ImGui::SetNextWindowPos(ImVec2(ImGui::GetItemRectMin().x, ImGui::GetItemRectMax().y + popupGap));
        if (ImGui::BeginPopup(kGuessesPopup)) {
            drawGuessers();
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

    // The live log (engine/LiveLog): what the model sees, kept with each recording to look at afterwards. It
    // applies from the next recording.
    if (ImGui::Checkbox("Log what the model sees while recording", &settings_.liveLog)) {
        saveSettings();
    }
}

void App::drawRecordChooser() {
    // The Record button asks what to save (the user asked, 2026-11-02): images at a steady pace, or only those the
    // model is unsure about (for a minute, the user asked, 2026-11-12).
    const float dpi = dpiScale_;
    ImGui::PushFont(fonts_.medium, 18.0f);
    ImGui::TextUnformatted(kRecordPopup);
    ImGui::PopFont();
    ImGui::Dummy(ImVec2(0.0f, 4.0f * dpi));

    const ImVec2 tileSize(std::round(400.0f * dpi), std::round(60.0f * dpi));
    char name[32];
    std::snprintf(name, sizeof(name), "Every %u s", kRecordingInterval);
    char description[96];
    const bool canUnsure = canRecordUnsure();
    constexpr char kNeedsModel[] = "Needs the model, and it is not running";
    std::optional<RecordKind> picked;
    std::snprintf(description, sizeof(description), "%u images, one every %u seconds, whatever is on the screen",
                  kRecordImages, kRecordingInterval);
    if (choiceTile("every", name, description, tileSize, fonts_, dpi)) {
        picked = RecordKind::Every;
    }
    static_assert(kUnsureSeconds == 60, "the tile says 1 minute");
    std::snprintf(description, sizeof(description), "1 minute, at most an image a second, when the model is unsure");
    if (choiceTile("unsure", "Only unsure", canUnsure ? description : kNeedsModel, tileSize, fonts_, dpi, canUnsure)) {
        picked = RecordKind::Unsure;
    }
    if (picked) {
        ImGui::CloseCurrentPopup();
        startRecording(*picked);
    }
}

void App::drawShowFlags() {
    // Each flag only says what is drawn: hiding characters does not stop looking for them. No explanations under the
    // flags (the user asked, 2026-11-12); only why there is nothing to draw, while there is not.
    ImGui::TextDisabled("Draw over the screen");
    ImGui::Dummy(ImVec2(0.0f, 2.0f * dpiScale_));
    if (bool on = settings_.showCharacters; ImGui::Checkbox(kLive.name, &on)) {
        showCharacters(on);
    }

    char why[256] = {};
    if (!recognizer_) {
        std::snprintf(why, sizeof(why), "%s",
                      recognizerError_.empty() ? "Not looking: the screen is not captured." : recognizerError_.c_str());
    } else if (recognizer_->state() == Recognizer::State::Starting) {
        const float preparing = recognizer_->preparing();
        if (preparing > 0.0f) {
            std::snprintf(why, sizeof(why), "Preparing the model for this GPU: %.0f%%",
                          static_cast<double>(preparing * 100.0f));
        } else {
            std::snprintf(why, sizeof(why), "Loading the model\xE2\x80\xA6");
        }
    } else if (recognizer_->state() == Recognizer::State::Failed) {
        std::snprintf(why, sizeof(why), "%s", recognizer_->error().c_str());
    } else if (!snapper_) {
        std::snprintf(why, sizeof(why), "Not snapped onto every image: %s", snapperError_.c_str());
    } else if (snapper_->state() == BoxSnapper::State::Failed) {
        std::snprintf(why, sizeof(why), "Not snapped onto every image any more: %s", snapper_->error().c_str());
    }
    const float indent = ImGui::GetFrameHeight() + ImGui::GetStyle().ItemInnerSpacing.x; // Under the flag.
    if (why[0] != '\0') {
        ImGui::Indent(indent);
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + std::round(340.0f * dpiScale_));
        ImGui::TextDisabled("%s", why);
        ImGui::PopTextWrapPos();
        ImGui::Unindent(indent);
    }

    // Kinds (the user asked, 2026-11-02): which kind each character is, once the model has learnt them.
    const bool knowsKinds =
        recognizer_ && recognizer_->state() == Recognizer::State::Running && !recognizer_->kinds().empty();
    ImGui::BeginDisabled(!knowsKinds || !settings_.showCharacters);
    if (ImGui::Checkbox("Kinds", &settings_.showKinds)) {
        saveSettings();
        noteInClip(settings_.showKinds ? "show Kinds on" : "show Kinds off");
    }
    ImGui::EndDisabled();

    // UI (the user asked, 2026-11-12): the game's interface, every piece the UI finder found, to check it finds them
    // all (a depth model is to leave them out later).
    if (ImGui::Checkbox("UI", &settings_.showUi)) {
        saveSettings();
        noteInClip(settings_.showUi ? "show UI on" : "show UI off");
    }
    why[0] = '\0';
    if (!uiFinder_) {
        std::snprintf(why, sizeof(why), "%s",
                      uiFinderError_.empty() ? "Not looking: the screen is not captured." : uiFinderError_.c_str());
    } else if (uiFinder_->state() == UiFinder::State::Failed) {
        std::snprintf(why, sizeof(why), "%s", uiFinder_->error().c_str());
    } else {
        std::snprintf(why, sizeof(why), "%s", uiFinder_->stats().problem.data()); // Found with too little.
    }
    if (why[0] != '\0') {
        ImGui::Indent(indent);
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + std::round(340.0f * dpiScale_));
        ImGui::TextDisabled("%s", why);
        ImGui::PopTextWrapPos();
        ImGui::Unindent(indent);
    }

    // Ground (the user asked, 2026-11-12): the ground plane the camera sees, turning with it: the first step to
    // knowing how far things are.
    if (ImGui::Checkbox("Ground", &settings_.showGround)) {
        saveSettings();
        noteInClip(settings_.showGround ? "show Ground on" : "show Ground off");
    }
    why[0] = '\0';
    if (!groundFinder_) {
        std::snprintf(why, sizeof(why), "%s",
                      groundFinderError_.empty() ? "Not looking: the screen is not captured."
                                                 : groundFinderError_.c_str());
    } else if (groundFinder_->state() == GroundFinder::State::Failed) {
        std::snprintf(why, sizeof(why), "%s", groundFinder_->error().c_str());
    }
    if (why[0] != '\0') {
        ImGui::Indent(indent);
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + std::round(340.0f * dpiScale_));
        ImGui::TextDisabled("%s", why);
        ImGui::PopTextWrapPos();
        ImGui::Unindent(indent);
    }
}

void App::showCharacters(bool on) {
    settings_.showCharacters = on;
    saveSettings();
    noteInClip(std::string("show ") + kLive.name + (on ? " on" : " off"));
}

void App::pickGuesser(ClipView::Guesser guesser) {
    clipView_->setGuesser(guesser);
    if (!settings_.showCharacters && !settings_.showGround) {
        showCharacters(true);
    }
}

void App::drawGuessers() {
    // The live view's flags (the user asked, 2026-11-12: Characters as in Show, it was a guesser "Nothing" before),
    // with whose characters under Characters.
    ImGui::TextDisabled("Draw over the clip");
    ImGui::Dummy(ImVec2(0.0f, 2.0f * dpiScale_));
    if (bool on = settings_.showCharacters; ImGui::Checkbox(kLive.name, &on)) {
        showCharacters(on);
    }
    ImGui::SameLine();
    ImGui::TextDisabled("3");

    struct Choice {
        ClipView::Guesser guesser;
        const char* name;
        const char* key;
        const char* about; // One sentence (the user asked, 2026-11-12).
    };
    const Choice choices[] = {
        {ClipView::Guesser::Recorded, "Recorded", "1", "What observex showed while you played."},
        {ClipView::Guesser::Today, "Today's heuristics", "2", "What observex would show now, with today's heuristics."},
    };
    const std::string& missing = clipView_->guessesError();
    const float indent = ImGui::GetFrameHeight() + ImGui::GetStyle().ItemInnerSpacing.x; // Under the flag.
    const float wrap = std::round(380.0f * dpiScale_);
    // Under Characters, off with it and Ground as Kinds in Show (keys 1 and 2 turn Characters on again): whose
    // characters, and whose grid (Recorded's as drawn live).
    ImGui::Indent(indent);
    for (const Choice& choice : choices) {
        ImGui::BeginDisabled((!settings_.showCharacters && !settings_.showGround) || !missing.empty());
        if (ImGui::RadioButton(choice.name, clipView_->guesser() == choice.guesser)) {
            pickGuesser(choice.guesser);
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::TextDisabled("%s", choice.key);
        ImGui::Indent(indent);
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + wrap);
        ImGui::TextDisabled("%s", choice.about);
        if (choice.guesser == ClipView::Guesser::Today) {
            // Only when something went wrong: Today then is not what observing would show.
            for (const std::string* error : {&clipView_->todaySnapError(), &heuristics_->error()}) {
                if (!error->empty()) {
                    ImGui::PushStyleColor(ImGuiCol_Text, theme::kRecord);
                    ImGui::TextUnformatted(error->c_str());
                    ImGui::PopStyleColor();
                }
            }
        }
        ImGui::PopTextWrapPos();
        ImGui::Unindent(indent);
        ImGui::Dummy(ImVec2(0.0f, 4.0f * dpiScale_));
    }
    if (!missing.empty()) {
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + wrap);
        ImGui::TextDisabled("%s", missing.c_str());
        ImGui::PopTextWrapPos();
        ImGui::Dummy(ImVec2(0.0f, 4.0f * dpiScale_));
    }
    ImGui::Unindent(indent);
    // Kinds and UI, the same flags as the live view's (the user asked, 2026-11-12).
    ImGui::BeginDisabled(!settings_.showCharacters);
    if (ImGui::Checkbox("Kinds", &settings_.showKinds)) {
        saveSettings();
    }
    ImGui::EndDisabled();
    if (ImGui::Checkbox("UI", &settings_.showUi)) {
        saveSettings();
    }
    if (const std::string why = clipView_->uiProblem(); !why.empty()) { // Only why there is nothing, or too little.
        ImGui::Indent(indent);
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + wrap);
        ImGui::TextDisabled("%s", why.c_str());
        ImGui::PopTextWrapPos();
        ImGui::Unindent(indent);
    }
    // Ground (the user asked, 2026-11-12): the guesser's grid, Recorded's as the app drew it live.
    if (ImGui::Checkbox("Ground", &settings_.showGround)) {
        saveSettings();
    }
    if (const std::string why = clipView_->groundProblem(); !why.empty()) {
        ImGui::Indent(indent);
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + wrap);
        ImGui::TextDisabled("%s", why.c_str());
        ImGui::PopTextWrapPos();
        ImGui::Unindent(indent);
    }
}

void App::drawReviewSteps(float height, float right) {
    // In the middle of the bar: back, where we are, forward; moved left of `right` (the bar's buttons on the right)
    // in a narrow window.
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
    if (const std::size_t toCheck = review_->toCheckCount(); toCheck > 0) {
        std::snprintf(reviewed, sizeof(reviewed), "%zu reviewed  \xC2\xB7  %zu to check", review_->reviewedCount(),
                      toCheck);
    } else {
        std::snprintf(reviewed, sizeof(reviewed), "%zu reviewed", review_->reviewedCount());
    }
    const float fontSize = ImGui::GetFontSize();
    const float positionWidth = fonts_.medium->CalcTextSizeA(fontSize, FLT_MAX, 0.0f, position).x;
    const float reviewedWidth = ImGui::CalcTextSize(reviewed).x;
    const float textGap = std::round(8.0f * dpiScale_);
    const float total =
        back.width(fonts_) + gap + positionWidth + textGap + reviewedWidth + gap + forward.width(fonts_);

    const ImVec2 pos = ImGui::GetWindowPos();
    const float buttonY = pos.y + std::round((height - BarButton::kHeight * dpiScale_) * 0.5f);
    const float textY = pos.y + std::round((height - fontSize) * 0.5f);
    float x = std::round(std::min(pos.x + (viewport->Size.x - total) * 0.5f, right - total));
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
    // Shift+Right: on to the next image with something to check, while there is one.
    const char* skip = review_->toCheckCount() > 0 ? "\nShift+Right: next image to check" : "";
    if (ImGui::IsItemHovered() && forward.enabled && review_->kindsToCheck()) {
        ImGui::SetTooltip("The kinds here are right: next image (Right arrow)%s", skip);
    } else if (ImGui::IsItemHovered() && forward.enabled) {
        ImGui::SetTooltip("Next image (Right arrow)%s", skip);
    } else if (ImGui::IsItemHovered() && review_->unansweredCount() > 0) {
        ImGui::SetTooltip("Answer every mark on this image first (%d left)", review_->unansweredCount());
    }
}

void App::drawClipSteps(float height, float right) {
    // In the middle of the bar: the first frame, a frame back, play or pause, a frame on, the last (the user asked,
    // 2026-11-11; the first and the last later that day), where the clip is ("0:03.2 / 0:10.3") and the frame shown,
    // numbered as in clip.txt, in a field to type another into ("Frame [38] of 1,066": Enter goes there, Escape
    // leaves it, Ctrl+G types into it; the user asked the same day). Laid out by the widest the numbers get, so
    // nothing moves while they count; moved left of `right` (the bar's buttons on the right) in a narrow window, but
    // never over the Guesses button on the left: where it still does not fit, the word "Frame" goes first, then
    // "of 1,066", then some of the space between, then the clip's length, then the time.
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    float gap = std::round(12.0f * dpiScale_);
    const float textGap = std::round(8.0f * dpiScale_);
    const float fieldGap = std::round(6.0f * dpiScale_);
    float buttonGap = std::round(4.0f * dpiScale_);
    const bool canPlay = clipView_->canPlay();
    BarButton first;
    first.icon = Icon::ToStart;
    first.enabled = canPlay;
    BarButton back;
    back.icon = Icon::StepBack;
    back.enabled = canPlay;
    BarButton play;
    play.icon = clipView_->playing() ? Icon::Pause : Icon::Play;
    play.enabled = canPlay;
    BarButton on;
    on.icon = Icon::StepForward;
    on.enabled = canPlay;
    BarButton last;
    last.icon = Icon::ToEnd;
    last.enabled = canPlay;

    char position[16];
    char length[16];
    formatClipTime(position, sizeof(position), clipView_->position());
    formatClipTime(length, sizeof(length), clipView_->seconds());
    const std::string time = std::string(position) + " / " + length;
    const std::string widestTime = zeroDigits(std::string(length) + " / " + length);
    const std::string widestPosition = zeroDigits(length); // Where it is alone, in a narrow window.
    const std::uint64_t frames = clipView_->frames() > 0 ? clipView_->frames() : clipView_->info().frames;
    const std::string of = "of " + formatCount(frames);
    const std::string widestNumber = zeroDigits(std::to_string(std::max<std::uint64_t>(frames, 1000)));

    const float fontSize = ImGui::GetFontSize();
    const ImVec2 fieldPadding(std::round(6.0f * dpiScale_), ImGui::GetStyle().FramePadding.y);
    const float timeWidth = fonts_.medium->CalcTextSizeA(fontSize, FLT_MAX, 0.0f, widestTime.c_str()).x;
    const float positionWidth = fonts_.medium->CalcTextSizeA(fontSize, FLT_MAX, 0.0f, widestPosition.c_str()).x;
    const float labelWidth = ImGui::CalcTextSize("Frame").x;
    const float fieldWidth = std::round(ImGui::CalcTextSize(widestNumber.c_str()).x + fieldPadding.x * 2.0f);
    const float ofWidth = ImGui::CalcTextSize(of.c_str()).x;
    const float buttonsWidth = first.width(fonts_) + back.width(fonts_) + play.width(fonts_) + on.width(fonts_) +
                               last.width(fonts_);
    const ImVec2 pos = ImGui::GetWindowPos();
    const float leftmost = guessesRight_ + std::round(8.0f * dpiScale_);
    const float room = right - leftmost;
    bool showLabel = true;
    bool showOf = true;
    bool spacious = true;   // The gaps as wide as elsewhere on the bar.
    bool showLength = true; // "0:03.2 / 0:10.3", else "0:03.2".
    bool showTime = true;
    const auto totalWidth = [&] {
        gap = std::round((spacious ? 12.0f : 8.0f) * dpiScale_);
        buttonGap = std::round((spacious ? 4.0f : 2.0f) * dpiScale_);
        return buttonsWidth + buttonGap * 4.0f + (showTime ? gap + (showLength ? timeWidth : positionWidth) : 0.0f) +
               (showLabel ? textGap + labelWidth : textGap) + fieldGap + fieldWidth +
               (showOf ? fieldGap + ofWidth : 0.0f);
    };
    for (bool* drop : {&showLabel, &showOf, &spacious, &showLength, &showTime}) {
        if (totalWidth() <= room) {
            break;
        }
        *drop = false;
    }
    const float total = totalWidth(); // Also sets the gaps.

    const float buttonY = pos.y + std::round((height - BarButton::kHeight * dpiScale_) * 0.5f);
    const float textY = pos.y + std::round((height - fontSize) * 0.5f);
    float x = std::round(std::max(std::min(pos.x + (viewport->Size.x - total) * 0.5f, right - total), leftmost));
    const auto button = [&](BarButton& drawn, const char* id, const char* tooltip) {
        ImGui::SetCursorScreenPos(ImVec2(x, buttonY));
        const bool clicked = drawn.draw(id, fonts_);
        if (ImGui::IsItemHovered() && drawn.enabled && tooltip != nullptr) {
            ImGui::SetTooltip("%s", tooltip);
        }
        x += drawn.width(fonts_) + buttonGap;
        return clicked;
    };
    if (button(first, "##first-frame", "The first frame (Home)")) {
        clipView_->toFirst();
    }
    if (button(back, "##frame-back", "One frame back (Left arrow)")) {
        clipView_->step(-1);
    }
    if (button(play, "##play", nullptr)) {
        clipView_->togglePlay();
    }
    if (ImGui::IsItemHovered() && play.enabled) {
        ImGui::SetTooltip("%s", clipView_->playing() ? "Pause (Space)"
                                : clipView_->ended() ? "Play again from the start (Space)"
                                                     : "Play (Space)");
    }
    if (button(on, "##frame-on", "One frame on (Right arrow)")) {
        clipView_->step(1);
    }
    if (button(last, "##last-frame", "The last frame (End)")) {
        clipView_->toLast();
    }
    x -= buttonGap;
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    if (showTime) {
        x += gap;
        drawList->AddText(fonts_.medium, fontSize, ImVec2(x, textY), canPlay ? theme::kText : theme::kTextMuted,
                          showLength ? time.c_str() : position);
        x += showLength ? timeWidth : positionWidth;
    }
    if (showLabel) {
        x += textGap;
        drawList->AddText(ImVec2(x, textY), theme::kTextMuted, "Frame");
        x += labelWidth;
    } else {
        x += textGap;
    }
    x += fieldGap;

    // The frame shown; typed over, Enter goes to the frame typed (clamped to the clip). While it is being typed in,
    // the clip's keys (Space, the arrows, 1 2 3) go to the field instead.
    ImGui::SetCursorScreenPos(ImVec2(x, pos.y + std::round((height - ImGui::GetFrameHeight()) * 0.5f)));
    ImGui::SetNextItemWidth(fieldWidth);
    if (canPlay && ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_G)) {
        ImGui::SetKeyboardFocusHere();
    }
    ImGui::BeginDisabled(!canPlay);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, fieldPadding);
    std::uint64_t typed = clipView_->frame();
    if (ImGui::InputScalar("##frame", ImGuiDataType_U64, &typed, nullptr, nullptr, "%llu",
                           ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll |
                               ImGuiInputTextFlags_CharsDecimal)) {
        clipView_->goTo(typed);
    }
    ImGui::PopStyleVar();
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered() && canPlay && !ImGui::IsItemActive()) {
        ImGui::SetTooltip("Frame %s of %s: type another and press Enter to go there (Ctrl+G)",
                          formatCount(clipView_->frame()).c_str(), formatCount(frames).c_str());
    }
    if (showOf) {
        x += fieldWidth + fieldGap;
        drawList->AddText(ImVec2(x, textY), theme::kTextMuted, of.c_str());
    }
}

void App::drawReviewChooser() {
    // The Review button first asks what to review (the user asked, 2026-11-10): the training recordings, to answer
    // what the model found in them, or the clips (Save last 10 s), to play them. Then the list of either.
    if (!ImGui::BeginPopup(kReviewPopup)) {
        return;
    }
    const float dpi = dpiScale_;
    ImGui::PushFont(fonts_.medium, 18.0f);
    ImGui::TextUnformatted(kReviewPopup);
    ImGui::PopFont();
    ImGui::Dummy(ImVec2(0.0f, 4.0f * dpi));

    const ImVec2 tileSize(std::round(440.0f * dpi), std::round(60.0f * dpi));
    char description[160];
    std::snprintf(description, sizeof(description), "%s %s  \xC2\xB7  answer what the model found, to train it",
                  formatCount(recordings_.size()).c_str(), recordings_.size() == 1 ? "recording" : "recordings");
    if (choiceTile("recordings", "Training recordings", description, tileSize, fonts_, dpi)) {
        openList_ = kRecordingsPopup;
        ImGui::CloseCurrentPopup();
    }
    std::snprintf(
        description, sizeof(description), "%s %s  \xC2\xB7  the last %.0f s, saved with Save last %.0f s, to play",
        formatCount(clips_.size()).c_str(), clips_.size() == 1 ? "clip" : "clips", kClipSeconds, kClipSeconds);
    if (choiceTile("clips", "Clips", description, tileSize, fonts_, dpi)) {
        openList_ = kClipsPopup;
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

void App::drawClips() {
    // The clips in the clips folder, newest first, each with its picture (the screen when it was saved), like the
    // recordings. Picking one plays it.
    if (!ImGui::BeginPopup(kClipsPopup)) {
        return;
    }
    const float dpi = dpiScale_;
    const ImVec2 tileSize(std::round(520.0f * dpi), std::round(88.0f * dpi));
    ImGui::PushFont(fonts_.medium, 18.0f);
    ImGui::TextUnformatted("Play a clip");
    ImGui::PopFont();
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + tileSize.x + ImGui::GetStyle().ScrollbarSize);
    ImGui::TextDisabled("%s", toUtf8((settings_.recordingsFolder / kClipsFolder).c_str()).c_str());
    ImGui::PopTextWrapPos();
    ImGui::Dummy(ImVec2(0.0f, 4.0f * dpi));

    if (clips_.empty()) {
        ImGui::TextUnformatted("No clips yet.");
        ImGui::TextDisabled("Observe a screen and press Save last %.0f s.", kClipSeconds);
        ImGui::EndPopup();
        return;
    }
    const float spacing = ImGui::GetStyle().ItemSpacing.y;
    const float listHeight = std::min(static_cast<float>(clips_.size()) * (tileSize.y + spacing) - spacing,
                                      std::round(ImGui::GetMainViewport()->Size.y * 0.6f));
    ImGui::BeginChild("##clips", ImVec2(tileSize.x + ImGui::GetStyle().ScrollbarSize, listHeight));
    bool askToRemove = false;
    for (std::size_t i = 0; i < clips_.size(); ++i) {
        const ClipRow& row = clips_[i];
        // "10.3 s · 1,241 frames · 2,380 looks", and under it "81.2 MB · Odyssey G61SH" (its size: the user asked,
        // 2026-11-11, once clips got a key frame every 0.1 s and 80-115 MB a game clip; on one line it pushed the
        // screen off the tile).
        const ClipInfo& info = row.entry.info;
        char seconds[24];
        std::snprintf(seconds, sizeof(seconds), "%.1f s", info.seconds);
        const std::string detail = std::string(seconds) + "  \xC2\xB7  " + formatCount(info.frames) +
                                   (info.frames == 1 ? " frame" : " frames") + "  \xC2\xB7  " +
                                   formatCount(info.looks) + (info.looks == 1 ? " look" : " looks");
        std::string where;
        if (row.bytes > 0) {
            char size[24];
            formatBytes(size, sizeof(size), row.bytes);
            where = size;
        }
        if (!info.screen.empty()) {
            where += (where.empty() ? "" : "  \xC2\xB7  ") + info.screen;
        }
        const ImVec2 rowMin = ImGui::GetCursorScreenPos();
        ImGui::SetNextItemAllowOverlap(); // So the X over it gets its clicks.
        if (pictureRow(static_cast<int>(i), tileSize, *thumbnails_, row.entry.folder / kClipPicture, row.when,
                       {detail, where}, fonts_, dpi)) {
            clipToOpen_ = row.entry;
            ImGui::CloseCurrentPopup();
        }
        const ImVec2 next = ImGui::GetCursorScreenPos();
        // An X at its top right (the user asked, 2026-11-11: for a clip saved by mistake): to the Recycle Bin, once
        // confirmed (asked for the same day).
        if (removeButton(static_cast<int>(i), ImVec2(rowMin.x + tileSize.x, rowMin.y), dpi)) {
            clipToConfirm_ = row;
            askToRemove = true;
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Remove this clip (to the Recycle Bin)");
        }
        if (i + 1 < clips_.size()) { // Back under the row for the next; after the last, the list ends.
            ImGui::SetCursorScreenPos(next);
        }
    }
    ImGui::EndChild();
    if (askToRemove) { // Opened over the list, which stays open behind it.
        ImGui::OpenPopup(kRemoveClipPopup);
    }
    drawRemoveClip();
    ImGui::EndPopup();
}

void App::drawRemoveClip() {
    // Which clip, and where it goes. Cancel is the default, so Enter or Space alone never removes one.
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(
        ImVec2(viewport->Pos.x + viewport->Size.x * 0.5f, viewport->Pos.y + viewport->Size.y * 0.4f), ImGuiCond_Always,
        ImVec2(0.5f, 0.5f));
    if (!ImGui::BeginPopupModal(kRemoveClipPopup, nullptr,
                                ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_AlwaysAutoResize |
                                    ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings)) {
        return;
    }
    if (!clipToConfirm_) {
        ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
        return;
    }
    const float dpi = dpiScale_;
    const ClipInfo& info = clipToConfirm_->entry.info;
    ImGui::PushFont(fonts_.medium, 18.0f);
    ImGui::TextUnformatted(kRemoveClipPopup);
    ImGui::PopFont();
    char size[24];
    formatBytes(size, sizeof(size), clipToConfirm_->bytes);
    char about[240];
    std::snprintf(about, sizeof(about), "Saved %s  \xC2\xB7  %.1f s  \xC2\xB7  %s %s  \xC2\xB7  %s",
                  clipToConfirm_->when.c_str(), info.seconds, formatCount(info.frames).c_str(),
                  info.frames == 1 ? "frame" : "frames", size);
    ImGui::TextDisabled("%s", about);
    ImGui::Dummy(ImVec2(0.0f, 4.0f * dpi));
    ImGui::TextUnformatted("It goes to the Recycle Bin, where you can restore it.");
    ImGui::Dummy(ImVec2(0.0f, 8.0f * dpi));

    ImGui::PushStyleColor(ImGuiCol_Button, theme::kRecord);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, IM_COL32(250, 112, 116, 255));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, IM_COL32(220, 70, 75, 255));
    const bool remove = ImGui::Button("Remove");
    ImGui::PopStyleColor(3);
    ImGui::SameLine();
    const bool cancel = ImGui::Button("Cancel");
    ImGui::SetItemDefaultFocus();
    if (remove) {
        clipToRemove_ = clipToConfirm_->entry.folder;
    }
    if (remove || cancel || ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
        clipToConfirm_.reset();
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

void App::drawRecordings() {
    // The Review button's list: every recording in the recordings folder, newest first, each with a picture of its
    // first image (the user asked, 2026-11-02). Picking one asks what to look for in it next.
    if (!ImGui::BeginPopup(kRecordingsPopup)) {
        return;
    }
    const float dpi = dpiScale_;
    const ImVec2 tileSize(std::round(520.0f * dpi), std::round(88.0f * dpi));
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
        ImGui::TextDisabled("Observe a screen and press Record.");
        ImGui::EndPopup();
        return;
    }
    const float spacing = ImGui::GetStyle().ItemSpacing.y;
    const float listHeight = std::min(static_cast<float>(recordings_.size()) * (tileSize.y + spacing) - spacing,
                                      std::round(ImGui::GetMainViewport()->Size.y * 0.6f));
    ImGui::BeginChild("##recordings", ImVec2(tileSize.x + ImGui::GetStyle().ScrollbarSize, listHeight));
    for (std::size_t i = 0; i < recordings_.size(); ++i) {
        const RecordingRow& row = recordings_[i];
        // Each row: a picture of its first image, when, then the details, then when the user last changed answers
        // in a review of it, if ever (the user asked, 2026-11-07). The details: how long, how many images, and how
        // far each category's review got: "00:34 · 12 images · Characters 11 of 12 reviewed".
        std::string detail;
        if (row.entry.info.seconds > 0) {
            char duration[16];
            formatDuration(duration, sizeof(duration), std::chrono::seconds(row.entry.info.seconds));
            detail = std::string(duration) + "  \xC2\xB7  ";
        }
        detail += std::to_string(row.images) + (row.images == 1 ? " image" : " images");
        if (row.bytes > 0) { // All its images together (the user asked, 2026-11-11).
            char size[24];
            formatBytes(size, sizeof(size), row.bytes);
            detail += std::string("  \xC2\xB7  ") + size;
        }
        if (row.entry.info.kind == RecordKind::Unsure) {
            detail += "  \xC2\xB7  only unsure";
        }
        bool reviewed = false;
        for (std::size_t c = 0; c < row.reviewed.size(); ++c) {
            const ReviewCount& count = row.reviewed[c];
            // Images answered before kinds, or holding a guess added since: "Characters: 30 to check", or
            // "Characters: 1 to check, 109 of 110 reviewed" once some are checked (what waits first: a long line
            // is cut at the tile's edge).
            char text[160] = {};
            if (count.reviewed > 0 && count.toCheck > 0) {
                std::snprintf(text, sizeof(text), "  \xC2\xB7  %s: %zu to check, %zu of %zu reviewed",
                              kCategories[c].name, count.toCheck, count.reviewed, row.images);
            } else if (count.toCheck > 0) {
                std::snprintf(text, sizeof(text), "  \xC2\xB7  %s: %zu to check", kCategories[c].name, count.toCheck);
            } else if (count.reviewed > 0) {
                std::snprintf(text, sizeof(text), "  \xC2\xB7  %s %zu of %zu reviewed", kCategories[c].name,
                              count.reviewed, row.images);
            }
            if (text[0] != '\0') {
                detail += text;
                reviewed = true;
            }
        }
        if (!reviewed) {
            detail += "  \xC2\xB7  not reviewed yet";
        }
        if (pictureRow(static_cast<int>(i), tileSize, *thumbnails_, row.firstImage, row.when,
                       {detail, row.lastReviewed}, fonts_, dpi)) {
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
    for (const Category& category : kCategories) {
        if (choiceTile(category.id, category.name, category.description, tileSize, fonts_, dpi)) {
            ImGui::CloseCurrentPopup();
            startReview(category);
        }
    }
    ImGui::Dummy(ImVec2(0.0f, 2.0f * dpi));
    ImGui::TextDisabled("More to look for later: terrain, the game's UI.");
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
    // The game's interface under the characters: they are in the world it covers.
    if (settings_.showUi && uiFinder_ && uiFinder_->state() == UiFinder::State::Running) {
        if (const UiPieces& pieces = uiFinder_->found(); pieces.image > 0) {
            drawUiPieces(drawList, fonts_.medium, min, max, static_cast<float>(pipeline_.width()),
                         {pieces.pieces.data(), static_cast<std::size_t>(pieces.count)}, dpiScale_);
        }
    }
    // The ground under them too, as the camera is on the image shown (it follows fewer images: moved on to it).
    if (groundFollowing_ && groundFinder_->state() == GroundFinder::State::Running) {
        if (const GroundSeen& seen = groundFinder_->seen(); seen.image > 0) {
            groundDrawn_ = {true, previewImage_, seen.followed, groundAt(seen.camera, previewMs_)};
            drawGround(drawList, min, max, static_cast<float>(pipeline_.width()), groundDrawn_.camera, dpiScale_);
        }
    }
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
        char detail[512] = {};
        std::size_t length = 0;
        const auto add = [&](const char* format, auto... values) {
            if (length < sizeof(detail)) {
                const int added = std::snprintf(detail + length, sizeof(detail) - length, format, values...);
                length = added > 0 ? std::min(length + static_cast<std::size_t>(added), sizeof(detail)) : length;
            }
        };
        if (recognizer_ && recognizer_->state() == Recognizer::State::Running) {
            const RecognizerStats& found = recognizer_->stats();
            add("%s: %.0f images/s  \xC2\xB7  %.2f ms each  \xC2\xB7  %.2f ms slowest", kLive.name,
                static_cast<double>(found.imagesPerSecond), static_cast<double>(found.workAvgMs),
                static_cast<double>(found.workMaxMs));
            if (snapper_ && snapper_->state() == BoxSnapper::State::Running) {
                const SnapperStats& snapped = snapper_->stats();
                add("  \xC2\xB7  snapped %.0f/s, %.2f ms each, %.2f slowest",
                    static_cast<double>(snapped.imagesPerSecond), static_cast<double>(snapped.workAvgMs),
                    static_cast<double>(snapped.workMaxMs));
            }
        }
        if (uiFinder_ && uiFinder_->state() == UiFinder::State::Running) {
            const UiFinderStats& ui = uiFinder_->stats();
            add("%sUI found %.0f/s, %.2f ms each (finding %.2f), %.2f slowest", length > 0 ? "  \xC2\xB7  " : "",
                static_cast<double>(ui.imagesPerSecond), static_cast<double>(ui.workAvgMs),
                static_cast<double>(ui.findAvgMs), static_cast<double>(ui.workMaxMs));
        }
        if (groundFollowing_ && groundFinder_->state() == GroundFinder::State::Running) {
            const GroundFinderStats& ground = groundFinder_->stats();
            const GroundCamera& camera = groundFinder_->seen().camera;
            add("%sGround %.0f/s, %.2f ms each (fitting %.2f), fitted %.0f%% (searched %.0f%%), %.0f of %.0f patches "
                "agree (%.1f px off), pitch %.0f\xC2\xB0 distance %.1f, minimap %.0f%%, turned by it %.0f/s (%.2f ms)",
                length > 0 ? "  \xC2\xB7  " : "", static_cast<double>(ground.imagesPerSecond),
                static_cast<double>(ground.workAvgMs), static_cast<double>(ground.fitAvgMs),
                static_cast<double>(ground.fitted * 100.0f), static_cast<double>(ground.searched * 100.0f),
                static_cast<double>(ground.agreeing),
                static_cast<double>(ground.matches), static_cast<double>(ground.missPixels),
                static_cast<double>(camera.pitch * 57.29578f), static_cast<double>(camera.distance),
                static_cast<double>(ground.headed * 100.0f), static_cast<double>(ground.headsPerSecond),
                static_cast<double>(ground.headAvgMs));
        }
        theme::drawLabel(drawList, ImVec2(min.x + inset, max.y - inset), ImVec2(0.0f, 1.0f), text,
                         detail[0] != '\0' ? detail : nullptr);
    }
}

void App::drawFound(ImDrawList* drawList, ImVec2 min, ImVec2 max) {
    // The snapper's boxes while it runs (on the newest image), else the recogniser's.
    const Found* found = &recognizer_->found();
    if (snapper_ && snapper_->state() == BoxSnapper::State::Running) {
        if (const Found& snapped = snapper_->found(); snapped.image > 0) {
            found = &snapped;
        }
    }
    drawThings(drawList, fonts_.medium, min, max, static_cast<float>(pipeline_.width()),
               {found->things.data(), static_cast<std::size_t>(found->count)}, recognizer_->kinds(),
               settings_.showKinds, dpiScale_);
}

void App::hintGround() {
    // What is shown now (as drawFound draws it): the own character's box gives the camera's distance, and the
    // characters and the interface are left out of the ground's motion.
    std::span<const Tracked> things;
    int ownKind = -1;
    if (recognizer_ && recognizer_->state() == Recognizer::State::Running) {
        const Found* found = &recognizer_->found();
        if (snapper_ && snapper_->state() == BoxSnapper::State::Running) {
            if (const Found& snapped = snapper_->found(); snapped.image > 0) {
                found = &snapped;
            }
        }
        things = {found->things.data(), static_cast<std::size_t>(found->count)};
        const std::vector<std::string>& kinds = recognizer_->kinds();
        const auto own = std::find(kinds.begin(), kinds.end(), "own");
        ownKind = own != kinds.end() ? static_cast<int>(own - kinds.begin()) : -1;
    }
    const UiPieces* pieces = nullptr;
    if (uiFinder_ && uiFinder_->state() == UiFinder::State::Running) {
        pieces = &uiFinder_->found();
    }
    groundFinder_->hint(things, ownKind, pieces);
}

} // namespace observex
