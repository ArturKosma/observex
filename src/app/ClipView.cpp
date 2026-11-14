#include "app/ClipView.h"

#include "app/Overlay.h"
#include "engine/ScreenCapture.h"

#include <imgui.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <exception>
#include <fstream>
#include <iterator>
#include <utility>

namespace observex {

namespace {

void centredText(const char* text, ImU32 color, float y) {
    const float width = ImGui::CalcTextSize(text).x;
    ImGui::SetCursorPos(ImVec2(std::round((ImGui::GetWindowWidth() - width) * 0.5f), std::round(y)));
    ImGui::PushStyleColor(ImGuiCol_Text, color);
    ImGui::TextUnformatted(text);
    ImGui::PopStyleColor();
}

// "0:03.2", as the bar shows the time.
void formatTime(char* out, std::size_t size, double seconds) {
    const double clamped = std::max(0.0, seconds);
    const int minutes = static_cast<int>(clamped / 60.0);
    std::snprintf(out, size, "%d:%04.1f", minutes, clamped - minutes * 60.0);
}

} // namespace

ClipView::ClipView(IDXGIAdapter1* gpu, ID3D11Device* device, std::filesystem::path folder, ClipInfo info,
                   Heuristics& today, std::unique_ptr<Snapping> snapping, std::unique_ptr<Grounding> ground,
                   std::unique_ptr<Depthing> depth, std::filesystem::path uiFolder,
                   std::shared_ptr<const StatsSource> stats)
    : folder_(std::move(folder)), info_(std::move(info)), uiFolder_(uiFolder), gpu_(gpu),
      grounding_(std::move(ground)), depthing_(std::move(depth)) {
    device->GetImmediateContext(&context_);
    try {
        player_ = std::make_unique<ClipPlayer>(gpu, folder_ / kClipVideo);
        reader_ = std::make_unique<ImageReader>(device, player_->frames());
        image_ = std::make_unique<ScreenImage>(device, player_->width(), player_->height());
    } catch (const std::exception& e) {
        image_.reset();
        reader_.reset(); // Before the player, whose channel it reads.
        player_.reset();
        openError_ = e.what();
    }
    if (player_) {
        try {
            uiImages_ = std::make_unique<ImageChannel>(device, player_->width(), player_->height(),
                                                       ScreenCapture::kImageFormat);
            uiFinder_ = std::make_unique<UiFinder>(gpu, *uiImages_, std::move(uiFolder), std::move(stats));
        } catch (const std::exception& e) {
            uiFinder_.reset(); // Before its channel.
            uiImages_.reset();
            uiError_ = std::string("The interface is not looked for: ") + e.what();
        }
    }

    // The numbers read from the game's interface while it was recorded.
    if (std::ifstream statsFile(folder_ / kClipStatsFile, std::ios::binary); statsFile) {
        const std::string statsText{std::istreambuf_iterator<char>(statsFile), std::istreambuf_iterator<char>()};
        recordedStats_ = readClipStats(statsText);
    }

    // The guesses: what the app showed then, and the same looks through today's heuristics.
    std::ifstream file(folder_ / kClipText, std::ios::binary);
    if (!file) {
        textError_ = "This clip has no clip.txt, so there is nothing to draw over it.";
        return;
    }
    const std::string text{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
    text_ = std::make_shared<const ClipText>(readClipText(text));
    recorded_ = recordedGuesses(*text_);
    recordedGrid_ = std::any_of(text_->grounds.begin(), text_->grounds.end(),
                                [](const ClipGroundLine& line) { return line.camera.known; });
    recordedDepth_ = std::any_of(text_->depths.begin(), text_->depths.end(),
                                 [](const ClipDepthLine& line) { return line.map != 0; });
    makeToday(today, std::move(snapping));
}

void ClipView::setHeuristics(Heuristics& today, std::unique_ptr<Snapping> snapping, std::unique_ptr<Grounding> ground,
                             std::unique_ptr<Depthing> depth, std::string about) {
    heuristicsNotice_ = "Today's heuristics loaded again: " + about;
    noticeUntil_ = std::chrono::steady_clock::now() + std::chrono::seconds(4);
    if (textError_.empty()) {
        makeToday(today, std::move(snapping));
    }
    // The ground followed again with them (the one under way, for heuristics before, stopped first), if it was.
    grounding_ = std::move(ground);
    if (groundAsked_) {
        ground_.reset();
        groundError_.clear();
        groundAsked_ = false;
        followGround();
    }
    // Today's depth shaded with them from the next frame drawn.
    depthing_ = std::move(depth);
    depthTodayError_.clear();
    depthStale_ = true;
}

const GameStats* ClipView::recordedStats() const {
    if (frame_ < 1 || frame_ > text_->frameImage.size()) {
        return nullptr;
    }
    const ClipStats* line = statsAt(recordedStats_, text_->frameImage[frame_ - 1]);
    return line != nullptr ? &line->stats : nullptr;
}

const GameStats* ClipView::todayStats() {
    if (!uiFinder_ || uiFinder_->state() != UiFinder::State::Running) {
        return nullptr;
    }
    const GameStats& stats = uiFinder_->gameStats();
    return stats.image > 0 ? &stats : nullptr;
}

std::string ClipView::statsProblem() {
    if (!uiFinder_) {
        return uiError_.empty() ? "The video cannot be played." : uiError_;
    }
    if (uiFinder_->state() == UiFinder::State::Failed) {
        return uiFinder_->error();
    }
    return uiFinder_->stats().statsProblem.data();
}

std::uint64_t ClipView::findMiniatures() {
    if (!uiFinder_ || uiFinder_->state() != UiFinder::State::Running) {
        return 0;
    }
    uiBehind_ = true; // The frame shown, looked at again: a paused one gets its answer too.
    return uiFinder_->findMiniatures();
}

const UiMiniatures* ClipView::miniatures() {
    if (!uiFinder_ || uiFinder_->state() != UiFinder::State::Running) {
        return nullptr;
    }
    return &uiFinder_->miniatures();
}

void ClipView::statsReloaded(const std::string& about) {
    heuristicsNotice_ = "Today's stats reading loaded again: " + about;
    noticeUntil_ = std::chrono::steady_clock::now() + std::chrono::seconds(4);
    uiBehind_ = true; // The frame shown, read again (by a new reader: the finder sees the DLL changed).
}

void ClipView::setSkills(std::vector<SkillToFind> skills) {
    if (uiFinder_) {
        uiFinder_->setSkills(std::move(skills));
        uiBehind_ = true;
    }
}

void ClipView::makeToday(Heuristics& today, std::unique_ptr<Snapping> snapping) {
    // The heuristics' own at once (a few milliseconds for a 10 s clip), then snapped in the background, once the one
    // under way (for heuristics before) has stopped.
    snapper_.reset();
    snapError_.clear();
    today_ = heuristicGuesses(*text_, today);
    if (!player_ || !snapping) {
        snapError_ =
            player_ ? "No snapping to snap them with." : "The video cannot be played, so they are not snapped.";
        return;
    }
    snapper_ = std::make_unique<ClipSnapper>(gpu_.Get(), folder_ / kClipVideo, text_, today_, std::move(snapping));
}

void ClipView::update() {
    if (snapper_ && snapper_->state() != ClipSnapper::State::Working) {
        if (snapper_->state() == ClipSnapper::State::Done) {
            today_ = snapper_->guesses();
        } else {
            snapError_ = "Could not snap them onto the video: " + snapper_->error();
        }
        snapper_.reset();
    }
    if (ground_ && ground_->state() != ClipGround::State::Working) {
        if (ground_->state() == ClipGround::State::Done) {
            groundCameras_ = ground_->cameras();
        } else {
            groundError_ = "Could not follow the ground over the video: " + ground_->error();
        }
        ground_.reset();
    }
    ImageStamp stamp;
    if (reader_ && reader_->copyNewImage(context_.Get(), image_->texture(), &stamp)) {
        image_->imageCopied();
        frame_ = stamp.number;
        if (frame_ == wanted_) {
            wanted_ = 0;
        }
        uiBehind_ = true;
    }
    // The frame shown to the UI finder, unless it had one in the last 1 / kImagesPerSecond seconds; then the one
    // shown when that time is up, so a paused frame always gets its own pieces.
    if (uiBehind_ && uiFinder_) {
        if (const auto now = std::chrono::steady_clock::now(); now >= nextUi_) {
            uiImages_->write(context_.Get(), image_->texture(), ImageStamp{frame_, 0}); // The full-size level.
            uiBehind_ = false;
            nextUi_ = now + std::chrono::milliseconds(1000 / UiFinder::kImagesPerSecond);
        }
    }
}

void ClipView::followGround() {
    // Once, the first time the flag Ground wants it, by what the app showed then (its own character's box and the
    // characters to leave out), as live.
    if (groundAsked_ || !player_ || !textError_.empty()) {
        return;
    }
    groundAsked_ = true;
    if (!grounding_) {
        groundError_ = "No following of the ground to follow it with.";
        return;
    }
    ground_ = std::make_unique<ClipGround>(gpu_.Get(), std::move(grounding_), folder_ / kClipVideo, text_, recorded_,
                                           uiFolder_);
}

std::string ClipView::groundProblem() const {
    if (!player_) {
        return {}; // draw() says why.
    }
    if (!textError_.empty()) {
        return guesser_ == Guesser::Recorded ? "This clip has no clip.txt, so nothing says what grid was drawn."
                                             : "The ground is followed by the character's box, and this clip has no "
                                               "clip.txt.";
    }
    if (guesser_ == Guesser::Recorded) {
        if (text_->grounds.empty()) {
            return "This clip was saved before clips kept the grid: Today's heuristics follow the ground over it.";
        }
        return recordedGrid_ ? std::string()
                             : "No grid was drawn while this clip was recorded (Ground was off, or the ground was not "
                               "found).";
    }
    return groundError_;
}

std::string ClipView::depthProblem() const {
    if (!player_) {
        return {}; // draw() says why.
    }
    if (!textError_.empty()) {
        return "This clip has no clip.txt, so nothing says what depth was drawn.";
    }
    if (!recordedDepth_) {
        return "No depth was drawn while this clip was recorded (Depth was off, or the clip was saved before clips "
               "kept the depth).";
    }
    if (!depthError_.empty()) {
        return depthError_;
    }
    if (guesser_ == Guesser::Today) {
        return depthing_ ? depthTodayError_ : std::string("No shading of depth to shade it with.");
    }
    return {};
}

void ClipView::loadDepth() {
    // Once, the first time the flag Depth wants it (a 10 s clip's maps are about 100 MB, read in a fraction of a
    // second).
    if (depthAsked_ || !recordedDepth_) {
        return;
    }
    depthAsked_ = true;
    std::string error;
    depths_ = readDepthFile(folder_ / kClipDepth, &error);
    if (depths_.empty()) {
        depthError_ = error.empty() ? std::string("depth.bin holds no maps.") : "Could not read depth.bin: " + error;
        return;
    }
    try {
        packer_ = std::make_unique<DepthPacker>();
    } catch (const std::exception& e) {
        depths_.clear();
        depthError_ = std::string("Could not unpack the depth: ") + e.what();
    }
}

void ClipView::drawDepth(ImDrawList* drawList, ImVec2 min, ImVec2 max, float rounding) {
    loadDepth();
    if (depths_.empty()) {
        return;
    }
    // The map the app drew live when the screen showed this frame's image.
    const std::uint64_t map = recordedDepth(text_->depths, frameMs(frame_));
    const auto record = std::lower_bound(depths_.begin(), depths_.end(), map,
                                         [](const DepthRecord& at, std::uint64_t image) { return at.image < image; });
    if (map == 0 || record == depths_.end() || record->image != map ||
        (guesser_ == Guesser::Today && (!depthing_ || !depthTodayError_.empty()))) {
        return;
    }
    if (map != depthUploaded_ || guesser_ != depthUploadedBy_ || (guesser_ == Guesser::Today && depthStale_)) {
        // Recorded: the levels and colours drawn then. Today: the map shaded again, the same interface left out.
        const std::size_t count = static_cast<std::size_t>(record->width) * static_cast<std::size_t>(record->height);
        std::array<std::uint32_t, Depthing::kPaletteSize> palette = record->palette;
        depthLevels_.resize(count);
        if (guesser_ == Guesser::Recorded) {
            if (!packer_->unpackLevels(*record, depthLevels_)) {
                depthError_ = "A depth map in depth.bin does not unpack.";
                depths_.clear();
                return;
            }
        } else {
            if (!packer_->unpackMap(*record, depthMap_)) {
                depthError_ = "A depth map in depth.bin does not unpack.";
                depths_.clear();
                return;
            }
            DepthInput input;
            input.map = depthMap_.data();
            input.width = record->width;
            input.height = record->height;
            input.screenWidth = record->screenWidth;
            input.screenHeight = record->screenHeight;
            input.ignore = record->ignore.data();
            input.ignoreCount = static_cast<int>(record->ignore.size());
            if (!depthing_->shade(input, depthLevels_.data(), palette.data())) {
                depthTodayError_ = "Today's heuristics could not shade the depth.";
                return;
            }
            depthStale_ = false;
        }
        depthPixels_.resize(count);
        paintDepth(depthLevels_.data(), palette.data(), count, depthPixels_.data());
        const auto width = static_cast<unsigned>(record->width);
        const auto height = static_cast<unsigned>(record->height);
        try {
            if (!depthImage_ || depthImage_->width() != width || depthImage_->height() != height) {
                ComPtr<ID3D11Device> device;
                context_->GetDevice(&device);
                depthImage_ = std::make_unique<ScreenImage>(device.Get(), width, height);
            }
        } catch (const std::exception& e) {
            depthError_ = std::string("Could not show the depth: ") + e.what();
            depths_.clear();
            return;
        }
        depthImage_->upload(depthPixels_.data(), width * 4);
        depthUploaded_ = map;
        depthUploadedBy_ = guesser_;
    }
    depthImage_->draw(drawList, min, max, rounding, theme::kBackground);
}

std::string ClipView::uiProblem() {
    if (!uiFinder_) {
        return uiError_; // Empty if the video did not open: draw() says why.
    }
    if (uiFinder_->state() == UiFinder::State::Failed) {
        return uiFinder_->error();
    }
    return uiFinder_->stats().problem.data(); // Found with too little.
}

bool ClipView::canPlay() const {
    return player_ && player_->state() != ClipPlayer::State::Failed;
}

bool ClipView::playing() const {
    return player_ && player_->state() == ClipPlayer::State::Playing;
}

bool ClipView::ended() const {
    return player_ && player_->state() == ClipPlayer::State::Ended;
}

void ClipView::togglePlay() {
    if (!canPlay()) {
        return;
    }
    if (playing()) {
        player_->pause();
    } else {
        player_->play();
    }
}

double ClipView::position() const {
    return player_ ? player_->position() : 0.0;
}

double ClipView::seconds() const {
    return player_ ? player_->seconds() : info_.seconds;
}

std::uint64_t ClipView::frames() const {
    return player_ ? player_->times().size() : 0;
}

void ClipView::seek(std::uint64_t number) {
    if (!canPlay() || frames() == 0) {
        return;
    }
    const std::uint64_t to = std::clamp<std::uint64_t>(number, 1, frames());
    // The player sends no frame for the one shown already (it only starts on from there).
    wanted_ = to == frame_ ? 0 : to;
    player_->seek(to);
}

void ClipView::step(int by) {
    if (!canPlay()) {
        return;
    }
    player_->pause();
    // From the frame shown, not one asked for that has not come: a held key then asks for the same frame until it
    // comes and goes on from there (a frame back takes 35-50 ms to decode, longer than the key repeats).
    const auto from = static_cast<long long>(std::max<std::uint64_t>(frame_, 1));
    seek(static_cast<std::uint64_t>(std::max(1LL, from + by)));
}

void ClipView::toFirst() {
    goTo(1);
}

void ClipView::toLast() {
    goTo(frames());
}

void ClipView::goTo(std::uint64_t number) {
    if (!canPlay()) {
        return;
    }
    player_->pause();
    seek(number);
}

std::span<const Tracked> ClipView::thingsShown() const {
    const Guesses* shown = guesses(guesser_);
    if (shown == nullptr || frame_ == 0) {
        return {};
    }
    return shown->at(frameMs(frame_));
}

std::span<const std::string> ClipView::kinds() const {
    if (!textError_.empty() || !text_) {
        return {};
    }
    return text_->kinds;
}

const Guesses* ClipView::guesses(Guesser guesser) const {
    if (!textError_.empty()) {
        return nullptr;
    }
    switch (guesser) {
    case Guesser::Recorded:
        return &recorded_;
    case Guesser::Today:
        return &today_;
    }
    return nullptr;
}

std::size_t ClipView::logLinesBy() const {
    // Made before the next frame came: by the last frame, all of them (the clip's own save too).
    const std::vector<double>& times = text_->logMs;
    const std::uint64_t next = frame_ + 1;
    if (next > frames()) {
        return times.size();
    }
    return static_cast<std::size_t>(std::lower_bound(times.begin(), times.end(), frameMs(next)) - times.begin());
}

double ClipView::frameMs(std::uint64_t number) const {
    if (number >= 1 && number <= text_->frameMs.size()) {
        return text_->frameMs[number - 1];
    }
    if (player_ && number >= 1 && number <= player_->times().size()) {
        return player_->times()[number - 1] * 1000.0;
    }
    return 0.0;
}

const std::string& ClipView::error() const {
    if (player_ && player_->state() == ClipPlayer::State::Failed) {
        return player_->error();
    }
    return openError_;
}

void ClipView::draw(const theme::Fonts& fonts, bool showCharacters, bool showKinds, bool showUi, bool showGround,
                    bool showDepth, float dpiScale) {
    if (showGround && guesser_ == Guesser::Today) {
        followGround();
    }
    if (canPlay() && !ImGui::GetIO().WantTextInput) {
        if (ImGui::IsKeyPressed(ImGuiKey_Space, false)) {
            togglePlay();
        }
        if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow, true)) {
            step(-1);
        }
        if (ImGui::IsKeyPressed(ImGuiKey_RightArrow, true)) {
            step(1);
        }
        if (ImGui::IsKeyPressed(ImGuiKey_Home, false)) {
            toFirst();
        }
        if (ImGui::IsKeyPressed(ImGuiKey_End, false)) {
            toLast();
        }
    }
    const float middle = ImGui::GetWindowHeight() * 0.5f;
    if (const std::string& why = error(); !why.empty()) {
        centredText("This clip cannot be played.", theme::kText, middle - ImGui::GetTextLineHeightWithSpacing());
        centredText(why.c_str(), theme::kTextMuted, middle);
        return;
    }
    if (!image_->hasImage()) {
        centredText("Loading the clip\xE2\x80\xA6", theme::kTextMuted, middle);
        return;
    }

