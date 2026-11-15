#include "app/App.h"

#include "app/DebugView.h"
#include "app/Overlay.h"
#include "app/Snapshot.h"
#include "app/StatsPanel.h"
#include "app/Widgets.h"
#include "engine/CrashLog.h"
#include "heuristics/KeyLabels.h"

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
#include <cstring>
#include <exception>
#include <fstream>
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
constexpr char kHandmadePopup[] = "handmade";
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
constexpr float kTileGap = 6.0f; // Between the tiles, before DPI scaling: their line, and room to grab it.

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

std::int64_t qpcNow() {
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return now.QuadPart;
}

double qpcFrequency() {
    LARGE_INTEGER frequency;
    QueryPerformanceFrequency(&frequency);
    return static_cast<double>(frequency.QuadPart);
}

// The local time it was at `qpc` (a QueryPerformanceCounter time, now or a moment before), as the log writes it:
// "2026-11-13 00:06:01.234".
std::string logTimeAt(std::int64_t qpc) {
    FILETIME utc;
    GetSystemTimePreciseAsFileTime(&utc);
    ULARGE_INTEGER ticks; // 100 ns ticks.
    ticks.LowPart = utc.dwLowDateTime;
    ticks.HighPart = utc.dwHighDateTime;
    const double back = std::max(0.0, static_cast<double>(qpcNow() - qpc) / qpcFrequency());
    ticks.QuadPart -= static_cast<ULONGLONG>(back * 1e7);
    utc.dwLowDateTime = ticks.LowPart;
    utc.dwHighDateTime = ticks.HighPart;
    SYSTEMTIME universal;
    SYSTEMTIME local;
    if (!FileTimeToSystemTime(&utc, &universal) || !SystemTimeToTzSpecificLocalTime(nullptr, &universal, &local)) {
        GetLocalTime(&local);
    }
    return logTime(local.wYear, local.wMonth, local.wDay, local.wHour, local.wMinute, local.wSecond,
                   local.wMilliseconds);
}

// "recording done" as the start of a log line: "Recording done".
std::string capitalized(std::string text) {
    if (!text.empty()) {
        text[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(text[0])));
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
        // The app's own frame, step by step (the Debug tab), summed up once a second; then the clip buffer has every
        // part's numbers (a clip says what to optimize).
        if (const auto now = std::chrono::steady_clock::now(); now - frameCostsSince_ >= std::chrono::seconds(1)) {
            frameCosts_ = frameMeter_.take(std::chrono::duration<double>(now - frameCostsSince_).count());
            frameCostsSince_ = now;
            noteCostsInClip();
        }
        frameMeter_.begin();
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
                addLog(clipSaveError_);
            }
        }
        pollShots(); // Shift+Win works while the window is minimized or behind the game too.

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
            // Then the GPU is free for the other reviewed recordings' guesses, until the review closes (for the kind
            // update: only a category with kinds).
            if (!review_->finding() && !refresher_ && !review_->category().kinds.empty()) {
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
        } else if (chosenNone_) {
            stopObservingAll();
            chosenNone_ = false;
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
                clipView_->setHeuristics(*todaysHeuristics(), todaysSnapping(), todaysGround(), todaysDepth(), about);
            }
        }
        // observex_stats.dll rebuilt (the user asked, 2026-11-12: to compare new reading rules with what was
        // recorded): the UI finder reads with it from its next image, and an open clip's frame is read again.
        if (stats_->poll()) {
            const std::string about = stats_->describe();
            noteInClip("stats reading loaded again: " + about);
            if (clipView_) {
                clipView_->statsReloaded(about);
            }
        }
        // The profile open (also while the State tile is not shown), and its skills to the readers when they changed.
        stateTree_.ensureProfile(settings_.brainsFolder);
        frameMeter_.lap(FrameStep::Housekeeping);
        followSkills();
        syncSkills();
        frameMeter_.lap(FrameStep::Skills);
        keepStats(); // Its laps: Stats, Geodata (only on a new reading).
        watchLoot();
        frameMeter_.lap(FrameStep::Loot);
        tickBrain();
        frameMeter_.lap(FrameStep::Brain);
        const bool away = review_ || clipView_;
        // The drop finder always (the user asked, 2026-11-15: drops lie where the camera turned away from, or where
        // the character runs by while seeking monsters), the Show flag Drops on or off.
        pipeline_.setDropsEnabled(dropFinder_ && dropFinder_->state() != DropFinder::State::Failed && !away);
        pipeline_.setRecognitionEnabled(recognizer_ && recognizer_->state() != Recognizer::State::Failed && !away);
        pipeline_.setSnapEnabled(snapper_ && snapper_->state() != BoxSnapper::State::Failed && !away);
        // The UI finder goes on with it, the Show flag UI on or off (the user asked, 2026-11-12: the flag only draws
        // what it found).
        pipeline_.setUiEnabled(uiFinder_ && uiFinder_->state() != UiFinder::State::Failed && !away);
        // The ground finder only while something needs its camera (Ground draws its grid, Geodata lays the
        // geodata's grid with it, the Stats tab's Camera card: the mouse's counts are anchored by what it follows;
        // the user asked, 2026-11-15, after its Pitch stayed "Not read yet" with both flags off), afresh each time it
        // starts again. The grid is drawn only with Ground on; the depth finder still waits for its flags.
        const bool cameraShown = settings_.showGameStats && !settings_.showSkills;
        const bool following = groundFinder_ && groundFinder_->state() != GroundFinder::State::Failed && !away &&
                               (settings_.showGround || settings_.showGeodata || cameraShown);
        if (following && !groundFollowing_) {
            groundFinder_->restart();
        }
        groundFollowing_ = following;
        pipeline_.setGroundEnabled(following);
        if (following) {
            hintGround();
        }
        // The depth finder only while a flag needs it (the user asked, 2026-11-12 late: the grid and the depth work
        // only with their flags on): Depth draws its maps (shaded only then), Ground tilts the grid's pitch by them.
        // Made the first time one is on while observing; with both off it is handed no images (the network stays
        // loaded). It leaves out the interface the UI finder found.
        const bool depthNeeded = settings_.showDepth || settings_.showGround || settings_.showGeodata;
        if (depthNeeded && !away && observed_ && !depthFinder_ && depthFinderError_.empty() &&
            pipeline_.depth() != nullptr) {
            startDepth();
        }
        const bool depthRunning =
            depthNeeded && depthFinder_ && depthFinder_->state() != DepthFinder::State::Failed && !away;
        depthRunning_ = depthRunning;
        depthShowing_ = depthRunning && settings_.showDepth;
        pipeline_.setDepthEnabled(depthRunning);
        if (depthRunning) {
            depthFinder_->show(depthShowing_);
            depthFinder_->hint(uiFinder_ && uiFinder_->state() == UiFinder::State::Running ? &uiFinder_->found()
                                                                                          : nullptr);
        }
        // The clip buffer pauses with it (its encoding took 21% of the video encoder): a clip is for what the
        // recogniser saw. Meanwhile it repeats the last image, so a clip saved right after still plays (still, with no
        // looks).
        pipeline_.setClipEnabled(clipRecorder_ && clipRecorder_->state() != ClipRecorder::State::Failed && !away);
        frameMeter_.lap(FrameStep::Steering);
        if (!window_->prepareFrame()) {
            pipeline_.setPreviewEnabled(false); // Minimized: nobody sees it.
            groundDrawn_.drawn = false;          // Nor the grid,
            noteGroundInClip();
            depthDrawn_ = 0; // nor the depth.
            noteDepthInClip();
            noteBrainInClip();
            noteCameraInClip();
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
            frameMeter_.lap(FrameStep::Preview);
        }
        if (clipView_) {
            clipView_->update();
            frameMeter_.lap(FrameStep::Clip);
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
        groundDrawn_.drawn = false; // Until the live view draws the grid,
        depthDrawn_ = 0;            // and the depth.
        drawFrame(); // Its laps: New frame, Brain acting.
        noteGroundInClip();
        noteDepthInClip();
        noteBrainInClip(); // After the tasks ran (drawFrame's actuateBrain).
        noteCameraInClip();
        frameMeter_.lap(FrameStep::Drawing);
        ImGui::Render();

        window_->bindBackBuffer();
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        frameMeter_.lap(FrameStep::Rendering);
        frameMeter_.end();
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
        settings_.brainsFolder = std::filesystem::path(snapshot_->path).parent_path() / L"brains";
    } else {
        settingsFile_ = appFolder() / L"observex.cfg";
        settings_ = Settings::load(settingsFile_);
        if (!settings_.brainName.empty()) {
            stateTree_.load(settings_.brainsFolder, settings_.brainName); // The one last saved or loaded.
        }
    }
    // The log: what earlier runs logged comes back.
    const std::filesystem::path logFolder =
        snapshot_ ? std::filesystem::path(snapshot_->path).parent_path() : appFolder();
    log_ = std::make_unique<LogBook>(logFolder / L"observex.log");
    // A crash says where it was in the log (with the call chain) and leaves a dump in crashes/ (the user asked,
    // 2026-11-15, after a crash in TensorRT that Windows' own report kept from us). Again once observing, as CUDA and
    // TensorRT may put their own handler in.
    crashLogFile_ = logFolder / L"observex.log";
    crashFolder_ = logFolder / L"crashes";
    installCrashLog(crashLogFile_, crashFolder_);
    addLog("observex started");
    // Where the character is and the geodata round it (the flag Geodata): models/geodata has the server's .l2j files.
    geodata_ = std::make_unique<GeodataView>(modelFolder("geodata"));
    if (settings_.minimapScale > 0.0f) {
        geodata_->setScale(settings_.minimapScale);
    }

    // observex_heuristics.dll next to the exe (the Release build puts both in the project folder), and
    // observex_stats.dll likewise.
    heuristics_ = std::make_shared<HeuristicsSource>(appFolder() / L"observex_heuristics.dll");
    stats_ = std::make_shared<StatsSource>(appFolder() / L"observex_stats.dll");

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
    stopActing();         // The brain no longer drives the controller (release anything held).
    mouseTest_.stop();    // TEMPORARY (the Mouse debug sweep, 2026-11-15): no more cursor moves.
    stateTree_.flush();   // A State tile field still being typed in is saved into its profile.
    refresher_.reset();   // Its finder writes nothing.
    review_.reset();      // Closing observex while reviewing makes no kind update.
    clipView_.reset();    // Its image and reader are on the UI's device, which goes below.
    kindUpdater_.reset(); // One still at work writes nothing.
    stopRecording();
    stopObserving();
    if (log_) { // shutdown() runs twice (the destructor too): once logged.
        addLog("observex closed");
        log_.reset();
    }
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
    // Win+Esc starts or stops the Brain while a screen is observed, whatever is in front (the user asked, 2026-11-15:
    // it must work while the game is focused; the game runs as administrator, so a keyboard hook never saw its keys,
    // and plain Escape is the game's). Not in snapshot mode.
    if (!brainKey_ && !snapshot_) {
        try {
            brainKey_ = std::make_unique<HotKey>(MOD_WIN, VK_ESCAPE);
        } catch (const std::exception& e) {
            addLog(std::string("Win+Esc will not start or stop the brain: ") + e.what());
        }
    }
    // Shift+Win, the training screenshot, likewise only while observing (it saves the observed screen).
    if (!shotHook_ && !snapshot_) {
        try {
            shotHook_ = std::make_unique<ShotHook>();
        } catch (const std::exception& e) {
            addLog(std::string("Shift+Win training screenshots will not work: ") + e.what());
        }
        // Its hook hears nothing typed into a game that runs as administrator unless observex is one (app/ShotHook);
        // Win+Esc, a registered hot key, is heard either way.
        if (!runsAsAdministrator()) {
            addLog("Not running as administrator: Shift+Win is not heard while Lineage 2 is in front "
                   "(start observex again and say Yes when Windows asks)");
        }
    }
    // Before the loop starts, so its very first image reaches the UI and the recogniser.
    pipeline_.setPreviewEnabled(true);
    pipeline_.setRecognitionEnabled(true);
    if (!pipeline_.start(screen)) {
        addLog("Could not observe " + screen.monitor + ": " + pipeline_.error());
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
        brainNoted_ = false;  // Its first brain line on the next frame,
        dropsNoted_ = false;  // drops line
        cameraNoted_ = false; // and camera line.
    } catch (const std::exception& e) {
        clipError_ = e.what();
    }
    // The camera by the mouse's counts: the game fills the screen observed, so it puts the cursor in its middle when
    // it starts turning.
    mouseCamera_ = std::make_shared<SharedMouseCamera>();
    mouseCamera_->middle(static_cast<int>((screen.bounds.left + screen.bounds.right) / 2),
                         static_cast<int>((screen.bounds.top + screen.bounds.bottom) / 2));
    try {
        mouseWatch_ = std::make_unique<MouseWatch>(
            [clip = clipRecorder_, camera = mouseCamera_](const MouseInput& input) {
                if (clip) {
                    clip->moused(input);
                }
                camera->add({groundMs(input.qpc), input.dx, input.dy, input.held, input.buttons, input.wheel,
                             input.cursorX, input.cursorY});
            });
    } catch (const std::exception& e) {
        addLog(std::string("Could not watch the mouse: ") + e.what());
    }
    addLog("Observing " + screen.monitor);

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
        uiFinder_ = std::make_unique<UiFinder>(screen.adapter.Get(), *pipeline_.ui(), modelFolder("ui"), stats_);
        uiFinder_->setSkills(skillsToFind()); // The profile's.
        uiFinder_->setLook(lookAsked_);       // And the monster Select Target looks at, if any.
    } catch (const std::exception& e) {
        uiFinderError_ = e.what();
    }

    // The drop finder: drops anywhere on the screen, looked at closely round a kill or drops found (the user asked,
    // 2026-11-15), once there is a drop model.
    dropFinderError_.clear();
    lootWatch_.reset();
    loot_ = {};
    if (std::error_code missing; std::filesystem::exists(modelFolder("drop") / L"model.onnx", missing)) {
        try {
            dropFinder_ = std::make_unique<DropFinder>(screen.adapter.Get(), *pipeline_.drops(), modelFolder("drop"));
        } catch (const std::exception& e) {
            dropFinderError_ = e.what();
        }
    } else {
        dropFinderError_ = "No drop model yet: review Drops on your Shift+Win screenshots, then train one.";
    }

    // The ground finder: the game's camera, for the Show flag Ground (the user asked, 2026-11-12).
    groundFinderError_.clear();
    groundFollowing_ = false;
    depthFinderError_.clear(); // The depth finder is made when the Depth or Ground flag first needs it (startDepth).
    try {
        groundFinder_ =
            std::make_unique<GroundFinder>(screen.adapter.Get(), *pipeline_.ground(), heuristics_, mouseCamera_);
    } catch (const std::exception& e) {
        groundFinderError_ = e.what();
    }
    installCrashLog(crashLogFile_, crashFolder_); // Ours again after the models' libraries loaded.
}

