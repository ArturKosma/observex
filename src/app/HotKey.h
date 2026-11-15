#pragma once

#include <atomic>
#include <cstdint>
#include <thread>

namespace observex {

// A system-wide shortcut (Windows' RegisterHotKey), e.g. Win+Esc for the Brain (the user asked, 2026-11-15: the
// Escape keyboard hook never saw a key while the game was in front, as the game runs as administrator and Windows keeps
// a hook of a program that does not from its keys; a registered hot key reaches observex whatever is in front). Windows
// takes the keys from every program while it is registered. Its own thread waits for it; pressed() is true once after
// each press. One per key; Windows refuses a key another program registered (the constructor throws).
class HotKey {
public:
    // `modifiers`: MOD_WIN, MOD_CONTROL, ... ; `key`: a virtual key, e.g. VK_ESCAPE.
    HotKey(std::uint32_t modifiers, std::uint32_t key);
    ~HotKey();

    HotKey(const HotKey&) = delete;
    HotKey& operator=(const HotKey&) = delete;

    bool pressed() { return pressed_.exchange(false, std::memory_order_relaxed); }

private:
    std::atomic<bool> pressed_{false};
    std::atomic<std::uint32_t> threadId_{0};
    std::thread thread_;
};

} // namespace observex