    // The frame as large as fits above the timeline, keeping its shape, framed like the observed screen.
    const float timelineHeight = std::round(28.0f * dpiScale);
    const float timelineGap = std::round(6.0f * dpiScale);
    ImVec2 area = ImGui::GetContentRegionAvail();
    area.y = std::max(1.0f, area.y - timelineHeight - timelineGap);
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const float aspect = static_cast<float>(image_->width()) / static_cast<float>(image_->height());
    const ImVec2 size = area.x / area.y > aspect ? ImVec2(area.y * aspect, area.y) : ImVec2(area.x, area.x / aspect);
    const ImVec2 min(std::round(origin.x + (area.x - size.x) * 0.5f), std::round(origin.y + (area.y - size.y) * 0.5f));
    const ImVec2 max(min.x + std::round(size.x), min.y + std::round(size.y));
    const float rounding = 8.0f * ImGui::GetStyle().FontScaleDpi;
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    image_->draw(drawList, min, max, rounding, theme::kBackground);
    // The depth instead of the frame (the flag Depth), with the interface, the ground and the guesses over it as live.
    if (showDepth && frame_ > 0) {
        drawDepth(drawList, min, max, rounding);
    }
    drawList->PathRect(ImVec2(min.x - 0.5f, min.y - 0.5f), ImVec2(max.x + 0.5f, max.y + 0.5f), rounding + 0.5f);
    drawList->PathStroke(theme::kBorder, ImDrawFlags_Closed, 1.0f);

