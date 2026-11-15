#pragma once

namespace observex {

// The training screenshot shortcut, Shift+Win (the user asked, 2026-11-15: a key Lineage 2 does not take, shown in the
// Log; Shift+Win opens nothing in Windows). Fed every key going down or up, system-wide (app/ShotHook): a screenshot
// is due when the Windows key comes up after it was down with Shift held (pressed before or after it) and no other
// key, so Windows' own shortcuts (Win+Shift+S, Ctrl+Shift+Win+B, ...) take none. Nothing is taken from anyone.
class ShotKey {
public:
    static constexpr unsigned kLeftWin = 0x5B; // Windows' virtual-key codes.
    static constexpr unsigned kRightWin = 0x5C;
    static constexpr unsigned kShift = 0x10;
    static constexpr unsigned kLeftShift = 0xA0;
    static constexpr unsigned kRightShift = 0xA1;

    // `vk` went down (or up); `shiftHeld` whether a Shift key is held now. True: take a screenshot.
    bool key(unsigned vk, bool down, bool shiftHeld) {
        if (vk == kLeftWin || vk == kRightWin) {
            if (down) {
                if (!winDown_) { // Held down, it repeats: only the first counts.
                    winDown_ = true;
                    shifted_ = shiftHeld;
                    other_ = false;
                }
                return false;
            }
            const bool shot = winDown_ && shifted_ && !other_;
            winDown_ = false;
            return shot;
        }
        if (winDown_ && down) {
            if (vk == kShift || vk == kLeftShift || vk == kRightShift) {
                shifted_ = true;
            } else {
                other_ = true;
            }
        }
        return false;
    }

private:
    bool winDown_ = false;
    bool shifted_ = false; // Shift held with it.
    bool other_ = false;   // Another key pressed while it was down.
};

} // namespace observex