void App::stopObserving() {
    stopActing();      // No screen to act on.
    mouseTest_.stop(); // TEMPORARY (the Mouse debug sweep, 2026-11-15): there are no monsters to sweep now.
    brainKey_.reset(); // Win+Esc only lives during an observing session.
    if (observed_ && pipeline_.state() != Pipeline::State::Stopped) {
        addLog("Stopped observing " + observed_->monitor);
    }
    xpWatch_ = {}; // Another screen or character next: its XP only starts it.
    liveStats_ = {};    // And nothing is read.
    targetWatch_ = {};  // Nor is a target watched,
    watchLook_ = {};    // nor its plate looked at.
    sendLook();
    pauseRecording();    // The recorder reads from the pipeline's channel; it goes first.
    shotHook_.reset();
    logSavedShots();       // The last screenshots saved still get their Log lines.
    shotRecorder_.reset(); // Reads from the pipeline's shots channel; a screenshot being saved is written first.
    shotPackage_.reset();
    recognizer_.reset(); // So does the recogniser (it hands looks to the clip buffer and the snapper, so before them).
    pipeline_.setSnapEnabled(false);
    snapper_.reset(); // Reads from the pipeline's snap channel.
    pipeline_.setUiEnabled(false);
    uiFinder_.reset(); // Reads from its ui channel.
    pipeline_.setDropsEnabled(false);
    dropFinder_.reset(); // Reads from its drops channel.
    lootWatch_.reset();  // No kill on another screen.
    loot_ = {};
    deadNow_ = 0;
    statsHistory_.clear(); // A new pipeline numbers its images from 1 again.
    pipeline_.setGroundEnabled(false);
    groundFinder_.reset(); // Reads from its ground channel.
    groundFollowing_ = false;
    pipeline_.setDepthEnabled(false);
    depthFinder_.reset(); // Reads from its depth channel, and hands maps to the clip buffer: before it.
    depthRunning_ = false;
    depthShowing_ = false;
    depthUploaded_ = 0;
    pipeline_.setClipEnabled(false);
    mouseWatch_.reset(); // Hands the clip buffer the mouse: before it.
    mouseCamera_.reset();
    clipRecorder_.reset(); // Reads from the pipeline's clip channel; a save still being written finishes first.
    clipSaving_ = false;
    preview_.reset(); // Before the pipeline frees the channel it reads from.
    observedImage_.reset();
    pipeline_.setRecognitionEnabled(false);
    pipeline_.stop();
}

void App::startDepth() {
    depthFinderError_.clear();
    try {
        DepthFinder::Record record; // Every map into the clip buffer too, so a clip replays the depth seen.
        if (clipRecorder_) {
            record = [clip = clipRecorder_](std::shared_ptr<const DepthRecord> map) { clip->depthed(std::move(map)); };
        }
        depthFinder_ = std::make_unique<DepthFinder>(observed_->adapter.Get(), *pipeline_.depth(), heuristics_,
                                                     modelFolder("depth"), std::move(record));
    } catch (const std::exception& e) {
        depthFinderError_ = e.what();
    }
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
        const std::string what =
            (kind == RecordKind::Unsure ? "only unsure" : "every " + std::to_string(kRecordingInterval) + " s") +
            ", into " + nameOf(recording_->folder);
        noteInClip("recording started: " + what);
        addLog("Recording started: " + what);
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
        addLog(capitalized(why) + ": " + std::to_string(info.images) + (info.images == 1 ? " image" : " images") +
               " in " + nameOf(recording_->folder));
    }
    recording_.reset();
}

void App::takeShot() {
    if (!observed_ || pipeline_.state() != Pipeline::State::Running || pipeline_.shots() == nullptr) {
        addLog("Training screenshot not taken: no screen is observed");
        return;
    }
    // The hour's package: made with its first screenshot, its recorder kept while the hour lasts.
    SYSTEMTIME now;
    GetLocalTime(&now);
    const std::filesystem::path folder =
        settings_.recordingsFolder / handmadeFolderName(now.wYear, now.wMonth, now.wDay, now.wHour);
    if (!shotRecorder_ || !shotPackage_ || shotPackage_->folder != folder) {
        logSavedShots(); // The hour before's last ones.
        shotRecorder_.reset();
        shotPackage_.reset();
        RecordingInfo info = readRecordingInfo(folder); // Taken earlier this hour, before a restart: on from there.
        if (info.started.empty()) {
            info.started = handmadeStarted(now.wYear, now.wMonth, now.wDay, now.wHour);
            info.screen = observed_->monitor;
            info.width = pipeline_.width();
            info.height = pipeline_.height();
            info.kind = RecordKind::Handmade;
            std::error_code error;
            std::filesystem::create_directories(folder, error);
            // Without the file the review would not list the package.
            if (!writeRecordingInfo(folder, info)) {
                addLog("Training screenshot not taken: could not write " + toUtf8((folder / kRecordingFile).c_str()));
                return;
            }
        }
        ShotPackage package{folder, handmadeTitle(info.started), nextImageNumber(listImages(folder)), 0};
        try {
            shotRecorder_ = std::make_unique<Recorder>(*observed_, *pipeline_.shots(), folder, package.first, nullptr,
                                                       ImageFormat::Png);
        } catch (const std::exception& e) {
            addLog(std::string("Training screenshot not taken: ") + e.what());
            return;
        }
        shotPackage_ = std::move(package);
    }
    pipeline_.takeShot(); // The next new image; the Log says once it is saved.
}

void App::pollShots() {
    if (shotHook_ && shotHook_->takePressed()) {
        takeShot();
    }
    if (snapshot_ && snapshot_->shot && !snapshotShot_ && pipeline_.state() == Pipeline::State::Running) {
        snapshotShot_ = true;
        takeShot();
    }
    logSavedShots();
}

void App::logSavedShots() {
    if (!shotRecorder_ || !shotPackage_) {
        return;
    }
    for (const unsigned saved = shotRecorder_->saved(); shotPackage_->logged < saved; ++shotPackage_->logged) {
        addLog("Training screenshot " + std::to_string(shotPackage_->first + shotPackage_->logged) +
               " saved in handmade " + shotPackage_->title);
    }
    if (shotRecorder_->failed()) {
        addLog("Training screenshots stopped: " + shotRecorder_->error());
        shotRecorder_.reset(); // The next Shift+Win tries again.
        shotPackage_.reset();
    }
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
    if (mouseWatch_) {
        about.mouse = MouseWatch::pointerSettings();
    }
    noteInClip("save last 10 s");
    // Before the save, so the clip has it too, and the costs up to now.
    addLog("Saved last 10 s as clip " + nameOf(folder));
    noteCostsInClip();
    clipSaveError_.clear();
    const std::filesystem::path statsFolder = folder;
    if (clipRecorder_->save(std::move(folder), std::move(about))) {
        clipSaving_ = true;
        writeClipStats(statsFolder); // The numbers read meanwhile, beside it.
    } else {
        clipSaveError_ = "Could not save the clip: it holds nothing yet.";
        addLog(clipSaveError_);
    }
}

void App::noteInClip(std::string_view text) {
    if (clipRecorder_) {
        clipRecorder_->note(text);
    }
}

void App::addLog(std::string text, std::int64_t qpc) {
    if (!log_) {
        return;
    }
    if (qpc == 0) {
        qpc = qpcNow();
    }
    LogLine line{logTimeAt(qpc), std::move(text)};
    if (clipRecorder_) {
        clipRecorder_->logged(qpc, line);
    }
    log_->add(std::move(line));
}

void App::keepStats() {
    if (!uiFinder_ || uiFinder_->state() != UiFinder::State::Running) {
        return;
    }
    if (const GameStats& newest = uiFinder_->gameStats();
        newest.image == 0 || (!statsHistory_.empty() && statsHistory_.back().image == newest.image)) {
        return;
    } else {
        liveStats_ = newest;
    }
    addLiveNearby(liveStats_);
    const GameStats& stats = liveStats_;
    statsHistory_.push_back(stats);
    // XP got: the difference between readings (the user asked, 2026-11-13), timed when the screen showed it.
    const double ticksPerSecond = qpcFrequency();
    const double shownSeconds = static_cast<double>(stats.shownQpc) / ticksPerSecond;
    if (const auto change = xpWatch_.read(stats.find("Status", "XP"), shownSeconds)) {
        addLog(change->text, static_cast<std::int64_t>(change->seconds * ticksPerSecond));
    }
    watchTarget(stats);
    frameMeter_.lap(FrameStep::Stats);
    // Where the character is (the minimap followed, /loc read from the chat), the Geodata flag on or off.
    if (geodata_) {
        std::vector<std::string> lines;
        geodata_->read(stats, groundMs(stats.shownQpc), lines);
        for (std::string& line : lines) {
            addLog(std::move(line), stats.shownQpc);
        }
        if (const float scale = geodata_->scale(); scale > 0.0f && std::fabs(scale - settings_.minimapScale) > 0.01f) {
            settings_.minimapScale = scale; // Learned (or bettered) from two /loc: kept for the next start.
            saveSettings();
        }
        frameMeter_.lap(FrameStep::Geodata);
    }
    LARGE_INTEGER frequency;
    QueryPerformanceFrequency(&frequency);
    const auto kept = static_cast<std::int64_t>(kStatsKeptSeconds * static_cast<double>(frequency.QuadPart));
    while (statsHistory_.size() > 1 && stats.shownQpc - statsHistory_.front().shownQpc > kept) {
        statsHistory_.pop_front();
    }
}

void App::tickBrain() {
    // The brain only advances while it is active (the user asked, 2026-11-15: an inactive one must not transition on
    // its own). Starting it again starts from Root (toggleActing).
    if (!brainActs_) {
        return;
    }
    const GameStats* stats = nullptr;
    if (clipView_) {
        stats = clipStatsShown(true);
    } else if (!review_ && uiFinder_ && uiFinder_->state() == UiFinder::State::Running && liveStats_.image > 0) {
        stats = &liveStats_;
    }
    if (stats == nullptr) {
        return; // Not reading: freeze, do not reset.
    }
    if (stats->image != brainImage_) { // One step a reading.
        brainImage_ = stats->image;
        stateTree_.tick(statetree::variablesOf(*stats));
    }
}

float App::imageWorkMs(std::string* parts) {
    // Each part's run on one screen image, on average over the last second (each part's own numbers, refreshed once a
    // second), added up.
    const std::vector<CostPart> all = costs();
    if (parts != nullptr) {
        for (const CostPart& part : all) {
            char text[64];
            std::snprintf(text, sizeof(text), "%s%s %.2f", parts->empty() ? "" : "  \xC2\xB7  ", part.run.name.data(),
                          static_cast<double>(part.run.eachMs));
            *parts += text;
        }
    }
    return costTotalMs(all);
}

std::vector<CostPart> App::costs() {
    std::vector<CostPart> parts;
    const auto add = [&parts](const CostPart& part, const char* name) {
        if (part.run.perSecond > 0.0f) { // It took images that second.
            parts.push_back(part);
            nameCost(parts.back().run, name);
        }
    };
    if (pipeline_.state() == Pipeline::State::Running) {
        add(pipeline_.stats().costs, "Capture");
    }
    if (recognizer_ && recognizer_->state() == Recognizer::State::Running) {
        add(recognizer_->stats().costs, kLive.name);
    }
    if (snapper_ && snapper_->state() == BoxSnapper::State::Running) {
        add(snapper_->stats().costs, "Snapping");
    }
    if (uiFinder_ && uiFinder_->state() == UiFinder::State::Running) {
        add(uiFinder_->stats().costs, "UI and stats");
    }
    if (groundFinder_ && groundFollowing_ && groundFinder_->state() == GroundFinder::State::Running) {
        add(groundFinder_->stats().costs, "Ground");
    }
    if (depthFinder_ && depthRunning_ && depthFinder_->state() == DepthFinder::State::Running) {
        add(depthFinder_->stats().costs, "Depth");
    }
    if (dropFinder_ && dropFinder_->state() == DropFinder::State::Running) {
        add(dropFinder_->stats().costs, "Drops");
    }
    if (clipRecorder_ && clipRecorder_->state() == ClipRecorder::State::Running) {
        add(clipRecorder_->stats().costs, "Clip buffer");
    }
    add(frameCosts_, "App frame");
    return parts;
}

void App::noteCostsInClip() {
    // Not while a review or a clip is open: the buffer pauses then.
    if (clipRecorder_ && clipRecorder_->state() == ClipRecorder::State::Running && !review_ && !clipView_) {
        clipRecorder_->costed(qpcNow(), costs());
    }
}

