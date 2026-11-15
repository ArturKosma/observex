#pragma once

#include <chrono>

namespace observex {

// Tells whether the Raspberry Pi Pico (the reactor's USB input device, see docs/training-loop.md) is plugged in, so
// the top bar can show a green/red light for it. "Connected" means any present USB device with Raspberry Pi's vendor
// id (0x2E8A) is on the machine, whether it is in the BOOTSEL bootloader or running the firmware. It asks Windows at
// most once a second (Config Manager) and caches the answer, so the UI can call it every frame for free.
class PicoLink {
public:
    bool connected();

private:
    bool connected_ = false;
    std::chrono::steady_clock::time_point checkedAt_{}; // Default (epoch): never checked yet.
    bool everChecked_ = false;
};

} // namespace observex
