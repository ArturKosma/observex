#include "app/HotKey.h"

#include <future>
#include <stdexcept>
#include <string>

#include <windows.h>

namespace observex {

HotKey::HotKey(std::uint32_t modifiers, std::uint32_t key) {
    std::promise<DWORD> registered; // 0: it is registered; else Windows' error.
    std::future<DWORD> result = registered.get_future();
    thread_ = std::thread([this, modifiers, key, &registered] {
        MSG message;
        PeekMessageW(&message, nullptr, WM_USER, WM_USER, PM_NOREMOVE); // This thread's queue, for WM_QUIT.
        threadId_.store(GetCurrentThreadId(), std::memory_order_release);
        if (!RegisterHotKey(nullptr, 1, modifiers | MOD_NOREPEAT, key)) {
            registered.set_value(GetLastError() != 0 ? GetLastError() : 1);
            return;
        }
        registered.set_value(0);
        while (GetMessageW(&message, nullptr, 0, 0) > 0) {
            if (message.message == WM_HOTKEY) {
                pressed_.store(true, std::memory_order_relaxed);
            }
        }
        UnregisterHotKey(nullptr, 1);
    });
    const DWORD error = result.get();
    if (error != 0) {
        thread_.join();
        throw std::runtime_error(error == ERROR_HOTKEY_ALREADY_REGISTERED
                                     ? std::string("another program has that shortcut")
                                     : "Windows refused it (error " + std::to_string(error) + ")");
    }
}

HotKey::~HotKey() {
    if (thread_.joinable()) {
        PostThreadMessageW(threadId_.load(std::memory_order_acquire), WM_QUIT, 0, 0);
        thread_.join();
    }
}

} // namespace observex
