#include "app/App.h"

#include <shellapi.h>
#include <windows.h>

#include <cwchar>
#include <optional>
#include <string>

namespace {

// observex.exe [--snapshot <file.png> [--click X Y]... [--mouse X Y]]
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
            snapshot->clicks.push_back(point(i + 1));
            i += 2;
        } else if (snapshot && arg == L"--mouse" && i + 2 < count) {
            snapshot->mouse = point(i + 1);
            i += 2;
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
