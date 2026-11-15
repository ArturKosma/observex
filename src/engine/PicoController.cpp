#include "engine/PicoController.h"

#include <windows.h>
#include <setupapi.h>
#include <devguid.h>

#include <algorithm>
#include <chrono>
#include <cwctype>
#include <cwchar>

namespace observex {
namespace {

constexpr std::uint8_t kSync = 0xAB; // Starts every command packet; the firmware re-aligns on it.

std::string lastError() {
    const DWORD code = GetLastError();
    if (code == 0) {
        return {};
    }
    char* buffer = nullptr;
    const DWORD n = FormatMessageA(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                                       FORMAT_MESSAGE_IGNORE_INSERTS,
                                   nullptr, code, 0, reinterpret_cast<char*>(&buffer), 0, nullptr);
    std::string text = n != 0 && buffer != nullptr ? std::string(buffer, n) : ("error " + std::to_string(code));
    if (buffer != nullptr) {
        LocalFree(buffer);
    }
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r' || text.back() == '.' || text.back() == ' ')) {
        text.pop_back();
    }
    return text;
}

// The COM port name ("COM3") of a present serial device whose instance id holds `wantExact` (e.g. "VID_2E8A&PID_4F58");
// if none has it, the first whose id holds `wantVid` ("VID_2E8A"); empty if neither is present.
std::string findPort(const wchar_t* wantExact, const wchar_t* wantVid) {
    const HDEVINFO info = SetupDiGetClassDevsW(&GUID_DEVCLASS_PORTS, nullptr, nullptr, DIGCF_PRESENT);
    if (info == INVALID_HANDLE_VALUE) {
        return {};
    }
    std::string exact;
    std::string anyVid;
    SP_DEVINFO_DATA dev{};
    dev.cbSize = sizeof(dev);
    for (DWORD i = 0; SetupDiEnumDeviceInfo(info, i, &dev) != FALSE; ++i) {
        wchar_t id[512] = {};
        if (SetupDiGetDeviceInstanceIdW(info, &dev, id, 512, nullptr) == FALSE) {
            continue;
        }
        for (wchar_t* p = id; *p != L'\0'; ++p) {
            *p = static_cast<wchar_t>(std::towupper(*p));
        }
        const bool hasExact = std::wcsstr(id, wantExact) != nullptr;
        const bool hasVid = std::wcsstr(id, wantVid) != nullptr;
        if (!hasExact && !hasVid) {
            continue;
        }
        const HKEY key = SetupDiOpenDevRegKey(info, &dev, DICS_FLAG_GLOBAL, 0, DIREG_DEV, KEY_READ);
        if (key == INVALID_HANDLE_VALUE) {
            continue;
        }
        wchar_t name[32] = {};
        DWORD type = 0;
        DWORD size = sizeof(name);
        const LONG read = RegQueryValueExW(key, L"PortName", nullptr, &type, reinterpret_cast<BYTE*>(name), &size);
        RegCloseKey(key);
        if (read != ERROR_SUCCESS || type != REG_SZ) {
            continue;
        }
        std::string ascii;
        for (int j = 0; name[j] != L'\0' && j < 31; ++j) {
            ascii.push_back(static_cast<char>(name[j]));
        }
        if (hasExact && exact.empty()) {
            exact = ascii;
        } else if (hasVid && anyVid.empty()) {
            anyVid = ascii;
        }
    }
    SetupDiDestroyDeviceInfoList(info);
    return !exact.empty() ? exact : anyVid;
}

} // namespace

PicoController::~PicoController() {
    close();
}

bool PicoController::open() {
    if (isOpen()) {
        return true;
    }
    error_.clear();
    const std::string port = findPort(L"VID_2E8A&PID_4F58", L"VID_2E8A");
    if (port.empty()) {
        error_ = "no controller serial port found (is the Pico plugged in and running the firmware?)";
        return false;
    }
    const std::string path = "\\\\.\\" + port; // The device path form, needed for COM10 and up.
    const HANDLE handle =
        CreateFileA(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        error_ = port + ": " + lastError();
        return false;
    }
    DCB dcb{};
    dcb.DCBlength = sizeof(dcb);
    GetCommState(handle, &dcb);
    dcb.BaudRate = CBR_115200; // A USB CDC port ignores the baud, but a valid one is still needed.
    dcb.ByteSize = 8;
    dcb.Parity = NOPARITY;
    dcb.StopBits = ONESTOPBIT;
    dcb.fBinary = TRUE;
    dcb.fParity = FALSE;
    dcb.fOutxCtsFlow = FALSE;
    dcb.fOutxDsrFlow = FALSE;
    dcb.fDsrSensitivity = FALSE;
    dcb.fDtrControl = DTR_CONTROL_ENABLE; // TinyUSB's CDC counts as connected only once DTR is asserted.
    dcb.fRtsControl = RTS_CONTROL_ENABLE;
    SetCommState(handle, &dcb);
    COMMTIMEOUTS timeouts{};
    timeouts.ReadIntervalTimeout = 50;
    timeouts.ReadTotalTimeoutConstant = 100;
    timeouts.ReadTotalTimeoutMultiplier = 0;
    timeouts.WriteTotalTimeoutConstant = 200;
    timeouts.WriteTotalTimeoutMultiplier = 0;
    SetCommTimeouts(handle, &timeouts);
    PurgeComm(handle, PURGE_RXCLEAR | PURGE_TXCLEAR);
    handle_ = handle;
    port_ = port;
    return true;
}