    // The game's interface the UI finder found, under the guesses as in the live view: on the frame shown, or one at
    // most a tenth of a second before it while playing.
    if (showUi && uiFinder_ && uiFinder_->state() == UiFinder::State::Running) {
        if (const UiPieces& pieces = uiFinder_->found(); pieces.image > 0) {
            drawUiPieces(drawList, fonts.medium, min, max, static_cast<float>(image_->width()),
                         {pieces.pieces.data(), static_cast<std::size_t>(pieces.count)}, dpiScale);
        }
    }
    // The ground: Recorded's as the app drew it live when the screen showed this frame's image; Today's as followed
    // over the clip (between the two followed images around this frame).
    if (showGround && frame_ > 0) {
        if (guesser_ == Guesser::Recorded) {
            drawGround(drawList, min, max, static_cast<float>(image_->width()),
                       recordedGround(text_->grounds, frameMs(frame_)), dpiScale);
        } else if (!groundCameras_.empty()) {
            drawGround(drawList, min, max, static_cast<float>(image_->width()),
                       groundAmong(groundCameras_, frameMs(frame_)), dpiScale);
        }
    }

    // What the guesser showed when the screen showed this frame's image.
    if (const Guesses* shown = guesses(guesser_); showCharacters && shown != nullptr && frame_ > 0) {
        drawThings(drawList, fonts.medium, min, max, static_cast<float>(image_->width()), shown->at(frameMs(frame_)),
                   text_->kinds, showKinds, dpiScale);
    }

