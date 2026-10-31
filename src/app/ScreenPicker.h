#pragma once

#include "app/ScreenImage.h"
#include "engine/Gpu.h"
#include "engine/ScreenCapture.h"

#include <imgui.h>

#include <memory>
#include <string>
#include <vector>

namespace observex {

// Live miniatures of every screen, laid out the way the screens are arranged on the desktop. Clicking
// one picks it. While shown, it captures the screens itself (on the UI thread, never waiting); the
// captures are freed as soon as it stops being drawn, so it costs nothing while hidden.
class ScreenPicker {
public:
    // `device` is the UI's GPU device; `gpu` the GPU whose screens are listed.
    ScreenPicker(ID3D11Device* device, IDXGIAdapter1* gpu);

    ScreenPicker(const ScreenPicker&) = delete;
    ScreenPicker& operator=(const ScreenPicker&) = delete;

    // The screen being observed (empty name for none). It is not captured again: its miniature shows
    // `image` (may be null). Call before fit() and draw() each frame.
    void setObserved(const std::string& name, const ScreenImage* image);

    // The size draw() will take for this `maxSize`.
    ImVec2 fit(ImVec2 maxSize);

    // Draws the miniatures at the cursor, centred in a `maxSize` area and as large as fits, over a
    // `background` coloured area. Returns the clicked screen, or nullptr. The pointer is valid until the
    // next call or close().
    const Screen* draw(ImVec2 maxSize, ImU32 background);

    // Call once per frame after the frame is presented: frees the captures if draw() was not called.
    void endFrame();

    // Frees the captures now.
    void close();

private:
    struct Miniature {
        Screen screen;
        std::unique_ptr<ScreenCapture> capture; // Null for the observed screen, or if it cannot be captured.
        std::unique_ptr<ScreenImage> image;
    };

    void open();
    void refresh(Miniature& miniature);
    RECT desktopArea() const; // The part of the desktop that all screens cover.

    ComPtr<ID3D11Device> device_;
    ComPtr<IDXGIAdapter1> gpu_;
    std::string observed_;
    const ScreenImage* observedImage_ = nullptr;
    std::vector<Miniature> miniatures_;
    bool open_ = false;
    bool drawn_ = false;
};

} // namespace observex