void App::drawDebug() {
    if (clipView_) {
        // As recorded: the look at the costs whose second holds the frame shown.
        const std::span<const ClipCostLine> lines = clipView_->recordedCosts();
        std::vector<CostPart> parts;
        for (const ClipCostLine& line : lines) {
            parts.push_back(line.part);
        }
        char when[96] = "";
        if (!lines.empty()) {
            const double seconds = std::max(0.0, lines.front().timeMs / 1000.0);
            std::snprintf(when, sizeof(when), "as recorded over the second up to %d:%04.1f of the clip",
                          static_cast<int>(seconds / 60.0), std::fmod(seconds, 60.0));
        }
        drawCosts(parts, when, "Not recorded in this clip", fonts_, dpiScale_);
        return;
    }
    drawCosts(costs(), "over the last second", "Nothing measured yet", fonts_, dpiScale_);
}

void App::writeClipStats(const std::filesystem::path& folder) {
    if (statsHistory_.empty()) {
        return;
    }
    LARGE_INTEGER now;
    LARGE_INTEGER frequency;
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&frequency);
    const double msPerTick = 1000.0 / static_cast<double>(frequency.QuadPart);
    std::string text = "# observex " + std::string(kClipStatsFile) +
                       ": the numbers read from the game's interface while this clip was recorded, by screen image "
                       "(core/GameStats)\n";
    for (const GameStats& stats : statsHistory_) {
        // To a tenth of a millisecond ("-911.2", not "-911.1691000000001").
        const double ms = std::round(static_cast<double>(stats.shownQpc - now.QuadPart) * msPerTick * 10.0) / 10.0;
        appendStatsLine(text, stats.image, ms, stats);
    }
    // Its folder may not be made yet (the clip is written in the background).
    std::error_code error;
    std::filesystem::create_directories(folder, error);
    std::ofstream out(folder / kClipStatsFile, std::ios::binary | std::ios::trunc);
    out << text;
    if (!out) {
        clipSaveError_ = "Could not write the clip's " + std::string(kClipStatsFile) + " (its numbers are missing)";
    }
}

void App::stopObservingAll() {
    closeReview();
    closeClip();
    stopRecording(); // A recording is of the screen observed.
    stopObserving();
    observed_.reset();
    error_.clear();
}

void App::drawGameStats() {
    // One column (the user asked, 2026-11-12: no Recorded and Today side by side, no words on which rules read): live,
    // what the UI finder read; on a clip, what the guesser shows (Recorded: clip-stats.txt; Today: read again on the
    // frame shown). Below it only why nothing is read, if so.
    const GameStats* stats = nullptr;
    std::string empty = "Nothing read yet";
    std::string problem;
    if (clipView_) {
        if (clipView_->guesser() == ClipView::Guesser::Recorded) {
            stats = clipStatsShown(false);
            empty = clipView_->statsRecorded() ? "Nothing read by this frame" : "Not recorded in this clip";
        } else {
            problem = clipView_->statsProblem();
            stats = problem.empty() ? clipStatsShown(true) : nullptr;
            empty = "Reading\xE2\x80\xA6";
        }
    } else {
        if (!uiFinder_) {
            empty = uiFinderError_.empty() ? "The screen is not captured." : uiFinderError_;
        } else if (uiFinder_->state() == UiFinder::State::Failed) {
            empty = uiFinder_->error();
        } else {
            if (liveStats_.image > 0) {
                stats = &liveStats_;
            }
            problem = uiFinder_->stats().statsProblem.data();
        }
    }
    if (problem.empty() && !stats_->error().empty()) {
        problem = stats_->error(); // observex_stats.dll could not be loaded: the app's own rules read.
    }
    // The camera by the mouse's counts on the frame shown: live, or the clip's (Recorded: its camera lines; Today: by
    // its mouse lines, the ground followed over it for the anchors).
    CameraShown camera;
    std::string cameraProblem;
    if (!clipView_) {
        camera = liveCamera();
    } else if (const CameraShown* shown = clipView_->guesser() == ClipView::Guesser::Recorded
                                              ? clipView_->recordedCamera(cameraProblem)
                                              : clipView_->todayCamera(cameraProblem)) {
        camera = *shown;
    }
    statsTile_.draw(fonts_, stats, empty.c_str(), problem.c_str(), stateTree_.skills(), skillIcons_, dpiScale_,
                    camera, cameraProblem.c_str());
}

void App::drawSkills() {
    // Add Skill's miniatures: on the clip's frame shown, else on the live screen.
    MiniatureSource source;
    if (clipView_) {
        source.ask = [this] { return clipView_ ? clipView_->findMiniatures() : 0; };
        source.newest = [this]() -> const UiMiniatures* { return clipView_ ? clipView_->miniatures() : nullptr; };
    } else {
        const auto running = [this] { return uiFinder_ && uiFinder_->state() == UiFinder::State::Running; };
        source.ask = [this, running] { return running() ? uiFinder_->findMiniatures() : 0; };
        source.newest = [this, running]() -> const UiMiniatures* {
            return running() ? &uiFinder_->miniatures() : nullptr;
        };
    }
    RefreshButton refresh;
    refresh.running = skillRefresher_.running();
    refresh.progress = skillRefresher_.progress();
    refresh.click = [this] { toggleRefresh(); };
    skillsView_.draw(fonts_, gpu_.device.Get(), stateTree_, settings_.brainsFolder, skillIcons_, source,
                     [this](const std::string& name, const std::string& key) { keyTyped(name, key); }, refresh,
                     dpiScale_);
}

void App::syncSkills() {
    // Compared only when the profile's file changed (a change saved, another profile, undo or redo).
    if (stateTree_.savedText() == profileSeen_) {
        return;
    }
    profileSeen_ = stateTree_.savedText();
    if (stateTree_.skills() == skillsGiven_) {
        return;
    }
    skillsGiven_ = stateTree_.skills();
    skillIcons_.make(gpu_.device.Get(), skillsGiven_);
    if (uiFinder_) {
        uiFinder_->setSkills(skillsToFind());
    }
    if (clipView_) {
        clipView_->setSkills(skillsToFind());
    }
}

std::vector<SkillToFind> App::skillsToFind() const {
    std::vector<SkillToFind> skills;
    for (const statetree::Skill& skill : stateTree_.skills()) {
        const auto pixels = static_cast<std::size_t>(skill.side) * static_cast<std::size_t>(skill.side);
        if (skill.side <= 0 || skill.bgr.size() != pixels * 3) {
            continue;
        }
        SkillToFind& to = skills.emplace_back();
        to.name = skill.name;
        to.cooldown = skill.cooldown;
        to.side = skill.side;
        to.grey.resize(pixels);
        for (std::size_t i = 0; i < pixels; ++i) { // The grey observex uses everywhere (GreyMaker).
            to.grey[i] = static_cast<std::uint8_t>(
                (skill.bgr[i * 3] * 29 + skill.bgr[i * 3 + 1] * 150 + skill.bgr[i * 3 + 2] * 77) >> 8);
        }
    }
    return skills;
}

namespace {

// A skill seen this many readings in a row (the UI finder reads 10 a second) on one slot with one key is taken: not
// while it is dragged, nor on one image read wrong.
constexpr int kFollowReadings = 3;

// Two places of a miniature are one slot when this near (it is found a pixel either way; slots are ~47 apart).
bool sameSlot(const StatsArea& a, const StatsArea& b) {
    return a.x1 > a.x0 && b.x1 > b.x0 && std::abs(a.x0 - b.x0) <= 3 && std::abs(a.y0 - b.y0) <= 3;
}

} // namespace

void App::followSkills() {
    // Live only: never from a clip or a review (the UI finder is off while one is open).
    if (review_ || clipView_ || !uiFinder_ || uiFinder_->state() != UiFinder::State::Running) {
        return;
    }
    const UiSkillPictures& seen = uiFinder_->skillPictures();
    if (seen.image == 0 || seen.image == skillPicturesImage_) {
        return;
    }
    skillPicturesImage_ = seen.image;
    std::vector<statetree::Skill>& skills = stateTree_.skills();
    bool changed = false;
    for (int i = 0; i < seen.count; ++i) {
        const UiSkillPictures::Picture& picture = seen.pictures[static_cast<std::size_t>(i)];
        const std::string name(picture.name.data(), strnlen(picture.name.data(), picture.name.size()));
        const auto skill = std::ranges::find(skills, name, &statetree::Skill::name);
        const int side = picture.at.x1 - picture.at.x0;
        if (skill == skills.end() || side <= 0 || side > UiSkillPictures::kSide) {
            continue;
        }
        auto follow = std::ranges::find(skillFollow_, name, &SkillFollow::name);
        if (follow == skillFollow_.end()) {
            follow = skillFollow_.insert(skillFollow_.end(), SkillFollow{name});
        }
        const std::string key(picture.key.data(), strnlen(picture.key.data(), picture.key.size()));
        if (sameSlot(follow->at, picture.at) && follow->key == key) {
            ++follow->same;
        } else {
            follow->at = picture.at;
            follow->key = key;
            follow->same = 1;
        }
        follow->side = side;
        follow->bgra.assign(picture.bgra.begin(), picture.bgra.begin() + static_cast<std::ptrdiff_t>(side * side * 4));
        if (follow->same < kFollowReadings) {
            continue;
        }
        if (sameSlot(follow->held, picture.at)) {
            continue; // A key typed for it on this slot stands.
        }
        follow->held = {};
        const bool moved = follow->taken.x1 > follow->taken.x0 && !sameSlot(follow->taken, picture.at);
        const bool newKey = key != skill->key;
        follow->taken = picture.at;
        if (!moved && !newKey) {
            continue;
        }
        // Its shortcut and its miniature on the slot it is in now (found by its picture without the key's corner, it
        // is the same skill; the miniature shows the slot's key as the icon).
        std::string line = skill->name + ":";
        if (newKey) {
            line += " shortcut " + (key == "-" ? std::string("none") : key) +
                    (skill->key.empty() ? std::string() : " (was " + skill->key + ")");
            skill->key = key;
        }
        if (moved) {
            line += newKey ? ", moved to another slot" : " moved to another slot";
        }
        skill->side = side;
        skill->bgr.resize(static_cast<std::size_t>(side * side) * 3);
        for (std::size_t p = 0; p < static_cast<std::size_t>(side * side); ++p) {
            skill->bgr[p * 3] = picture.bgra[p * 4];
            skill->bgr[p * 3 + 1] = picture.bgra[p * 4 + 1];
            skill->bgr[p * 3 + 2] = picture.bgra[p * 4 + 2];
        }
        addLog(line + ", miniature updated");
        changed = true;
    }
    if (changed) {
        stateTree_.saveSkills(); // syncSkills, next, hands the new miniatures to the readers and the icons.
    }
}

void App::keyTyped(const std::string& name, const std::string& key) {
    const auto skill = std::ranges::find(stateTree_.skills(), name, &statetree::Skill::name);
    if (skill == stateTree_.skills().end()) {
        return;
    }
    // Its newest picture seen live, else the profile's.
    const auto follow = std::ranges::find(skillFollow_, name, &SkillFollow::name);
    KeyPicture picture{skill->bgr.data(), static_cast<std::size_t>(skill->side) * 3, 3, skill->side};
    if (follow != skillFollow_.end()) {
        follow->held = follow->taken.x1 > follow->taken.x0 ? follow->taken : follow->at; // It stands there.
        if (follow->side > 0) {
            picture = {follow->bgra.data(), static_cast<std::size_t>(follow->side) * 4, 4, follow->side};
        }
    }
    const auto pixels = static_cast<std::size_t>(picture.side) * static_cast<std::size_t>(picture.side);
    if (key.empty() || key == "-" || key.find('?') != std::string::npos || picture.side <= 0 ||
        (picture.step == 3 && skill->bgr.size() != pixels * 3)) {
        return; // No key, or letters not known: nothing to learn.
    }
    KeyFont font;
    std::string error;
    const std::filesystem::path file = modelFolder("ui") / kKeysFile;
    if (!font.load(file, &error)) {
        addLog(name + ": could not learn its shortcut " + key + " (" + error + ")");
        return;
    }
    if (font.read(picture) == key) {
        return; // Read so already.
    }
    const int learned = font.teach(picture, key);
    if (learned == 0) {
        addLog(name + ": no letters on its miniature to learn the shortcut " + key + " from");
    } else if (!font.save(file, &error)) {
        addLog(name + ": could not learn its shortcut " + key + " (" + error + ")");
    } else {
        addLog(name + ": learned the shortcut " + key + " from its slot (" + std::to_string(learned) +
               (learned == 1 ? " glyph)" : " glyphs)"));
    }
}

void App::addLiveNearby(GameStats& stats) {
    // What drawFound shows: the snapper's boxes while it runs, else the recogniser's.
    const bool looking = recognizer_ && recognizer_->state() == Recognizer::State::Running;
    std::span<const Tracked> things;
    std::span<const std::string> kinds;
    if (looking) {
        const Found* found = &recognizer_->found();
        if (snapper_ && snapper_->state() == BoxSnapper::State::Running) {
            if (const Found& snapped = snapper_->found(); snapped.image > 0) {
                found = &snapped;
            }
        }
        things = {found->things.data(), static_cast<std::size_t>(found->count)};
        kinds = recognizer_->kinds();
    }
    addNearby(stats, looking, things, kinds);
}

std::vector<MouseTarget> App::visibleMonsters() {
    // TEMPORARY (the Mouse debug sweep, 2026-11-15).
    std::vector<MouseTarget> targets;
    for (const SelectableMonster& m : selectableMonsters()) {
        targets.push_back({m.id, m.x, m.y, m.w, m.h});
    }
    return targets;
}

