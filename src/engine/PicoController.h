#pragma once

#include <cstdint>
#include <string>

namespace observex {

// The PC side of the link to the observex controller (the Raspberry Pi Pico running the firmware in
// C:\_programming\pico\observex-controller). The Pico shows up as a USB mouse + keyboard and a CDC serial port, and it
// moves the real cursor or presses keys only when sent a command here, so a game sees input as coming from real USB
// hardware, not from software injection. This finds the controller's COM port (by its USB id VID_2E8A&PID_4F58), opens
// it, and sends the 9-byte command packets the firmware expects (see the firmware's main.c for the protocol). One
// writer at a time; used from the UI thread in observex today.
class PicoController {
public:
    PicoController() = default;
    ~PicoController();

    PicoController(const PicoController&) = delete;
    PicoController& operator=(const PicoController&) = delete;

    // Finds and opens the controller's serial port (an already-open link counts as success). False if it cannot;
    // error() then says why.
    bool open();
    void close();
    bool isOpen() const { return handle_ != nullptr; }
    const std::string& port() const { return port_; }   // "COM3", once open; empty otherwise.
    const std::string& error() const { return error_; }

    // Moves the cursor by (dx, dy) screen pixels (relative), holding `buttons` (bit 0 left, 1 right, 2 middle) and
    // turning the wheel by `wheel`. A large move is split into 127-pixel HID steps by the firmware. False if the write
    // failed (the link is then taken as lost and closed).
    bool moveMouse(int dx, int dy, std::uint8_t buttons = 0, int wheel = 0);
    // Holds `modifier` (HID modifier bits) and up to 6 HID key codes (`count` 0 releases every key). False as above.
    bool pressKeys(std::uint8_t modifier, const std::uint8_t* keys, int count);
    // Sends a ping and waits up to `timeoutMs` for the firmware's one-byte echo: the round trip in milliseconds, or -1
    // if nothing came back (so the caller can tell a live firmware from a port that merely opened).
    double ping(int timeoutMs = 300);
    // Asks the firmware to reboot into its bootloader (for reflashing); the port then goes away, so this closes it.
    bool rebootToBootloader();

private:
    bool send(const std::uint8_t packet[9]);

    void* handle_ = nullptr; // A Win32 HANDLE while open, null when closed (kept as void* so no windows.h here).
    std::string port_;
    std::string error_;
};

} // namespace observex
