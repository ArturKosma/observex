#include "app/Settings.h"

#include <shellapi.h>
#include <shobjidl.h>
#include <wrl/client.h>

#include <fstream>
#include <string>
#include <string_view>
#include <system_error>

namespace observex {

namespace {

using Microsoft::WRL::ComPtr;

std::filesystem::path fromUtf8(std::string_view text) {
    return std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(text.data()), text.size()));
}

std::string toUtf8(const std::filesystem::path& path) {
    const std::u8string text = path.u8string();
    return std::string(reinterpret_cast<const char*>(text.data()), text.size());
}

} // namespace

Settings Settings::load(const std::filesystem::path& file) {
    Settings settings;
    settings.recordingsFolder = defaultRecordingsFolder();
    settings.brainsFolder = defaultBrainsFolder();

    std::ifstream in(file, std::ios::binary);
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        const std::size_t equals = line.find('=');
        if (equals == std::string::npos) {
            continue;
        }
        const std::string_view key(line.data(), equals);
        const std::string_view value(line.data() + equals + 1, line.size() - equals - 1);
        if (key == "recordings" && !value.empty()) {
            settings.recordingsFolder = fromUtf8(value);
        } else if (key == "brains" && !value.empty()) {
            settings.brainsFolder = fromUtf8(value);
        } else if (key == "brain") {
            settings.brainName = value;
        } else if (key == "show-characters") {
            settings.showCharacters = value != "0";
        } else if (key == "show-kinds") {
            settings.showKinds = value != "0";
        } else if (key == "show-ui") {
            settings.showUi = value != "0";
        } else if (key == "show-live") {
            settings.showLive = value != "0";
        } else if (key == "show-stats") {
            settings.showGameStats = value != "0";
        } else if (key == "show-state") {
            settings.showState = value != "0";
        } else if (key == "show-skills") {
            settings.showSkills = value != "0";
        } else if (key == "show-log") {
            settings.showLog = value != "0";
        } else if (key == "show-ground") {
            settings.showGround = value != "0";
        } else if (key == "show-depth") {
            settings.showDepth = value != "0";
        } else if (key == "live-log") {
            settings.liveLog = value != "0";
        }
    }
    return settings;
}

bool Settings::save(const std::filesystem::path& file) const {
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    out << "recordings=" << toUtf8(recordingsFolder) << "\r\n";
    out << "show-characters=" << (showCharacters ? 1 : 0) << "\r\n";
    out << "show-kinds=" << (showKinds ? 1 : 0) << "\r\n";
    out << "show-ui=" << (showUi ? 1 : 0) << "\r\n";
    out << "show-live=" << (showLive ? 1 : 0) << "\r\n";
    out << "show-stats=" << (showGameStats ? 1 : 0) << "\r\n";
    out << "show-state=" << (showState ? 1 : 0) << "\r\n";
    out << "show-skills=" << (showSkills ? 1 : 0) << "\r\n";
    out << "show-log=" << (showLog ? 1 : 0) << "\r\n";
    out << "show-ground=" << (showGround ? 1 : 0) << "\r\n";
    out << "show-depth=" << (showDepth ? 1 : 0) << "\r\n";
    out << "live-log=" << (liveLog ? 1 : 0) << "\r\n";
    return static_cast<bool>(out);
}

std::filesystem::path appFolder() {
    std::wstring path(MAX_PATH, L'\0');
    for (;;) {
        const DWORD length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
        if (length < path.size()) {
            path.resize(length);
            break;
        }
        path.resize(path.size() * 2); // Longer than MAX_PATH.
    }
    return std::filesystem::path(path).parent_path();
}

std::filesystem::path projectFolder() {
    std::error_code error;
    for (std::filesystem::path folder = appFolder(); !folder.empty(); folder = folder.parent_path()) {
        if (std::filesystem::exists(folder / L"tools" / L"train" / L"guess.py", error)) {
            return folder;
        }
        if (folder == folder.parent_path()) {
            break;
        }
    }
    return {};
}

std::filesystem::path modelFolder(const char* category) {
    const std::filesystem::path project = projectFolder();
    return (project.empty() ? appFolder() : project) / L"models" / std::filesystem::path(category);
}

std::filesystem::path defaultRecordingsFolder() {
    return appFolder() / L"recordings";
}

std::filesystem::path defaultBrainsFolder() {
    return appFolder() / L"brains";
}

std::optional<std::filesystem::path> pickFolder(HWND owner, const std::filesystem::path& start, const wchar_t* title) {
    const bool com = SUCCEEDED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE));
    std::optional<std::filesystem::path> chosen;
    {
        ComPtr<IFileOpenDialog> dialog;
        if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog)))) {
            FILEOPENDIALOGOPTIONS options = 0;
            dialog->GetOptions(&options);
            dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
            dialog->SetTitle(title);
            std::error_code error;
            std::filesystem::create_directories(start, error); // So the picker can open in it.
            ComPtr<IShellItem> startItem;
            if (SUCCEEDED(SHCreateItemFromParsingName(start.c_str(), nullptr, IID_PPV_ARGS(&startItem)))) {
                dialog->SetFolder(startItem.Get());
            }
            ComPtr<IShellItem> result;
            PWSTR path = nullptr;
            if (SUCCEEDED(dialog->Show(owner)) && SUCCEEDED(dialog->GetResult(&result)) &&
                SUCCEEDED(result->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
                chosen = std::filesystem::path(path);
                CoTaskMemFree(path);
            }
        }
    }
    if (com) {
        CoUninitialize();
    }
    return chosen;
}

void showInExplorer(const std::filesystem::path& folder) {
    std::error_code error;
    std::filesystem::create_directories(folder, error);
    ShellExecuteW(nullptr, L"open", folder.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

} // namespace observex