std::vector<SelectableMonster> App::selectableMonsters() {
    // The monsters shown now, as addLiveNearby reads them: the snapper's boxes while it runs, else the recogniser's.
    std::vector<SelectableMonster> monsters;
    if (!observed_ || !recognizer_ || recognizer_->state() != Recognizer::State::Running) {
        return monsters;
    }
    const Found* found = &recognizer_->found();
    if (snapper_ && snapper_->state() == BoxSnapper::State::Running) {
        if (const Found& snapped = snapper_->found(); snapped.image > 0) {
            found = &snapped;
        }
    }
    const std::vector<std::string>& kinds = recognizer_->kinds();
    const auto it = std::find(kinds.begin(), kinds.end(), "monster");
    if (it == kinds.end()) {
        return monsters; // A model without a "monster" kind: nothing to sweep.
    }
    const auto monsterKind = static_cast<std::uint8_t>(it - kinds.begin());
    const RECT& bounds = observed_->bounds; // The monitor's desktop rectangle, in the same pixels as GetCursorPos.
    const double sx = static_cast<double>(bounds.right - bounds.left) / std::max(1u, pipeline_.width());
    const double sy = static_cast<double>(bounds.bottom - bounds.top) / std::max(1u, pipeline_.height());
    for (int i = 0; i < found->count; ++i) {
        const Tracked& thing = found->things[static_cast<std::size_t>(i)];
        if (thing.kind != monsterKind || thing.held || kindSureOf(thing) < kTellKind) {
            continue; // Only monsters it is sure of and sees right now (not held through a gap).
        }
        const double cx = thing.x + thing.width * 0.5;
        const double cy = thing.y + thing.height * 0.5;
        const StatsArea box{static_cast<int>(std::lround(thing.x)), static_cast<int>(std::lround(thing.y)),
                            static_cast<int>(std::lround(thing.x + thing.width)),
                            static_cast<int>(std::lround(thing.y + thing.height))};
        monsters.push_back({thing.id, box, static_cast<int>(std::lround(bounds.left + cx * sx)),
                            static_cast<int>(std::lround(bounds.top + cy * sy)),
                            static_cast<int>(std::lround(thing.width * sx)),
                            static_cast<int>(std::lround(thing.height * sy))});
    }
    return monsters;
}

namespace {

// How long a Use Skill / Click Mouse press is held before release, so the game registers it (a 1 ms tap was missed).
constexpr auto kHoldMs = std::chrono::milliseconds(80);

// A skill's hotbar shortcut (heuristics/KeyLabels, e.g. "F2", "Ctrl+1", "A") as a HID modifier + key code, for the
// controller to press. Handles single keys (F1-F12, A-Z, 0-9) with optional Ctrl / Alt / Shift / Win modifiers joined
// by '+'. False for anything else (an odd label), so the caller can say it does not know how to press it.
bool hidForKey(const std::string& key, std::uint8_t& modifier, std::uint8_t& code) {
    modifier = 0;
    code = 0;
    std::vector<std::string> parts;
    std::string token;
    for (const char c : key) {
        if (c == '+') {
            if (!token.empty()) {
                parts.push_back(token);
            }
            token.clear();
        } else {
            token.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
        }
    }
    if (!token.empty()) {
        parts.push_back(token);
    }
    if (parts.empty()) {
        return false;
    }
    for (std::size_t i = 0; i + 1 < parts.size(); ++i) { // All but the last are modifiers.
        const std::string& m = parts[i];
        if (m == "CTRL" || m == "CONTROL") {
            modifier |= 0x01;
        } else if (m == "SHIFT") {
            modifier |= 0x02;
        } else if (m == "ALT") {
            modifier |= 0x04;
        } else if (m == "WIN" || m == "GUI" || m == "CMD" || m == "META") {
            modifier |= 0x08;
        } else {
            return false; // An unknown modifier: do not guess.
        }
    }
    const std::string& k = parts.back();
    if (k.size() == 1) {
        const char c = k[0];
        if (c >= 'A' && c <= 'Z') {
            code = static_cast<std::uint8_t>(0x04 + (c - 'A'));
            return true;
        }
        if (c >= '1' && c <= '9') {
            code = static_cast<std::uint8_t>(0x1E + (c - '1'));
            return true;
        }
        if (c == '0') {
            code = 0x27;
            return true;
        }
        return false;
    }
    if (k.size() >= 2 && k[0] == 'F') {
        int n = 0;
        if (std::from_chars(k.data() + 1, k.data() + k.size(), n).ec == std::errc() && n >= 1 && n <= 12) {
            code = static_cast<std::uint8_t>(0x3A + (n - 1)); // HID_KEY_F1 is 0x3A.
            return true;
        }
    }
    return false;
}

} // namespace

void App::toggleActing() {
    if (brainActs_) {
        stopActing();
        return;
    }
    // Why it cannot start goes into the Log (the Brain button has no tooltip: the user asked, 2026-11-15).
    if (!observed_ || review_ || clipView_) {
        addLog("Brain not started: observe a game screen first");
        return;
    }
    stopRefresh("Refresh stopped: the brain started"); // Both drive the controller: one at a time.
    mouseTest_.stop(); // The Mouse debug sweep and acting both drive the controller: one at a time.
    if (!controller_.open()) {
        addLog("Brain not started: controller: " + controller_.error());
        return;
    }
    if (controller_.ping() < 0.0) {
        addLog("Brain not started: the controller did not answer; is the firmware running?");
        return;
    }
    brainActs_ = true;
    stateTree_.restart(); // The whole tree afresh, from Root (the user asked, 2026-11-15).
    brainImage_ = 0;      // Its first step on the reading there is now.
    actingTaskStart_ = ~std::uint64_t{0}; // The task that runs starts afresh,
    parallelRuns_.clear();                // and the Parallel ones.
    addLog("Brain active: from Root, it drives the mouse and keyboard through the controller");
}

void App::stopActing() {
    if (!brainActs_) {
        return;
    }
    brainActs_ = false;
    lookAt({});
    parallelRuns_.clear();
    heldButtons_ = 0;
    keyHeld_ = false;
    if (controller_.isOpen()) {
        controller_.moveMouse(0, 0, 0, 0);    // Release any held mouse button,
        controller_.pressKeys(0, nullptr, 0); // and any held key.
    }
    addLog("Brain inactive");
}

void App::actuateBrain() {
    if (!brainActs_) {
        return;
    }
    // Only while a live screen is observed and the link holds; otherwise stop (releasing anything held).
    if (review_ || clipView_ || !observed_ || !controller_.isOpen()) {
        stopActing();
        return;
    }
    const double dt = std::max(0.0, static_cast<double>(ImGui::GetIO().DeltaTime));
    // Release a held key or mouse button once its short hold is up (a press is held, not tapped, so the game registers
    // it; the user asked, 2026-11-15: Use Skill input was not being sent).
    const auto now = std::chrono::steady_clock::now();
    if (keyHeld_ && now >= keyReleaseAt_) {
        controller_.pressKeys(0, nullptr, 0);
        keyHeld_ = false;
    }
    if (heldButtons_ != 0 && now >= buttonReleaseAt_) {
        controller_.moveMouse(0, 0, 0, 0);
        heldButtons_ = 0;
    }
    runParallelTasks(now); // Alongside the one below, the whole time their states are active.
    // The task that runs: one at a time, in order, each until it is done (the user asked, 2026-11-15; core/StateTree's
    // TaskKind says when each is). After the last nothing runs until the state is entered again.
    const statetree::State* state = stateTree_.currentState();
    const int index = stateTree_.currentTask();
    if (state == nullptr || index < 0 || index >= static_cast<int>(state->tasks.size())) {
        lookAt({});
        return;
    }
    const statetree::Task& task = state->tasks[static_cast<std::size_t>(index)];
    if (stateTree_.taskStarts() != actingTaskStart_) { // A task starts (or starts again): afresh.
        actingTaskStart_ = stateTree_.taskStarts();
        taskPressed_ = false;
        selector_.start();
        skillUser_.start();
    }
    if (task.kind != statetree::TaskKind::SelectTarget) {
        lookAt({});
    }
    const bool pressing = keyHeld_ || heldButtons_ != 0; // A press still held: the next one waits for its release.
    switch (task.kind) {
    case statetree::TaskKind::SelectTarget: {
        // Onto the monster, a click once the game shows it hovered, done once it shows it as the target
        // (core/TargetSelect), on the stats reader's name plate over it (asked for here) and its target window.
        LARGE_INTEGER frequency;
        LARGE_INTEGER counter;
        QueryPerformanceFrequency(&frequency);
        QueryPerformanceCounter(&counter);
        const double ticks = static_cast<double>(frequency.QuadPart);
        POINT cursor{};
        GetCursorPos(&cursor);
        const std::vector<SelectableMonster> monsters = selectableMonsters();
        TargetSelector::Input in;
        in.now = static_cast<double>(counter.QuadPart) / ticks;
        in.dt = dt;
        in.cursorX = cursor.x;
        in.cursorY = cursor.y;
        in.monsters = monsters;
        if (liveStats_.image > 0) {
            in.seen.image = liveStats_.image;
            in.seen.shown = static_cast<double>(liveStats_.shownQpc) / ticks;
            in.seen.plate = liveStats_.plate;
            in.seen.hasTarget = liveStats_.hasTarget;
            in.seen.targetName = liveStats_.targetName.data();
            const GameGauge* hp = liveStats_.find(kTargetPiece, "HP");
            in.seen.targetHp = hp != nullptr ? hp->full : -1.0f;
        }
        const TargetSelector::Output out = selector_.step(in);
        lookAt(out.look);
        if ((out.dx != 0 || out.dy != 0) && heldButtons_ == 0) { // A click is pressed and let go where it was pressed.
            controller_.moveMouse(out.dx, out.dy, 0);
        }
        if (out.click) {
            heldButtons_ = 0x01;
            controller_.moveMouse(0, 0, heldButtons_, 0); // Press; held kHoldMs, then released at the top.
            buttonReleaseAt_ = now + kHoldMs;
        }
        if (!out.log.empty()) {
            addLog(out.log);
        }
        if (out.done) {
            lookAt({});
            stateTree_.finishTask();
        }
        break;
    }
    case statetree::TaskKind::ClickMouse:
        // Done once clicked: pressed, held kHoldMs and let go.
        if (!taskPressed_ && !pressing) {
            heldButtons_ = task.button == statetree::ClickButton::Right ? 0x02 : 0x01;
            controller_.moveMouse(0, 0, heldButtons_, 0); // Press; held kHoldMs, then released at the top.
            buttonReleaseAt_ = now + kHoldMs;
            taskPressed_ = true;
        } else if (taskPressed_ && heldButtons_ == 0) {
            addLog(std::string("Click Mouse: ") + statetree::nameOf(task.button) + " button clicked");
            stateTree_.finishTask();
        }
        break;
    case statetree::TaskKind::UseSkill: {
        // Of its skills, top to bottom, the first ready one is pressed; done once its cast ended or its MP cost was
        // taken, with fallbacks (core/SkillUse). On the newest live reading.
        std::vector<UsableSkill> usable;
        usable.reserve(task.skills.size());
        for (const std::string& name : task.skills) {
            const statetree::Skill* skill = stateTree_.tree().skill(name);
            std::uint8_t mod = 0;
            std::uint8_t code = 0;
            usable.push_back({name, skill != nullptr ? skill->key : std::string(),
                              skill != nullptr && hidForKey(skill->key, mod, code),
                              skill == nullptr || skill->cooldown, skill != nullptr ? skill->mp : 0});
        }
        LARGE_INTEGER frequency;
        LARGE_INTEGER counter;
        QueryPerformanceFrequency(&frequency);
        QueryPerformanceCounter(&counter);
        const double ticks = static_cast<double>(frequency.QuadPart);
        SkillUser::Input in;
        in.now = static_cast<double>(counter.QuadPart) / ticks;
        in.canPress = !pressing;
        in.skills = usable;
        if (liveStats_.image > 0) {
            if (liveStats_.image != skillVariablesImage_) {
                skillVariablesImage_ = liveStats_.image;
                skillVariables_ = statetree::variablesOf(liveStats_);
            }
            in.image = liveStats_.image;
            in.shown = static_cast<double>(liveStats_.shownQpc) / ticks;
            in.variables = &skillVariables_;
        }
        const SkillUser::Output out = skillUser_.step(in);
        if (out.press >= 0) {
            std::uint8_t mod = 0;
            std::uint8_t code = 0;
            hidForKey(usable[static_cast<std::size_t>(out.press)].key, mod, code);
            const std::uint8_t keys[1] = {code};
            controller_.pressKeys(mod, keys, 1); // Press; held kHoldMs, then released at the top.
            keyHeld_ = true;
            keyReleaseAt_ = now + kHoldMs;
        }
        if (!out.log.empty()) {
            addLog(out.log);
        }
        if (out.done && !keyHeld_) { // Done once its key was let go too.
            stateTree_.finishTask();
        }
        break;
    }
    case statetree::TaskKind::DebugPrint:
        // Where the brain is, written once; done.
        addLog("Debug Print: " + statetree::whereText(stateTree_.tree(), state->id, index));
        stateTree_.finishTask();
        break;
    }
}

