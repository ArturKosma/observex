#include "app/PicoLink.h"

#include <cwchar>
#include <string>

#include <windows.h>

#include <cfgmgr32.h>

namespace observex {
namespace {

// Is any present USB device a Raspberry Pi (vendor id 0x2E8A)? The Config Manager hands back every present USB
// device's instance id (a double-null-terminated list of "USB\VID_xxxx&PID_yyyy\serial" strings); we look for
// "VID_2E8A" in one of them. Enumerating the whole USB tree and matching the substring is reliable, where a partial
// "USB\VID_2E8A" filter is not. No device-info set to open or free.
bool picoPresent() {
    const wchar_t* enumerator = L"USB";
    const ULONG flags = CM_GETIDLIST_FILTER_ENUMERATOR | CM_GETIDLIST_FILTER_PRESENT;

    ULONG length = 0;
    if (CM_Get_Device_ID_List_SizeW(&length, enumerator, flags) != CR_SUCCESS || length <= 1) {
        return false; // 1 is just the empty list's trailing null.
    }
    std::wstring buffer(length, L'\0');
    if (CM_Get_Device_ID_ListW(enumerator, buffer.data(), length, flags) != CR_SUCCESS) {
        return false;
    }
    // Walk the double-null-terminated list; each id is its own string. Raspberry Pi's vendor id, upper case as the
    // Config Manager writes it.
    for (const wchar_t* id = buffer.c_str(); *id != L'\0'; id += std::wcslen(id) + 1) {
        if (std::wcsstr(id, L"VID_2E8A") != nullptr) {
            return true;
        }
    }
    return false;
}

} // namespace

bool PicoLink::connected() {
    const auto now = std::chrono::steady_clock::now();
    if (!everChecked_ || now - checkedAt_ >= std::chrono::seconds(1)) {
        connected_ = picoPresent();
        checkedAt_ = now;
        everChecked_ = true;
    }
    return connected_;
}

} // namespace observex
