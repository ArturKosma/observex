#include "app/App.h"

#include <shellapi.h>
#include <windows.h>

#include <algorithm>
#include <cwchar>
#include <cwctype>
#include <optional>
#include <string>

namespace {

// The ImGui key with this name, e.g. "Y", "RightArrow" or "Tab", ignoring case; ImGuiKey_None if there is none.
ImGuiKey keyNamed(const std::wstring& name) {
    for (int key = ImGuiKey_NamedKey_BEGIN; key < ImGuiKey_NamedKey_END; ++key) {
        const char* keyName = ImGui::GetKeyName(static_cast<ImGuiKey>(key));
        std::size_t i = 0;
        while (i < name.size() && keyName[i] != '\0' && std::towlower(name[i]) == std::towlower(keyName[i])) {
            ++i;
        }
        if (i == name.size() && keyName[i] == '\0') {
            return static_cast<ImGuiKey>(key);
        }
    }
    return ImGuiKey_None;
}

// observex.exe [--snapshot <file.png> [--click X Y | --drag X1 Y1 X2 Y2 | --key NAME]... [--mouse X Y]
//              [--recordings FOLDER] [--wait SECONDS]]
// The snapshot options draw the UI in a hidden window and save it; see App::Snapshot.
std::optional<observex::App::Snapshot> readSnapshotOptions() {
    int count = 0;
    wchar_t** args = CommandLineToArgvW(GetCommandLineW(), &count);
    if (args == nullptr) {
        return std::nullopt;
    }
    std::optional<observex::App::Snapshot> snapshot;
    const auto point = [&](int i) { return ImVec2(std::wcstof(args[i], nullptr), std::wcstof(args[i + 1], nullptr)); };
    for (int i = 1; i < count; ++i) {
        const std::wstring arg = args[i];
        if (arg == L"--snapshot" && i + 1 < count) {
            snapshot.emplace();
            snapshot->path = args[++i];
        } else if (snapshot && arg == L"--click" && i + 2 < count) {
            snapshot->steps.push_back({point(i + 1), point(i + 1)});
            i += 2;
        } else if (snapshot && arg == L"--drag" && i + 4 < count) {
            snapshot->steps.push_back({point(i + 1), point(i + 3)});
            i += 4;
        } else if (snapshot && arg == L"--key" && i + 1 < count) {
            snapshot->steps.push_back({ImVec2(), ImVec2(), keyNamed(args[++i])});
        } else if (snapshot && arg == L"--recordings" && i + 1 < count) {
            snapshot->recordingsFolder = args[++i];
        } else if (snapshot && arg == L"--mouse" && i + 2 < count) {
            snapshot->mouse = point(i + 1);
            i += 2;
        } else if (snapshot && arg == L"--wait" && i + 1 < count) {
            snapshot->wait = std::max(0.0f, std::wcstof(args[++i], nullptr));
        }
    }
    LocalFree(args);
    return snapshot;
}

} // namespace

int main() {
    observex::App app(readSnapshotOptions());
    return app.run();
}