void PicoController::close() {
    if (isOpen()) {
        CloseHandle(static_cast<HANDLE>(handle_));
    }
    handle_ = nullptr;
    port_.clear();
}

bool PicoController::send(const std::uint8_t packet[9]) {
    if (!isOpen()) {
        error_ = "not open";
        return false;
    }
    DWORD written = 0;
    if (WriteFile(static_cast<HANDLE>(handle_), packet, 9, &written, nullptr) == FALSE || written != 9) {
        error_ = "write failed: " + lastError();
        close(); // A failed write means the link is gone (the Pico was unplugged); reopen() to try again.
        return false;
    }
    return true;
}

bool PicoController::moveMouse(int dx, int dy, std::uint8_t buttons, int wheel) {
    dx = std::clamp(dx, -32767, 32767);
    dy = std::clamp(dy, -32767, 32767);
    const int w = std::clamp(wheel, -127, 127);
    const auto ux = static_cast<std::uint16_t>(static_cast<std::int16_t>(dx));
    const auto uy = static_cast<std::uint16_t>(static_cast<std::int16_t>(dy));
    const std::uint8_t packet[9] = {kSync,
                                    'M',
                                    static_cast<std::uint8_t>(ux & 0xFF),
                                    static_cast<std::uint8_t>(ux >> 8),
                                    static_cast<std::uint8_t>(uy & 0xFF),
                                    static_cast<std::uint8_t>(uy >> 8),
                                    buttons,
                                    static_cast<std::uint8_t>(static_cast<std::int8_t>(w)),
                                    0};
    return send(packet);
}

bool PicoController::pressKeys(std::uint8_t modifier, const std::uint8_t* keys, int count) {
    std::uint8_t packet[9] = {kSync, 'K', modifier, 0, 0, 0, 0, 0, 0};
    for (int i = 0; i < 6 && i < count && keys != nullptr; ++i) {
        packet[3 + i] = keys[i];
    }
    return send(packet);
}

double PicoController::ping(int timeoutMs) {
    if (!isOpen()) {
        return -1.0;
    }
    const HANDLE handle = static_cast<HANDLE>(handle_);
    PurgeComm(handle, PURGE_RXCLEAR);
    COMMTIMEOUTS wait{};
    wait.ReadIntervalTimeout = 0;
    wait.ReadTotalTimeoutConstant = static_cast<DWORD>(std::max(1, timeoutMs));
    wait.ReadTotalTimeoutMultiplier = 0;
    wait.WriteTotalTimeoutConstant = 200;
    SetCommTimeouts(handle, &wait);
    const std::uint8_t packet[9] = {kSync, 'P', 0, 0, 0, 0, 0, 0, 0};
    const auto start = std::chrono::steady_clock::now();
    double round = -1.0;
    if (send(packet)) {
        char got = 0;
        DWORD read = 0;
        if (ReadFile(handle, &got, 1, &read, nullptr) != FALSE && read == 1 && got == 'P') {
            const auto end = std::chrono::steady_clock::now();
            round = std::chrono::duration<double, std::milli>(end - start).count();
        }
    }
    COMMTIMEOUTS back{};
    back.ReadIntervalTimeout = 50;
    back.ReadTotalTimeoutConstant = 100;
    back.ReadTotalTimeoutMultiplier = 0;
    back.WriteTotalTimeoutConstant = 200;
    if (isOpen()) {
        SetCommTimeouts(handle, &back);
    }
    return round;
}

bool PicoController::rebootToBootloader() {
    const std::uint8_t packet[9] = {kSync, 'R', 0, 0, 0, 0, 0, 0, 0};
    const bool ok = send(packet);
    close();
    return ok;
}

} // namespace observex
