#pragma once

namespace observex {

class Pipeline;
class PreviewReader;

// Shows what the pipeline sees: its state, speed numbers and a live preview of the captured screen.
class CapturePanel {
public:
    // `preview` may be null (no preview available). Sets `restartRequested` when the user asks for a restart.
    void draw(const char* title, bool* open, Pipeline& pipeline, PreviewReader* preview, bool& restartRequested);

    // Whether the last draw() showed the preview. The pipeline copies images for it only while this is true.
    bool wantsPreview() const { return visible_ && showPreview_; }

private:
    void drawStats(Pipeline& pipeline);
    void drawPreview(PreviewReader* preview);

    bool visible_ = false;
    bool showPreview_ = true;
};

} // namespace observex