    // New heuristics were loaded: said over the video's top left for a few seconds.
    if (!heuristicsNotice_.empty() && std::chrono::steady_clock::now() < noticeUntil_) {
        const ImVec2 padding(std::round(10.0f * dpiScale), std::round(6.0f * dpiScale));
        const ImVec2 at(min.x + std::round(12.0f * dpiScale), min.y + std::round(12.0f * dpiScale));
        const ImVec2 textSize = ImGui::CalcTextSize(heuristicsNotice_.c_str());
        drawList->AddRectFilled(at, ImVec2(at.x + textSize.x + padding.x * 2.0f, at.y + textSize.y + padding.y * 2.0f),
                                theme::kSurfaceActive, 6.0f * dpiScale);
        drawList->AddText(ImVec2(at.x + padding.x, at.y + padding.y), theme::kText, heuristicsNotice_.c_str());
    }

    const float top = max.y + timelineGap;
    drawTimeline(ImVec2(min.x, top), ImVec2(max.x, top + timelineHeight), dpiScale);
}

void ClipView::drawTimeline(ImVec2 min, ImVec2 max, float dpiScale) {
    if (!canPlay() || frames() == 0) {
        return;
    }
    const std::vector<double>& times = player_->times();
    const double length = std::max(times.back(), 0.001);
    const float knob = std::round(6.0f * dpiScale);
    const float left = min.x + knob;
    const float right = std::max(left + 1.0f, max.x - knob);
    const float line = std::round(max.y - 9.0f * dpiScale); // The track's middle; the notes' marks above it.
    const auto xOf = [&](double seconds) {
        return left + (right - left) * static_cast<float>(std::clamp(seconds / length, 0.0, 1.0));
    };
    // The frame nearest to `x`.
    const auto frameAt = [&](float x) {
        const double seconds = static_cast<double>(std::clamp((x - left) / (right - left), 0.0f, 1.0f)) * length;
        auto i = static_cast<std::size_t>(std::lower_bound(times.begin(), times.end(), seconds) - times.begin());
        if (i == times.size() || (i > 0 && seconds - times[i - 1] < times[i] - seconds)) {
            --i;
        }
        return static_cast<std::uint64_t>(i + 1);
    };

    ImGui::SetCursorScreenPos(min);
    ImGui::InvisibleButton("##timeline", ImVec2(max.x - min.x, max.y - min.y));
    const bool hovered = ImGui::IsItemHovered();
    const float mouseX = ImGui::GetIO().MousePos.x;
    if (ImGui::IsItemActivated()) { // Held down: paused while dragging, playing on after if it played.
        scrubbing_ = true;
        playAfterScrub_ = playing();
        scrubAt_ = 0;
        player_->pause();
    }
    if (ImGui::IsItemActive()) {
        // The frame under the mouse. The player finishes the one it decodes before it takes the latest asked, so the
        // picture follows the mouse a frame at a time (each frame decodes from its keyframe on: a clip has one every
        // 0.1 s, so at most about 20 ms a frame; older clips every 0.5 s, up to 90 ms).
        scrubAt_ = frameAt(mouseX);
        if (scrubAt_ != current()) {
            seek(scrubAt_);
        }
    }
    if (ImGui::IsItemDeactivated() && scrubbing_) {
        scrubbing_ = false;
        if (scrubAt_ != 0 && scrubAt_ != current()) { // Let go: the frame itself.
            seek(scrubAt_);
        }
        if (playAfterScrub_) {
            player_->play();
        }
    }

    ImDrawList* drawList = ImGui::GetWindowDrawList();
    const float half = std::round(2.0f * dpiScale);
    // The knob: under the mouse while dragging (the picture follows it a moment later), else at the frame shown.
    const std::uint64_t at = scrubbing_ && scrubAt_ != 0 ? scrubAt_ : current();
    const float x = at > 0 ? xOf(times[at - 1]) : left;
    drawList->AddRectFilled(ImVec2(left, line - half), ImVec2(right, line + half),
                            hovered || scrubbing_ ? theme::kBorderHover : theme::kBorder, half);
    drawList->AddRectFilled(ImVec2(left, line - half), ImVec2(x, line + half), theme::kAccent, half);

    // The app's notes (e.g. "show Kinds off", "clip saved: ..."): a mark each above the track, named on hover.
    const ClipNote* hoveredNote = nullptr;
    const float reach = 4.0f * dpiScale;
    for (const ClipNote& note : text_->notes) {
        const float noteX = std::round(xOf(note.timeMs / 1000.0));
        const bool pointed =
            hovered && !scrubbing_ && std::abs(mouseX - noteX) <= reach && ImGui::GetIO().MousePos.y < line - half;
        drawList->AddRectFilled(ImVec2(noteX - 1.0f * dpiScale, min.y + 2.0f * dpiScale),
                                ImVec2(noteX + 1.0f * dpiScale, line - half - 3.0f * dpiScale),
                                pointed ? theme::kText : theme::kTextMuted, 1.0f * dpiScale);
        if (pointed) {
            hoveredNote = &note;
        }
    }
    drawList->AddCircleFilled(ImVec2(x, line), scrubbing_ ? knob : knob - 1.0f * dpiScale, theme::kText);

    char time[16];
    if (hoveredNote != nullptr) {
        formatTime(time, sizeof(time), hoveredNote->timeMs / 1000.0);
        ImGui::SetTooltip("%s  %s", time, hoveredNote->text.c_str());
    } else if (hovered && !scrubbing_) {
        const std::uint64_t under = frameAt(mouseX);
        formatTime(time, sizeof(time), times[under - 1]);
        ImGui::SetTooltip("Frame %llu  \xC2\xB7  %s", static_cast<unsigned long long>(under), time);
    }
}

} // namespace observex