void App::runParallelTasks(std::chrono::steady_clock::time_point now) {
    const std::vector<statetree::TaskAt> running = stateTree_.parallelTasks();
    const statetree::Tree& tree = stateTree_.tree();
    const auto taskAt = [&tree](const statetree::TaskAt& at) -> const statetree::Task& {
        return tree.find(at.state)->tasks[static_cast<std::size_t>(at.task)];
    };
    // Its state left, or the task there changed: it stops (in its state again, it starts afresh).
    std::erase_if(parallelRuns_, [&](const ParallelRun& run) {
        return std::ranges::find(running, run.at) == running.end() || taskAt(run.at).kind != run.kind;
    });
    const auto period = std::chrono::duration_cast<std::chrono::steady_clock::duration>(
        std::chrono::duration<double>(statetree::kDebugPrintSeconds));
    for (const statetree::TaskAt& at : running) {
        const statetree::Task& task = taskAt(at);
        auto run = std::ranges::find(parallelRuns_, at, &ParallelRun::at);
        if (run == parallelRuns_.end()) { // Its state became active: it starts, due at once.
            parallelRuns_.push_back({at, task.kind, now});
            run = parallelRuns_.end() - 1;
        }
        switch (task.kind) {
        case statetree::TaskKind::DebugPrint:
            // Where the brain is, every kDebugPrintSeconds (from when it last wrote; after a stall, from now).
            if (now >= run->next) {
                const statetree::State* current = stateTree_.currentState();
                addLog("Debug Print: " + statetree::whereText(tree, current != nullptr ? current->id : tree.root(),
                                                              stateTree_.currentTask()));
                run->next = std::max(run->next + period, now);
            }
            break;
        case statetree::TaskKind::UseSkill:
        case statetree::TaskKind::SelectTarget:
        case statetree::TaskKind::ClickMouse:
            break; // Never Parallel (statetree::canRunParallel): they share the keyboard and mouse.
        }
    }
}

std::vector<RefreshSkill> App::refreshSkills() const {
    std::vector<RefreshSkill> skills;
    const std::vector<statetree::Skill>& profile = stateTree_.skills();
    skills.reserve(profile.size());
    const bool live = observed_ && liveStats_.image > 0 && pipeline_.width() > 0 && pipeline_.height() > 0;
    for (const statetree::Skill& skill : profile) {
        RefreshSkill one;
        one.name = skill.name;
        const GameGauge* gauge = live ? liveStats_.find(kSkillsPiece, skill.name) : nullptr;
        if (gauge != nullptr && gauge->seen && gauge->at.x1 > gauge->at.x0) {
            const RECT& bounds = observed_->bounds; // The monitor on the desktop, in GetCursorPos' pixels.
            const double sx = static_cast<double>(bounds.right - bounds.left) / pipeline_.width();
            const double sy = static_cast<double>(bounds.bottom - bounds.top) / pipeline_.height();
            const StatsArea& at = gauge->at;
            one.seen = true;
            one.at = at;
            one.x = static_cast<int>(std::lround(bounds.left + (at.x0 + at.x1) * 0.5 * sx));
            one.y = static_cast<int>(std::lround(bounds.top + (at.y0 + at.y1) * 0.5 * sy));
            one.side = static_cast<int>(std::lround((at.x1 - at.x0) * sx));
        }
        skills.push_back(std::move(one));
    }
    return skills;
}

void App::toggleRefresh() {
    if (skillRefresher_.running()) {
        stopRefresh("Refresh stopped");
        return;
    }
    // Why it cannot start goes into the Log, as the Brain's.
    if (!observed_ || review_ || clipView_) {
        addLog("Refresh not started: observe a game screen first");
        return;
    }
    if (brainActs_) {
        addLog("Refresh not started: stop the brain first (both drive the mouse)");
        return;
    }
    if (!uiFinder_ || uiFinder_->state() != UiFinder::State::Running || liveStats_.image == 0) {
        addLog("Refresh not started: nothing is read from the screen yet");
        return;
    }
    const std::vector<RefreshSkill> skills = refreshSkills();
    if (skills.empty()) {
        addLog("Refresh not started: the profile has no skills (Add Skill adds them)");
        return;
    }
    const auto first = std::find_if(skills.begin(), skills.end(), [](const RefreshSkill& skill) { return skill.seen; });
    if (first == skills.end()) {
        addLog("Refresh not started: none of the profile's skills is seen on the hotbar");
        return;
    }
    mouseTest_.stop(); // The Mouse debug sweep drives the controller too: one at a time.
    if (!controller_.open()) {
        addLog("Refresh not started: controller: " + controller_.error());
        return;
    }
    if (controller_.ping() < 0.0) {
        addLog("Refresh not started: the controller did not answer; is the firmware running?");
        return;
    }
    // The game in front, so it shows its tooltips (observex is in front: its button was just clicked).
    if (HWND game = GetAncestor(WindowFromPoint(POINT{first->x, first->y}), GA_ROOT);
        game != nullptr && (!window_ || game != window_->handle()) && !SetForegroundWindow(game)) {
        addLog("Refresh: could not bring the game to the front; its tooltips may not show");
    }
    POINT cursor{};
    GetCursorPos(&cursor);
    skillRefresher_.start(cursor.x, cursor.y);
    refreshAsked_ = 0;
    addLog("Refresh: the mouse goes over each of " + std::to_string(skills.size()) +
           " skills to read its MP cost off its tooltip (Win+Esc stops it)");
}

void App::stopRefresh(const std::string& why) {
    if (!skillRefresher_.running()) {
        return;
    }
    skillRefresher_ = SkillRefresher(); // Not running; it only moves the cursor, so nothing is held to let go.
    refreshAsked_ = 0;
    addLog(why);
}

void App::driveRefresh() {
    if (!skillRefresher_.running()) {
        return;
    }
    if (review_ || clipView_ || !observed_ || !uiFinder_ || uiFinder_->state() != UiFinder::State::Running) {
        stopRefresh("Refresh stopped: the game screen is no longer observed");
        return;
    }
    if (!controller_.isOpen()) {
        stopRefresh("Refresh stopped: the controller was lost");
        return;
    }
    LARGE_INTEGER frequency;
    LARGE_INTEGER counter;
    QueryPerformanceFrequency(&frequency);
    QueryPerformanceCounter(&counter);
    const double ticks = static_cast<double>(frequency.QuadPart);
    POINT cursor{};
    GetCursorPos(&cursor);
    const std::vector<RefreshSkill> skills = refreshSkills();
    SkillRefresher::Input in;
    in.now = static_cast<double>(counter.QuadPart) / ticks;
    in.dt = std::max(0.0, static_cast<double>(ImGui::GetIO().DeltaTime));
    in.cursorX = cursor.x;
    in.cursorY = cursor.y;
    in.imageWidth = static_cast<int>(pipeline_.width());
    in.imageHeight = static_cast<int>(pipeline_.height());
    in.skills = skills;
    if (refreshAsked_ != 0) {
        const UiText& text = uiFinder_->text();
        if (text.request == refreshAsked_) {
            in.answered = true;
            in.shown = static_cast<double>(text.shownQpc) / ticks;
            in.text = text.text.data();
            refreshAsked_ = 0;
        }
    }
    const SkillRefresher::Output out = skillRefresher_.step(in);
    if (out.dx != 0 || out.dy != 0) {
        controller_.moveMouse(out.dx, out.dy, 0);
    }
    if (out.read) {
        refreshAsked_ = uiFinder_->readText(out.area);
    }
    if (out.learned) {
        std::vector<statetree::Skill>& profile = stateTree_.skills();
        const auto skill = std::find_if(profile.begin(), profile.end(),
                                        [&](const statetree::Skill& one) { return one.name == out.name; });
        if (skill != profile.end()) {
            char line[200];
            if (skill->mp == out.mp) {
                std::snprintf(line, sizeof(line), "Refresh %s: MP cost %d, as it was", out.name.c_str(), out.mp);
            } else {
                const std::string was = skill->mp > 0 ? std::to_string(skill->mp) : std::string("not set");
                std::snprintf(line, sizeof(line), "Refresh %s: MP cost %d (was %s)", out.name.c_str(), out.mp,
                              was.c_str());
                skill->mp = out.mp;
                stateTree_.saveSkills(); // Into the profile at once (Ctrl+Z in the State tile undoes it).
            }
            addLog(line);
        }
    }
    if (!out.log.empty()) {
        addLog(out.log);
    }
}

void App::lookAt(const PlateLook& look) {
    brainLook_ = look;
    sendLook();
}

void App::sendLook() {
    const PlateLook& look = brainLook_.id != 0 ? brainLook_ : watchLook_;
    if (look == lookAsked_) {
        return;
    }
    lookAsked_ = look;
    if (uiFinder_) {
        uiFinder_->setLook(look);
    }
}

std::vector<WatchedThing> App::thingsShown() {
    std::vector<WatchedThing> things;
    if (recognizer_ && recognizer_->state() == Recognizer::State::Running) {
        const Found* found = &recognizer_->found();
        if (snapper_ && snapper_->state() == BoxSnapper::State::Running) {
            if (const Found& snapped = snapper_->found(); snapped.image > 0) {
                found = &snapped;
            }
        }
        things.reserve(static_cast<std::size_t>(found->count));
        for (int i = 0; i < found->count; ++i) {
            const Tracked& thing = found->things[static_cast<std::size_t>(i)];
            things.push_back({thing.id, {static_cast<int>(std::lround(thing.x)), static_cast<int>(std::lround(thing.y)),
                                         static_cast<int>(std::lround(thing.x + thing.width)),
                                         static_cast<int>(std::lround(thing.y + thing.height))}});
        }
    }
    return things;
}

void App::watchTarget(const GameStats& stats) {
    const std::vector<WatchedThing> things = thingsShown();
    const GameGauge* hp = stats.find(kTargetPiece, "HP");
    TargetWatch::Input in;
    in.image = stats.image;
    in.hasTarget = stats.hasTarget;
    in.target = stats.target.data();
    in.targetHp = hp != nullptr ? hp->full : -1.0f;
    in.plate = stats.plate;
    in.things = things;
    const TargetWatch::Output out = targetWatch_.step(in);
    if (out.dead != 0 && recognizer_) {
        recognizer_->markDead(out.dead); // Shown dead from its next look on.
    }
    if (out.dead != 0) {
        deadNow_ = out.dead; // Its drops are looked for round it (watchLoot).
        deadName_ = stats.target.data();
    }
    if (!out.log.empty()) {
        addLog(out.log, stats.shownQpc);
    }
    watchLook_ = out.look;
    sendLook();
}

void App::watchLoot() {
    const std::uint32_t dead = std::exchange(deadNow_, 0);
    if (!dropFinder_ || dropFinder_->state() == DropFinder::State::Failed || review_ || clipView_) {
        loot_ = {}; // Nothing looks meanwhile; the watch goes on when it can again.
        lootShown_.clear();
        noteDropsInClip();
        return;
    }
    const std::vector<WatchedThing> things = thingsShown();
    // The game's interface (the UI finder's pieces): the cast bar and the minimap's arrow looked like drops to the
    // first drop model, and nothing on it lies on the ground.
    std::span<const UiPiece> ui;
    if (uiFinder_ && uiFinder_->state() == UiFinder::State::Running) {
        const UiPieces& pieces = uiFinder_->found();
        ui = {pieces.pieces.data(), static_cast<std::size_t>(pieces.count)};
    }
    std::vector<DropSeen> close;
    std::vector<DropSeen> sweep;
    lootShown_.clear();
    LootWatch::Input in;
    if (dropFinder_->state() == DropFinder::State::Running) {
        const FoundDrops& found = dropFinder_->found();
        const auto keep = [&ui](const DropLook& look, std::vector<DropSeen>& kept, std::vector<Detection>* shown) {
            for (int i = 0; i < look.count; ++i) {
                const Detection& drop = look.drops[static_cast<std::size_t>(i)];
                const float x = drop.x + drop.width * 0.5f;
                const float y = drop.y + drop.height * 0.5f;
                if (std::any_of(ui.begin(), ui.end(), [x, y](const UiPiece& piece) {
                        return x >= static_cast<float>(piece.x0) && x < static_cast<float>(piece.x1) &&
                               y >= static_cast<float>(piece.y0) && y < static_cast<float>(piece.y1);
                    })) {
                    continue;
                }
                kept.push_back({{static_cast<int>(std::lround(drop.x)), static_cast<int>(std::lround(drop.y)),
                                 static_cast<int>(std::lround(drop.x + drop.width)),
                                 static_cast<int>(std::lround(drop.y + drop.height))},
                                drop.sure});
                if (shown != nullptr) {
                    shown->push_back(drop);
                }
            }
        };
        keep(found.close, close, &lootShown_);
        keep(found.sweep, sweep, nullptr);
        in.close.looked = found.close.image;
        in.sweep.looked = found.sweep.image;
        lootImage_ = found.close.image;
    }
    in.ms = static_cast<double>(qpcNow()) * 1000.0 / qpcFrequency();
    in.width = static_cast<int>(pipeline_.width());
    in.height = static_cast<int>(pipeline_.height());
    in.dead = dead;
    in.name = deadName_;
    in.things = things;
    in.close.drops = close;
    in.sweep.drops = sweep;
    loot_ = lootWatch_.step(in);
    dropFinder_->lookAt(loot_.square);
    if (loot_.drops == 0) {
        lootShown_.clear(); // The newest close look may be of an earlier spot until this one's first.
    }
    if (!loot_.log.empty()) {
        addLog(loot_.log);
    }
    noteDropsInClip();
}

void App::noteDropsInClip() {
    if (!clipRecorder_) {
        return;
    }
    // What drawDrops draws, whatever the flag says (the drop finder looks either way, as the recogniser does): a line
    // whenever it changes, by a new close look or another spot, or nothing drawn any more.
    const bool drawn = dropFinder_ && dropFinder_->state() == DropFinder::State::Running && !lootShown_.empty();
    const LootSquare square = drawn ? loot_.square : LootSquare{};
    const std::uint64_t image = drawn ? lootImage_ : 0;
    const bool kill = drawn && loot_.kill;
    if (dropsNoted_ && square == dropsNotedSquare_ && image == dropsNotedImage_ && kill == dropsNotedKill_) {
        return;
    }
    dropsNoted_ = true;
    dropsNotedSquare_ = square;
    dropsNotedImage_ = image;
    dropsNotedKill_ = kill;
    clipRecorder_->dropsShown(qpcNow(), image, kill, square,
                              drawn ? std::span<const Detection>(lootShown_) : std::span<const Detection>());
}

