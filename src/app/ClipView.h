#pragma once

#include "app/ScreenImage.h"
#include "core/Recordings.h"
#include "engine/ClipPlayer.h"
#include "engine/Gpu.h"
#include "engine/ImageChannel.h"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>

namespace observex {

// The clip screen (the user asked, 2026-11-10): a saved clip (core/Recordings, engine/ClipRecorder), picked in the
// review's list of clips, plays in the window instead of the observed screen, as large as fits. For now it only plays
// the video; what the model saw on it comes later. Space plays and pauses; the bar's middle has the button, the time
// and the frame number.
class ClipView {
public:
    // Opens the clip in `folder` and starts playing it: decoded on `gpu` (the UI's GPU), shown with `device` (the UI's
    // device). Never throws: if the video cannot be played, draw() says why.
    ClipView(IDXGIAdapter1* gpu, ID3D11Device* device, std::filesystem::path folder, ClipInfo info);

    ClipView(const ClipView&) = delete;
    ClipView& operator=(const ClipView&) = delete;

    const std::filesystem::path& folder() const { return folder_; }
    const ClipInfo& info() const { return info_; }

    // Takes the newest frame into the image shown. Call on every frame, outside the ImGui frame.
    void update();

    // Whether the video plays: it opened and the player did not fail.
    bool canPlay() const;
    bool playing() const;
    bool ended() const;
    // Pauses while playing, else plays (from the first frame again once it ended).
    void togglePlay();
    // Seconds into the video of the frame shown, and the video's length.
    double position() const;
    double seconds() const;
    // The frame shown, numbered from 1 as in clip.txt; 0 before the first.
    std::uint64_t frame() const { return frame_; }

    // Draws the frame as large as fits into the rest of the current window, or why it cannot. Space plays and pauses.
    void draw();

private:
    // Why the video cannot be played (it did not open, or the player failed); empty while it can.
    const std::string& error() const;

    std::filesystem::path folder_;
    ClipInfo info_;
    ComPtr<ID3D11DeviceContext> context_;
    // Declared in this order so they go in the reverse one: the image, the reader of the player's frames, the player.
    std::unique_ptr<ClipPlayer> player_; // Null if the video did not open.
    std::unique_ptr<ImageReader> reader_;
    std::unique_ptr<ScreenImage> image_;
    std::string openError_;
    std::uint64_t frame_ = 0;
};

} // namespace observex
