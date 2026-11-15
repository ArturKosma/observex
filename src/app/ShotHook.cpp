#include "app/ShotHook.h"

#include "core/ShotKey.h"

#include <atomic>
#include <stdexcept>
#include <string>

#include <windows.h>

#include <objbase.h>
#include <shellapi.h>

namespace observex {

namespace {

ShotKey g_key;                       // Only the hook touches it, on the UI thread.
std::atomic<bool> g_pressed{false};

LRESULT CALLBACK shotHook(int code, WPARAM wParam, LPARAM lParam) {
    if (code == HC_ACTION) {
        const auto* kb = reinterpret_cast<const KBDLLHOOKSTRUCT*>(lParam);
        const bool down = wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN;
        const bool shift = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
        if (g_key.key(kb->vkCode, down, shift)) {
            g_pressed.store(true, std::memory_order_relaxed);
        }
    }
    return CallNextHookEx(nullptr, code, wParam, lParam); // Never taken: the game and Windows get every key.
}

} // namespace

ShotHook::ShotHook() {
    g_key = ShotKey();
    g_pressed.store(false, std::memory_order_relaxed);
    hook_ = SetWindowsHookExW(WH_KEYBOARD_LL, shotHook, GetModuleHandleW(nullptr), 0);
    if (hook_ == nullptr) {
        throw std::runtime_error("Windows refused the keyboard hook for Shift+Win");
    }
}

ShotHook::~ShotHook() {
    if (hook_ != nullptr) {
        UnhookWindowsHookEx(static_cast<HHOOK>(hook_));
    }
}

bool ShotHook::takePressed() {
    return g_pressed.exchange(false, std::memory_order_relaxed);
}

bool runsAsAdministrator() {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
        return false;
    }
    TOKEN_ELEVATION elevation{};
    DWORD size = 0;
    const bool elevated = GetTokenInformation(token, TokenElevation, &elevation, sizeof(elevation), &size) != FALSE &&
                          elevation.TokenIsElevated != 0;
    CloseHandle(token);
    return elevated;
}

bool restartAsAdministrator() {
    std::wstring path(32768, L'\0');
    const DWORD length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    if (length == 0 || length >= path.size()) {
        return false;
    }
    path.resize(length);
    // The arguments as given: the command line after the program's name (quoted or not), then kAsAdministrator.
    const wchar_t* given = GetCommandLineW();
    if (*given == L'"') {
        for (++given; *given != L'\0' && *given != L'"'; ++given) {
        }
        given += *given == L'"' ? 1 : 0;
    } else {
        for (; *given != L'\0' && *given != L' ' && *given != L'\t'; ++given) {
        }
    }
    const std::wstring arguments = std::wstring(given) + L" " + kAsAdministrator;
    std::wstring folder(32768, L'\0');
    const DWORD folderLength = GetCurrentDirectoryW(static_cast<DWORD>(folder.size()), folder.data());
    folder.resize(folderLength < folder.size() ? folderLength : 0);

    // COM for the shell, as ShellExecuteEx asks.
    const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    SHELLEXECUTEINFOW info{};
    info.cbSize = sizeof(info);
    info.fMask = SEE_MASK_NOASYNC; // This one ends at once after.
    info.lpVerb = L"runas";
    info.lpFile = path.c_str();
    info.lpParameters = arguments.c_str();
    info.lpDirectory = folder.empty() ? nullptr : folder.c_str(); // The same folder (else it would be System32).
    info.nShow = SW_SHOWNORMAL;
    const bool started = ShellExecuteExW(&info) != FALSE; // False also when the user says no.
    if (SUCCEEDED(com)) {
        CoUninitialize();
    }
    return started;
}

} // namespace observex