void App::drawDrops(ImDrawList* drawList, ImVec2 min, ImVec2 max) {
    // Drops a close look found (one sweep look alone may be a look-alike: not drawn).
    if (!dropFinder_ || dropFinder_->state() != DropFinder::State::Running || lootShown_.empty()) {
        return;
    }
    drawLoot(drawList, fonts_.medium, min, max, static_cast<float>(pipeline_.width()), lootShown_, dpiScale_);
}

const GameStats* App::clipStatsShown(bool today) {
    const GameStats* read = today ? clipView_->todayStats() : clipView_->recordedStats();
    if (read == nullptr) {
        return nullptr;
    }
    clipStats_ = *read;
    addNearby(clipStats_, true, clipView_->thingsShown(), clipView_->kinds());
    return &clipStats_;
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

void App::noteDepthInClip() {
    if (!clipRecorder_ || previewQpc_ == 0 || depthDrawn_ == depthNoted_) {
        return;
    }
    // A line whenever another map is drawn, or none any more.
    depthNoted_ = depthDrawn_;
    clipRecorder_->depthShown(previewQpc_, previewImage_, depthDrawn_);
}

CameraShown App::liveCamera() const {
    if (!mouseCamera_) {
        return {};
    }
    float coneOff = 0.0f;
    float turnsOff = 0.0f;
    mouseCamera_->offs(coneOff, turnsOff);
    return cameraOf(mouseCamera_->at(previewMs_), coneOff, turnsOff);
}

void App::noteCameraInClip() {
    if (!clipRecorder_ || previewQpc_ == 0) {
        return;
    }
    // What the Camera card shows (whatever the tab shows): a line whenever it changes, so every frame it turns.
    const CameraShown camera = liveCamera();
    if (cameraNoted_ && camera == notedCamera_) {
        return;
    }
    cameraNoted_ = true;
    notedCamera_ = camera;
    clipRecorder_->cameraShown(previewQpc_, previewImage_, camera);
}

void App::noteBrainInClip() {
    if (!clipRecorder_ || previewQpc_ == 0) {
        return;
    }
    // What the State tile shows green: the current state and its task that runs, while the brain acts; nothing while
    // not. A line whenever it changes, by the state's names (a profile read again gets other ids).
    const statetree::State* state = brainActs_ ? stateTree_.currentState() : nullptr;
    const int id = state != nullptr ? state->id : 0;
    const int task = state != nullptr ? stateTree_.currentTask() : -1;
    if (brainNoted_ && id == brainNotedState_ && task == brainNotedTask_) {
        return;
    }
    brainNoted_ = true;
    brainNotedState_ = id;
    brainNotedTask_ = task;
    statetree::BrainShown brain;
    brain.acting = state != nullptr;
    brain.task = task;
    if (state != nullptr) {
        brain.state = statetree::statePath(stateTree_.tree(), id);
    }
    clipRecorder_->brained(previewQpc_, previewImage_, std::move(brain));
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
    handmade_.clear();
    for (RecordingEntry& entry : listRecordings(settings_.recordingsFolder)) {
        RecordingRow row;
        const bool handmade = entry.info.kind == RecordKind::Handmade;
        // A handmade package by the user's name for it: "15/11/2026 - 15:00".
        row.when = handmade ? handmadeTitle(entry.info.started) : describeWhen(entry.info.started);
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
        (handmade ? handmade_ : recordings_).push_back(std::move(row));
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
        if (!settings_.showLive) { // It is shown in the Live tile.
            settings_.showLive = true;
            saveSettings();
        }
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
    // Not in snapshot mode: it would write into models/, which tests must leave alone. Only for a category with kinds
    // (not Drops: a drop is a drop, there are no kinds to learn).
    if (!snapshot_ && !category.kinds.empty()) {
        kindUpdater_.reset(); // One still at work stops first: the new one learns from everything it would.
        kindUpdater_ = std::make_unique<KindUpdater>(settings_.recordingsFolder, category.id, kindIdsOf(category),
                                                     modelFolder(category.id));
    }
}

void App::openClip(const ClipEntry& clip) {
    closeReview(); // One thing at a time is shown instead of the screen.
    closeClip();
    clipView_ = std::make_unique<ClipView>(gpuAdapter_.Get(), gpu_.device.Get(), clip.folder, clip.info,
                                           *todaysHeuristics(), todaysSnapping(), todaysGround(), todaysDepth(),
                                           modelFolder("ui"), stats_, modelFolder("drop"));
    clipView_->setSkills(skillsToFind()); // The profile's, read again on its frames.
    ++clipsOpened_; // The Log tile's lines are this clip's now.
    noteInClip("clip opened: " + nameOf(clip.folder));
    if (!settings_.showLive) { // It is shown in the Live tile.
        settings_.showLive = true;
        saveSettings();
    }
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

std::unique_ptr<Depthing> App::todaysDepth() const {
    try {
        return heuristics_->makeDepth();
    } catch (const std::exception&) {
        return HeuristicsSource().makeDepth();
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
        } else if (frame == 2 + moveFrames && input.release) {
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
    // Win+Esc (a hot key, so it works while the game is in front) starts or stops the Brain.
    if (brainKey_ && brainKey_->pressed()) {
        if (skillRefresher_.running()) {
            stopRefresh("Refresh stopped (Win+Esc)"); // It stops Refresh first, as it would the brain.
        } else {
            toggleActing();
        }
    }
    // TEMPORARY (the Mouse debug sweep, 2026-11-15): drive it one step this frame from the real cursor and the
    // monsters shown now, and draw its countdown big over everything.
    if (mouseTest_.active()) {
        POINT cursor{};
        GetCursorPos(&cursor);
        mouseTest_.tick(visibleMonsters(), cursor.x, cursor.y,
                        static_cast<double>(ImGui::GetIO().DeltaTime) * 1000.0);
    }
    if (mouseTest_.countingDown()) {
        const ImGuiViewport* countdownView = ImGui::GetMainViewport();
        ImDrawList* front = ImGui::GetForegroundDrawList();
        char buf[8];
        std::snprintf(buf, sizeof(buf), "%d", std::max(1, static_cast<int>(std::ceil(mouseTest_.countdownLeft()))));
        const float big = std::round(140.0f * dpiScale_);
        const ImVec2 textSize = fonts_.medium->CalcTextSizeA(big, FLT_MAX, 0.0f, buf);
        const ImVec2 at(countdownView->Pos.x + std::round((countdownView->Size.x - textSize.x) * 0.5f),
                        countdownView->Pos.y + std::round((countdownView->Size.y - textSize.y) * 0.5f));
        const float shadow = std::round(3.0f * dpiScale_);
        front->AddText(fonts_.medium, big, ImVec2(at.x + shadow, at.y + shadow), IM_COL32(0, 0, 0, 160), buf);
        front->AddText(fonts_.medium, big, at, theme::kAccent, buf);
    }
    frameMeter_.lap(FrameStep::NewFrame);
    actuateBrain(); // While acting: run the current state's tasks (drive the controller).
    driveRefresh(); // While the Skills tab's Refresh runs: the cursor over the skills, their MP costs read.
    frameMeter_.lap(FrameStep::Acting);
    if (observed_) {
        // Whether the brain drives the mouse/keyboard, seen even from the corner of the eye while watching the game
        // (the user asked, 2026-11-15): a border round the whole window, green while it is active, red while a screen
        // is observed and it is not; none while no screen is.
        const ImGuiViewport* actView = ImGui::GetMainViewport();
        ImDrawList* front = ImGui::GetForegroundDrawList();
        const float thick = std::round(3.0f * dpiScale_);
        front->AddRect(ImVec2(actView->Pos.x + thick * 0.5f, actView->Pos.y + thick * 0.5f),
                       ImVec2(actView->Pos.x + actView->Size.x - thick * 0.5f,
                              actView->Pos.y + actView->Size.y - thick * 0.5f),
                       brainActs_ ? theme::kYes : theme::kRecord, 0.0f, 0, thick);
    }
    picker_->setObserved(observed_ ? observed_->name : std::string(), observedImage_.get());

    // The view before the bar over it: while the State tile has the keyboard, it takes the keys pressed this frame
    // before the bar's clip steps or the Live tile read them.
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const float barHeight = std::round(52.0f * dpiScale_);
    ImGui::SetNextWindowPos(ImVec2(viewport->Pos.x, viewport->Pos.y + barHeight));
    ImGui::SetNextWindowSize(ImVec2(viewport->Size.x, viewport->Size.y - barHeight));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::Begin("##view", nullptr, kFixedWindow);
    ImGui::PopStyleVar();
    const float tabsHeight = std::round(38.0f * dpiScale_);
    drawTabs(tabsHeight);
    const ImVec2 pos = ImGui::GetWindowPos();
    const ImVec2 size = ImGui::GetWindowSize();
    // The Debug tile a narrower column at the right, the Log tile one left of it, the others left of them.
    const float tileGap = std::round(kTileGap * dpiScale_);
    TileRect area{pos.x, pos.y + tabsHeight, pos.x + size.x, pos.y + size.y};
    const bool leftTiles = settings_.showLive || settings_.showGameStats || settings_.showState || settings_.showSkills;
    TileRect debugTile;
    if (settings_.showDebug) {
        TileRect others;
        debugTile = placeColumn(area, leftTiles || settings_.showLog, debugSplit_, tileGap, others);
        area = others;
    }
    TileRect logTile;
    if (settings_.showLog) {
        TileRect others;
        logTile = placeColumn(area, leftTiles, logSplit_, tileGap, others);
        area = others;
    }
    // Skills, while open, covers the column of Stats and State, whichever are on (the user asked, 2026-11-14): it goes
    // where Stats goes, as tall as both.
    const bool skills = settings_.showSkills;
    const Tiles tiles = placeTiles(settings_.showLive, settings_.showGameStats || skills,
                                   settings_.showState && !skills, area, liveSplit_, statsSplit_, tileGap);
    // Errors stack at the top right of the Live tile, or of the view without it.
    const float inset = std::round(12.0f * dpiScale_);
    const auto drawErrors = [&](ImVec2 corner) {
        for (const std::string* error : {&recordingError_, &reviewError_, &clipSaveError_}) {
            if (!error->empty()) {
                theme::drawLabel(ImGui::GetWindowDrawList(), corner, ImVec2(1.0f, 0.0f), error->c_str());
                corner.y += ImGui::GetTextLineHeight() + std::round(16.0f * dpiScale_);
            }
        }
    };
    // Each tile is a window of its own, with the usual padding.
    const auto beginTile = [](const char* id, const TileRect& tile, ImGuiWindowFlags flags) {
        ImGui::SetCursorScreenPos(ImVec2(tile.left, tile.top));
        ImGui::BeginChild(id, ImVec2(tile.right - tile.left, tile.bottom - tile.top),
                          ImGuiChildFlags_AlwaysUseWindowPadding, flags);
    };
    if (tiles.state.shown()) { // First: see above.
        beginTile("##state", tiles.state, ImGuiWindowFlags_HorizontalScrollbar);
        // Its Brain button (the top bar's State tree button until 2026-11-15) starts or stops acting. With a clip open,
        // the brain as recorded at the frame shown is green (the user asked, 2026-11-15), its condition dots on the
        // reading the Stats tile shows (Recorded's or Today's).
        StateTreeView::Replay replay;
        statetree::Variables clipVariables;
        if (clipView_) {
            replay.recorded = clipView_->brainRecorded();
            replay.brain = clipView_->recordedBrain();
            const bool today = clipView_->guesser() == ClipView::Guesser::Today;
            if (const GameStats* stats = clipStatsShown(today); stats != nullptr) {
                clipVariables = statetree::variablesOf(*stats);
                replay.variables = &clipVariables;
            }
        }
        if (stateTree_.draw(fonts_, settings_.brainsFolder, skillIcons_, brainActs_, clipView_ ? &replay : nullptr)) {
            toggleActing();
        }
        ImGui::EndChild();
    }
    if (stateTree_.name() != settings_.brainName) { // Saved or loaded under another name: loaded again at start.
        settings_.brainName = stateTree_.name();
        saveSettings();
    }
    if (tiles.live.shown()) {
        beginTile("##live", tiles.live, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        drawLive();
        drawErrors(ImVec2(tiles.live.right - inset, tiles.live.top + inset));
        ImGui::EndChild();
    }
    if (tiles.stats.shown()) {
        beginTile(skills ? "##skills" : "##stats", tiles.stats, ImGuiWindowFlags_None);
        if (skills) {
            drawSkills();
        } else {
            drawGameStats();
        }
        ImGui::EndChild();
    }
    if (logTile.shown()) {
        beginTile("##log", logTile, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        drawLog();
        ImGui::EndChild();
    }
    if (debugTile.shown()) {
        beginTile("##debug", debugTile, ImGuiWindowFlags_None);
        drawDebug();
        ImGui::EndChild();
    }
    drawTileLines(tiles, logTile, debugTile, pos.x);
    if (!tiles.live.shown()) {
        drawErrors(ImVec2(pos.x + size.x - inset, pos.y + tabsHeight + inset));
    }
    ImGui::End();

    drawTopBar(barHeight);
}

void App::drawTabs(float height) {
    const ImVec2 pos = ImGui::GetWindowPos();
    ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(pos.x, pos.y + height - 1.0f),
                                              ImVec2(pos.x + ImGui::GetWindowWidth(), pos.y + height), theme::kBorder);
    // No tooltips (the user asked, 2026-11-12: too much clutter).
    struct Tab {
        const char* id;
        const char* text;
        bool* on;
    };
    const Tab tabs[] = {
        {"##tab-live", "Live", &settings_.showLive},
        {"##tab-state", "State", &settings_.showState},
        {"##tab-stats", "Stats", &settings_.showGameStats},
        {"##tab-skills", "Skills", &settings_.showSkills},
        {"##tab-log", "Log", &settings_.showLog},
        {"##tab-debug", "Debug", &settings_.showDebug},
    };
    // Stats and State under Skills are not shown: drawn off, and a click on one closes Skills and shows it.
    const auto under = [this](const bool* on) {
        return settings_.showSkills && (on == &settings_.showGameStats || on == &settings_.showState);
    };
    int shown = 0;
    for (const Tab& tab : tabs) {
        shown += *tab.on && !under(tab.on) ? 1 : 0;
    }
    float x = pos.x + std::round(12.0f * dpiScale_);
    for (const Tab& tab : tabs) {
        const bool on = *tab.on && !under(tab.on);
        const bool last = on && shown == 1;
        ImGui::SetCursorScreenPos(ImVec2(x, pos.y));
        if (drawTab(tab.id, tab.text, on, height, fonts_) && !last) {
            if (under(tab.on) || (settings_.showSkills && !*tab.on && (tab.on == &settings_.showGameStats ||
                                                                       tab.on == &settings_.showState))) {
                settings_.showSkills = false;
                *tab.on = true;
            } else {
                *tab.on = !*tab.on;
            }
            saveSettings();
        }
        x = ImGui::GetItemRectMax().x + std::round(2.0f * dpiScale_);
    }

    // Controller light right of the tabs (after Log; the user asked, 2026-11-15, moved from the row's far right): a
    // green/red dot, "Controller", and in smaller, dimmer text whether the reactor's Raspberry Pi Pico is plugged in.
    // All three centred on the tab names' middle by their capitals (the user asked, 2026-11-15: centring the words'
    // line boxes set "Connected" 1-2 px higher than "Controller" and the group above the tab names).
    {
        const bool connected = picoLink_.connected();
        ImDrawList* drawList = ImGui::GetWindowDrawList();
        ImFont* font = ImGui::GetFont();
        const float fontSize = ImGui::GetFontSize();
        const float smallSize = std::round(fontSize * 0.82f);
        const float radius = std::round(5.0f * dpiScale_);
        const float dotGap = std::round(8.0f * dpiScale_);
        const float stateGap = std::round(9.0f * dpiScale_);
        const char* name = "Controller";
        const char* state = connected ? "Connected" : "Not Connected";
        const ImVec2 nameSize = ImGui::CalcTextSize(name);
        const float startX = x + std::round(26.0f * dpiScale_); // Set a little apart from the last tab.
        const float midY = tabTextMiddle(pos.y, height, fonts_);
        const ImU32 color = connected ? theme::kYes : theme::kRecord;
        const ImVec2 center(startX + radius, midY);
        drawList->AddCircleFilled(center, radius, color, 16);
        drawList->AddCircle(center, radius, theme::faded(color, 0.45f), 16, std::round(1.0f * dpiScale_));
        const ImVec2 nameAt(startX + radius * 2.0f + dotGap, std::round(midY - capsMiddle(font, fontSize)));
        drawList->AddText(nameAt, theme::kText, name);
        const ImVec2 stateAt(nameAt.x + nameSize.x + stateGap, std::round(midY - capsMiddle(font, smallSize)));
        drawList->AddText(font, smallSize, stateAt, theme::faded(theme::kTextMuted, 0.8f), state);
    }
}

void App::drawTileLines(const Tiles& tiles, const TileRect& log, const TileRect& debug, float left) {
    // A line in the gap from `min` to `max` (`across`: a horizontal one); dragging it sets `share`, of `length` from
    // `start` (where the tiles it parts begin).
    const float gap = std::round(kTileGap * dpiScale_);
    const auto line = [&](const char* id, ImVec2 min, ImVec2 max, bool across, float start, float length,
                          float* share) {
        ImGui::SetCursorScreenPos(min);
        ImGui::InvisibleButton(id, ImVec2(max.x - min.x, max.y - min.y));
        const bool active = ImGui::IsItemActive();
        const bool hovered = ImGui::IsItemHovered();
        if (hovered || active) {
            ImGui::SetMouseCursor(across ? ImGuiMouseCursor_ResizeNS : ImGuiMouseCursor_ResizeEW);
        }
        if (active && length > 0.0f) {
            const float mouse = across ? ImGui::GetIO().MousePos.y : ImGui::GetIO().MousePos.x;
            *share = std::clamp((mouse - gap * 0.5f - start) / length, 0.15f, 0.85f);
        }
        const ImU32 colour = active ? theme::kAccent : hovered ? theme::kBorderHover : theme::kBorder;
        const float middle = std::round(across ? (min.y + max.y) * 0.5f : (min.x + max.x) * 0.5f);
        ImGui::GetWindowDrawList()->AddRectFilled(across ? ImVec2(min.x, middle) : ImVec2(middle, min.y),
                                                  across ? ImVec2(max.x, middle + 1.0f) : ImVec2(middle + 1.0f, max.y),
                                                  colour);
    };
    const TileRect& column = tiles.stats.shown() ? tiles.stats : tiles.state; // Right of Live.
    if (tiles.live.shown() && column.shown()) {
        line("##live-line", ImVec2(tiles.live.right, tiles.live.top), ImVec2(column.left, tiles.live.bottom), false,
             tiles.live.left, column.right - tiles.live.left, &liveSplit_);
    }
    if (tiles.stats.shown() && tiles.state.shown()) {
        line("##stats-line", ImVec2(tiles.stats.left, tiles.stats.bottom), ImVec2(tiles.stats.right, tiles.state.top),
             true, tiles.stats.top, tiles.state.bottom - tiles.stats.top, &statsSplit_);
    }
    if (log.shown() && (tiles.live.shown() || tiles.stats.shown() || tiles.state.shown())) {
        line("##log-line", ImVec2(log.left - gap, log.top), ImVec2(log.left, log.bottom), false, left,
             log.right - left, &logSplit_);
    }
    if (debug.shown() && (log.shown() || tiles.live.shown() || tiles.stats.shown() || tiles.state.shown())) {
        line("##debug-line", ImVec2(debug.left - gap, debug.top), ImVec2(debug.left, debug.bottom), false, left,
             debug.right - left, &debugSplit_);
    }
}

void App::drawLog() {
    // Clear at the top left (the user asked, 2026-11-13): empties the log and its file. Not for a clip's lines.
    BarButton clear;
    clear.text = "Clear";
    clear.enabled = !clipView_ && log_ && !log_->lines().empty();
    if (clear.draw("##clear-log", fonts_)) {
        log_->clear();
    }
    ImGui::Dummy(ImVec2(0.0f, std::round(6.0f * dpiScale_)));
    if (!clipView_ && log_ && !log_->error().empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, theme::kTextMuted);
        ImGui::TextUnformatted(("Not saved: " + log_->error()).c_str());
        ImGui::PopStyleColor();
    }
    // The lines scroll under it.
    ImGui::BeginChild("##log-lines", ImVec2(0.0f, 0.0f));
    if (clipView_) {
        clipLogView_.draw(clipView_->logLines(), clipView_->logLinesBy(), clipsOpened_, "Not recorded in this clip",
                          dpiScale_);
    } else if (log_) {
        logView_.draw(log_->lines(), log_->lines().size(), log_->added(), "Nothing logged yet", dpiScale_);
    }
    ImGui::EndChild();
}

void App::drawLive() {
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
                        settings_.showDepth, settings_.showDrops, dpiScale_);
    } else if (observed_) {
        drawObservedScreen();
    } else {
        drawChooseScreen();
    }
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
                            ImGui::IsPopupOpen(kHandmadePopup) || ImGui::IsPopupOpen(kClipsPopup) ||
                            ImGui::IsPopupOpen(kCategoryPopup);
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
        ImGui::SetTooltip("Answer what observex found in a training recording or your screenshots, or play a clip");
    }
    // The chooser and the lists hang under the button.
    const ImVec2 reviewCorner(ImGui::GetItemRectMax().x, ImGui::GetItemRectMax().y + popupGap);
    ImGui::SetNextWindowPos(reviewCorner, ImGuiCond_Always, ImVec2(1.0f, 0.0f));
    drawReviewChooser();
    if (openList_ != nullptr) { // Here, not inside the chooser's popup, so the list's name means the same popup.
        ImGui::OpenPopup(openList_);
        openList_ = nullptr;
    }
    ImGui::SetNextWindowPos(reviewCorner, ImGuiCond_Always, ImVec2(1.0f, 0.0f));
    drawRecordings(kRecordingsPopup, "Review a recording", recordings_, false);
    ImGui::SetNextWindowPos(reviewCorner, ImGuiCond_Always, ImVec2(1.0f, 0.0f));
    drawRecordings(kHandmadePopup, "Review your screenshots", handmade_, true);
    ImGui::SetNextWindowPos(reviewCorner, ImGuiCond_Always, ImVec2(1.0f, 0.0f));
    drawClips();
    if (openCategory_) { // Here, not inside the list's popup, so the chooser's name means the same popup.
        openCategory_ = false;
        ImGui::OpenPopup(kCategoryPopup);
    }
    drawCategoryChooser();
    // The brain's start/stop is in the State tile now (its Brain button; the user asked, 2026-11-15), not here.

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
        const ImVec2 showMin = ImGui::GetItemRectMin();
        const ImVec2 showMax = ImGui::GetItemRectMax();
        if (ImGui::BeginPopup(kShowPopup)) {
            drawShowFlags();
            ImGui::EndPopup();
        }
        // Right of it, what observex spends on a screen image in all (the user asked, 2026-11-12).
        std::string parts;
        char total[32];
        std::snprintf(total, sizeof(total), "%.1f ms", static_cast<double>(imageWorkMs(&parts)));
        const ImVec2 size = ImGui::CalcTextSize(total);
        const ImVec2 at(showMax.x + std::round(12.0f * dpiScale_), std::round((showMin.y + showMax.y - size.y) * 0.5f));
        ImGui::GetWindowDrawList()->AddText(at, theme::kTextMuted, total);
        if (ImGui::IsMouseHoveringRect(at, ImVec2(at.x + size.x, at.y + size.y)) && !parts.empty()) {
            ImGui::SetTooltip("%s ms on each screen image, over the last second", parts.c_str());
        }
    }

    // Over a clip instead: Guesses, whose guesses are drawn over it (the user asked, 2026-11-11, like Show).
    if (clipView_) {
        BarButton guessesButton;
        guessesButton.icon = Icon::Guesses;
        guessesButton.caption = "Guesses";
        guessesButton.text =
            !settings_.showCharacters && !settings_.showGround && !settings_.showDepth && !settings_.showDrops
                ? "Hidden"
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
        // While a screen is observed, its None tile (left of the screens) stops observing (the user asked,
        // 2026-11-12; a tile since 2026-11-15, the user asked: the line under the screens was hard to see).
        const ScreenPicker::Choice choice = picker_->draw(ImVec2(size.x, maxSize.y), theme::kSurface);
        if (choice.screen != nullptr) {
            chosen_ = *choice.screen;
            ImGui::CloseCurrentPopup();
        } else if (choice.none) {
            chosenNone_ = true;
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

    // Where the State tile saves its trees (.brain files) and loads them from (the user asked, 2026-11-12: saving
    // asks only for a name).
    ImGui::TextDisabled("Brains folder");
    std::string brains = toUtf8(settings_.brainsFolder.c_str());
    ImGui::SetNextItemWidth(width);
    ImGui::InputText("##brains-folder", brains.data(), brains.size() + 1, ImGuiInputTextFlags_ReadOnly);
    if (ImGui::Button("Change\xE2\x80\xA6##brains")) {
        if (const std::optional<std::filesystem::path> picked =
                pickFolder(window_->handle(), settings_.brainsFolder, L"Choose where state trees are saved")) {
            settings_.brainsFolder = *picked;
            saveSettings();
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Open##brains")) {
        showInExplorer(settings_.brainsFolder);
    }
    if (settings_.brainsFolder != defaultBrainsFolder()) {
        ImGui::SameLine();
        if (ImGui::Button("Use default##brains")) {
            settings_.brainsFolder = defaultBrainsFolder();
            saveSettings();
        }
    }
    ImGui::Dummy(ImVec2(0.0f, sectionGap));

    // The live log (engine/LiveLog): what the model sees, kept with each recording to look at afterwards. It
    // applies from the next recording.
    if (ImGui::Checkbox("Log what the model sees while recording", &settings_.liveLog)) {
        saveSettings();
    }

    // TEMPORARY DEBUG TEST (the user asked, 2026-11-15; to be removed). The "Mouse debug" sweep: clicked, it counts
    // down 3 s, then the Pico controller moves the real cursor onto each visible monster, resting 2 s on each, looping
    // until the mouse is moved. Remove this block with app/MouseTest.*, engine/PicoController.* and the
    // controller_/mouseTest_/mouseTestStatus_ members.
    ImGui::Dummy(ImVec2(0.0f, sectionGap));
    ImGui::TextDisabled("Mouse debug (test)");
    if (mouseTest_.active()) {
        if (ImGui::Button("Stop mouse debug")) {
            mouseTest_.stop();
            mouseTestStatus_.clear();
        }
    } else if (ImGui::Button("Mouse debug")) {
        mouseTestStatus_.clear();
        if (!observed_) {
            mouseTestStatus_ = "Observe the game screen first.";
        } else if (!controller_.open()) {
            mouseTestStatus_ = "Controller: " + controller_.error();
        } else if (const double ms = controller_.ping(); ms < 0.0) {
            mouseTestStatus_ =
                "Opened " + controller_.port() + ", but the controller did not answer. Is the firmware running?";
        } else {
            stopActing(); // The sweep and the brain both drive the controller: one at a time.
            mouseTest_.begin(controller_);
            char msg[96];
            std::snprintf(msg, sizeof(msg), "Linked on %s (ping %.1f ms).", controller_.port().c_str(), ms);
            mouseTestStatus_ = msg;
        }
    }
    if (const std::string& line = mouseTest_.active() ? mouseTest_.status() : mouseTestStatus_; !line.empty()) {
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + std::round(420.0f * dpiScale_));
        ImGui::TextDisabled("%s", line.c_str());
        ImGui::PopTextWrapPos();
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

    // Drops (the user asked, 2026-11-15): the spot looked at closely (round a kill, or drops the sweep found) and the
    // drops found there. Only while it cannot draw, why.
    if (ImGui::Checkbox("Drops", &settings_.showDrops)) {
        saveSettings();
        noteInClip(settings_.showDrops ? "show Drops on" : "show Drops off");
    }
    why[0] = '\0';
    if (!dropFinder_) {
        std::snprintf(why, sizeof(why), "%s",
                      dropFinderError_.empty() ? "Not looking: the screen is not captured." : dropFinderError_.c_str());
    } else if (dropFinder_->state() == DropFinder::State::Starting && dropFinder_->progress() > 0.0f) {
        std::snprintf(why, sizeof(why), "Preparing the drop model for this GPU: %.0f%%",
                      static_cast<double>(dropFinder_->progress() * 100.0f));
    } else if (dropFinder_->state() == DropFinder::State::Failed) {
        std::snprintf(why, sizeof(why), "%s", dropFinder_->error().c_str());
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
    // No words under Ground and Depth (the user asked, 2026-11-12).

    // Geodata (the user asked, 2026-11-15): the server's own map of the ground (its geodata) round the character, as a
    // grid fixed to the world; while observex is not sure where the character is, "Type /loc" in the middle.
    if (ImGui::Checkbox("Geodata", &settings_.showGeodata)) {
        saveSettings();
        noteInClip(settings_.showGeodata ? "show Geodata on" : "show Geodata off");
    }

    // Depth (the user asked, 2026-11-12): how near each pixel is, the depth network's guess, nearer brighter, over the
    // screen instead of it.
    if (ImGui::Checkbox("Depth", &settings_.showDepth)) {
        saveSettings();
        noteInClip(settings_.showDepth ? "show Depth on" : "show Depth off");
    }
}

void App::showCharacters(bool on) {
    settings_.showCharacters = on;
    saveSettings();
    noteInClip(std::string("show ") + kLive.name + (on ? " on" : " off"));
}

void App::pickGuesser(ClipView::Guesser guesser) {
    clipView_->setGuesser(guesser);
    if (!settings_.showCharacters && !settings_.showGround && !settings_.showDepth && !settings_.showDrops) {
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
        ImGui::BeginDisabled(
            (!settings_.showCharacters && !settings_.showGround && !settings_.showDepth && !settings_.showDrops) ||
            !missing.empty());
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
    // Depth (the user asked, 2026-11-12): the guesser's depth, Recorded's exactly as the app drew it live. No words
    // under Ground and Depth (the user asked, 2026-11-12).
    if (ImGui::Checkbox("Depth", &settings_.showDepth)) {
        saveSettings();
    }
    // Drops (the user asked, 2026-11-15: missing here): Recorded's as Show > Drops drew them live, Today's what
    // today's drop model finds on the frame shown. Only why there are none to draw.
    if (ImGui::Checkbox("Drops", &settings_.showDrops)) {
        saveSettings();
    }
    if (const std::string why = clipView_->dropsProblem(); settings_.showDrops && !why.empty()) {
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
    // The training screenshots taken by hand (the user asked, 2026-11-15), a package an hour.
    std::snprintf(description, sizeof(description), "%s %s  \xC2\xB7  your Shift+Win screenshots, to draw boxes on",
                  formatCount(handmade_.size()).c_str(), handmade_.size() == 1 ? "package" : "packages");
    if (choiceTile("handmade", "Handmade", description, tileSize, fonts_, dpi)) {
        openList_ = kHandmadePopup;
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

void App::drawRecordings(const char* popup, const char* title, const std::vector<RecordingRow>& rows,
                         bool handmade) {
    // The Review button's lists: every recording (or handmade package) in the recordings folder, newest first, each
    // with a picture of its first image (the user asked, 2026-11-02). Picking one asks what to look for in it next.
    if (!ImGui::BeginPopup(popup)) {
        return;
    }
    const float dpi = dpiScale_;
    const ImVec2 tileSize(std::round(520.0f * dpi), std::round(88.0f * dpi));
    ImGui::PushFont(fonts_.medium, 18.0f);
    ImGui::TextUnformatted(title);
    ImGui::PopFont();
    // Wrapped to the list, so a long folder path does not stretch the popup.
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + tileSize.x + ImGui::GetStyle().ScrollbarSize);
    ImGui::TextDisabled("%s", toUtf8(settings_.recordingsFolder.c_str()).c_str());
    ImGui::PopTextWrapPos();
    ImGui::Dummy(ImVec2(0.0f, 4.0f * dpi));

    if (rows.empty()) {
        ImGui::TextUnformatted(handmade ? "No screenshots yet." : "No recordings yet.");
        ImGui::TextDisabled(handmade ? "Observe a screen and press Shift+Win: each goes into the hour's package."
                                     : "Observe a screen and press Record.");
        ImGui::EndPopup();
        return;
    }
    const float spacing = ImGui::GetStyle().ItemSpacing.y;
    const float listHeight = std::min(static_cast<float>(rows.size()) * (tileSize.y + spacing) - spacing,
                                      std::round(ImGui::GetMainViewport()->Size.y * 0.6f));
    ImGui::BeginChild("##recordings", ImVec2(tileSize.x + ImGui::GetStyle().ScrollbarSize, listHeight));
    for (std::size_t i = 0; i < rows.size(); ++i) {
        const RecordingRow& row = rows[i];
        // Each row: a picture of its first image, when, then the details, then when the user last changed answers
        // in a review of it, if ever (the user asked, 2026-11-07). The details: how long, how many images, and how
        // far each category's review got: "00:34 · 12 images · Characters 11 of 12 reviewed".
        std::string detail;
        if (row.entry.info.seconds > 0) {
            char duration[16];
            formatDuration(duration, sizeof(duration), std::chrono::seconds(row.entry.info.seconds));
            detail = std::string(duration) + "  \xC2\xB7  ";
        }
        const char* noun = handmade ? (row.images == 1 ? " screenshot" : " screenshots")
                                    : (row.images == 1 ? " image" : " images");
        detail += std::to_string(row.images) + noun;
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
                reviewError_ = (handmade ? "No screenshots in the package " : "No images in the recording of ") +
                               row.when;
            } else {
                reviewFolder_ = row.entry.folder;
                reviewAbout_ = (handmade ? "Handmade " : "Recorded ") + row.when + "  \xC2\xB7  " +
                               std::to_string(row.images) + noun;
                openCategory_ = true;
            }
            ImGui::CloseCurrentPopup();
        }
    }
    ImGui::EndChild();
    ImGui::EndPopup();
}

void App::drawCategoryChooser() {
    // After the recording or handmade package: what to look for (characters or drops; more categories, more tiles).
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
    ImGui::TextDisabled("%s", reviewAbout_.c_str());
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
    const ScreenPicker::Choice choice = picker_->draw(ImVec2(maxSize.x, size.y), theme::kBackground);
    if (choice.screen != nullptr) {
        chosen_ = *choice.screen;
    } else if (choice.none) {
        chosenNone_ = true;
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
    // The depth instead of it (the flag Depth), the newest map; the interface, the ground and the characters over it
    // as over the screen.
    if (depthShowing_ && depthFinder_->state() == DepthFinder::State::Running) {
        if (const DepthSeen& seen = depthFinder_->seen(); seen.image > 0) {
            const auto width = static_cast<unsigned>(seen.width);
            const auto height = static_cast<unsigned>(seen.height);
            if (seen.image != depthUploaded_) {
                if (!depthImage_ || depthImage_->width() != width || depthImage_->height() != height) {
                    depthImage_ = std::make_unique<ScreenImage>(gpu_.device.Get(), width, height);
                }
                depthImage_->upload(seen.pixels.data(), width * 4);
                depthUploaded_ = seen.image;
            }
            depthImage_->draw(drawList, min, max, rounding, theme::kBackground);
            depthDrawn_ = seen.image;
        }
    }
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
    GroundCamera shownCamera; // Unknown until the ground finder has followed an image.
    if (groundFollowing_ && groundFinder_->state() == GroundFinder::State::Running) {
        if (const GroundSeen& seen = groundFinder_->seen(); seen.image > 0) {
            shownCamera = groundAt(seen.camera, previewMs_);
            // Turned on and tilted as the mouse's counts say from the image followed to the one shown: exact on every
            // frame, not along the follow's speeds.
            if (mouseCamera_ && shownCamera.known) {
                if (const MouseCamera::View now = mouseCamera_->at(previewMs_); now.known) {
                    const MouseCamera::View then = mouseCamera_->at(seen.camera.timeMs);
                    shownCamera.yaw = static_cast<float>(
                        std::remainder(static_cast<double>(seen.camera.yaw) + now.turned - then.turned,
                                       2.0 * 3.14159265358979323846));
                    if (now.pitchKnown) {
                        shownCamera.pitch = now.pitch;
                    }
                }
            }
            if (settings_.showGround) {
                groundDrawn_ = {true, previewImage_, seen.followed, shownCamera};
                drawGround(drawList, min, max, static_cast<float>(pipeline_.width()), shownCamera, dpiScale_);
            }
        }
    }
    // The geodata's ground (the flag Geodata), laid where the camera sees it (no camera: no grid, but what to do
    // still shows), the interface left clear.
    if (settings_.showGeodata && geodata_) {
        std::span<const UiPiece> pieces;
        if (uiFinder_ && uiFinder_->state() == UiFinder::State::Running) {
            const UiPieces& found = uiFinder_->found();
            pieces = {found.pieces.data(), static_cast<std::size_t>(found.count)};
        }
        geodata_->draw(drawList, fonts_, min, max, static_cast<float>(pipeline_.width()),
                       static_cast<float>(pipeline_.height()), shownCamera, pieces, previewMs_, dpiScale_);
    }
    if (settings_.showCharacters && recognizer_ && recognizer_->state() == Recognizer::State::Running) {
        drawFound(drawList, min, max);
    }
    if (settings_.showDrops) {
        drawDrops(drawList, min, max);
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
        char detail[1024] = {};
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
        if (dropFinder_ && dropFinder_->state() == DropFinder::State::Running) {
            const DropFinderStats& drops = dropFinder_->stats();
            add("%sDrops %.0f/s, %.2f ms each, %.2f slowest", length > 0 ? "  \xC2\xB7  " : "",
                static_cast<double>(drops.looksPerSecond), static_cast<double>(drops.workAvgMs),
                static_cast<double>(drops.workMaxMs));
            if (!loot_.square.empty()) {
                add(", close square %d px", loot_.square.side);
            }
        }
        if (groundFollowing_ && groundFinder_->state() == GroundFinder::State::Running) {
            const GroundFinderStats& ground = groundFinder_->stats();
            const GroundCamera& camera = groundFinder_->seen().camera;
            add("%sGround %.0f/s, %.2f ms each (fitting %.2f), fitted %.0f%% (searched %.0f%%), %.0f of %.0f patches "
                "agree (%.1f px off), pitch %.0f\xC2\xB0 distance %.1f, minimap %.0f%%, turned by it %.0f/s (%.2f ms), "
                "depth %.0f/s said the pitch %.0f%% (%.1f\xC2\xB0 off, %.2f ms), turns held the pitch %.0f%% (said "
                "it %.0f%%, %.2f ms)",
                length > 0 ? "  \xC2\xB7  " : "", static_cast<double>(ground.imagesPerSecond),
                static_cast<double>(ground.workAvgMs), static_cast<double>(ground.fitAvgMs),
                static_cast<double>(ground.fitted * 100.0f), static_cast<double>(ground.searched * 100.0f),
                static_cast<double>(ground.agreeing),
                static_cast<double>(ground.matches), static_cast<double>(ground.missPixels),
                static_cast<double>(camera.pitch * 57.29578f), static_cast<double>(camera.distance),
                static_cast<double>(ground.headed * 100.0f), static_cast<double>(ground.headsPerSecond),
                static_cast<double>(ground.headAvgMs), static_cast<double>(ground.depthPerSecond),
                static_cast<double>(ground.depthMeasured * 100.0f), static_cast<double>(ground.depthOffDegrees),
                static_cast<double>(ground.depthAvgMs), static_cast<double>(ground.turnsHeld * 100.0f),
                static_cast<double>(ground.turnsTaken * 100.0f), static_cast<double>(ground.turnsAvgMs));
        }
        if (depthRunning_ && depthFinder_->state() == DepthFinder::State::Running) { // While Depth or Ground is on.
            const DepthFinderStats& depth = depthFinder_->stats();
            add("%sDepth %.0f/s, network %.2f ms (%.2f slowest), shading %.2f, packing %.2f, %.0f kB a map, %.1f%% "
                "of the pixels changed",
                length > 0 ? "  \xC2\xB7  " : "", static_cast<double>(depth.mapsPerSecond),
                static_cast<double>(depth.networkAvgMs), static_cast<double>(depth.networkMaxMs),
                static_cast<double>(depth.shadeAvgMs), static_cast<double>(depth.packAvgMs),
                static_cast<double>(depth.packedKb), static_cast<double>(depth.changed * 100.0f));
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
    // And the depth finder's newest map: the camera's pitch by it (only while it runs: with its flags off its last map
    // would be old, and the ground finder may follow for the Camera card alone).
    std::shared_ptr<const DepthMade> depth;
    if (depthFinder_ && depthRunning_ && depthFinder_->state() == DepthFinder::State::Running) {
        depth = depthFinder_->made();
    }
    groundFinder_->hint(things, ownKind, pieces, std::move(depth));
}

} // namespace observex
