#include "app/Finder.h"

#include "app/Settings.h"

#include <windows.h>

#include <algorithm>
#include <charconv>
#include <stdexcept>
#include <system_error>

namespace observex {

namespace {

namespace fs = std::filesystem;

fs::path python(const fs::path& project) {
    return project / L"tools" / L"train" / L".venv" / L"Scripts" / L"python.exe";
}

fs::path model(const fs::path& project, const char* category) {
    return project / L"models" / fs::path(category) / L"model.pth";
}

// Reads a progress line, "<done> of <total> images". False, and nothing changed, if `line` is something else.
bool readProgress(const std::string& line, int& done, int& total) {
    const std::size_t of = line.find(" of ");
    const std::size_t images = line.rfind(" images");
    if (of == std::string::npos || images == std::string::npos || images < of || images + 7 != line.size()) {
        return false;
    }
    int first = 0;
    int second = 0;
    const char* text = line.data();
    const auto [firstEnd, firstError] = std::from_chars(text, text + of, first);
    const auto [secondEnd, secondError] = std::from_chars(text + of + 4, text + images, second);
    if (firstError != std::errc() || firstEnd != text + of || secondError != std::errc() ||
        secondEnd != text + images) {
        return false;
    }
    done = first;
    total = second;
    return true;
}

[[noreturn]] void fail(const char* what) {
    throw std::runtime_error(std::string(what) + " (" + std::system_category().message(GetLastError()) + ")");
}

} // namespace

bool Finder::needed(const fs::path& folder, const char* category) {
    const fs::path project = projectFolder();
    std::error_code error;
    if (project.empty() || !fs::exists(python(project), error) || !fs::exists(model(project, category), error)) {
        return false;
    }
    const fs::path guesses = folder / (std::string(category) + "-guesses.txt");
    if (!fs::exists(guesses, error)) {
        return true;
    }
    const fs::file_time_type guessed = fs::last_write_time(guesses, error);
    const fs::file_time_type trained = fs::last_write_time(model(project, category), error);
    return !error && guessed < trained;
}

Finder::Finder(const fs::path& folder, const char* category) {
    const fs::path project = projectFolder();
    if (project.empty()) {
        throw std::runtime_error("The training tool (tools/train) is not next to observex.exe.");
    }
    const fs::path tool = project / L"tools" / L"train";
    std::wstring command = L"\"" + python(project).wstring() + L"\" -u \"" + (tool / L"guess.py").wstring() + L"\" \"" +
                           folder.wstring() + L"\" --category " + fs::path(category).wstring();

    // Its output and error output both go into one pipe, which only we read. A large pipe, so it never waits on us.
    SECURITY_ATTRIBUTES inheritable{sizeof(inheritable), nullptr, TRUE};
    HANDLE read = nullptr;
    HANDLE write = nullptr;
    if (!CreatePipe(&read, &write, &inheritable, 1 << 20)) {
        fail("Could not start the finder");
    }
    output_ = read;
    SetHandleInformation(read, HANDLE_FLAG_INHERIT, 0);

    // A job ends every process in it when it closes. The Python in the tool's .venv is a small launcher that starts
    // the real Python as a second process, so stopping only the first would leave the second running.
    job_ = CreateJobObjectW(nullptr, nullptr);
    if (!job_) {
        CloseHandle(write);
        fail("Could not start the finder");
    }
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    SetInformationJobObject(job_, JobObjectExtendedLimitInformation, &limits, sizeof(limits));

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdOutput = write;
    startup.hStdError = write;
    PROCESS_INFORMATION started{};
    // No window, and suspended until it is in the job.
    const BOOL ok = CreateProcessW(nullptr, command.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW | CREATE_SUSPENDED,
                                   nullptr, tool.c_str(), &startup, &started);
    CloseHandle(write); // Its copy keeps the pipe open until it ends.
    if (!ok) {
        fail("Could not start the training tool's Python");
    }
    process_ = started.hProcess;
    AssignProcessToJobObject(job_, started.hProcess);
    ResumeThread(started.hThread);
    CloseHandle(started.hThread);
}

Finder::~Finder() {
    if (job_) {
        CloseHandle(job_); // Ends it, if it still runs.
    }
    if (process_) {
        CloseHandle(process_);
    }
    if (output_) {
        CloseHandle(output_);
    }
}

Finder::State Finder::poll() {
    if (state_ != State::Running) {
        return state_;
    }
    read();
    if (WaitForSingleObject(process_, 0) != WAIT_OBJECT_0) {
        return state_;
    }
    read(); // What it wrote just before it ended.
    DWORD code = 1;
    GetExitCodeProcess(process_, &code);
    if (code == 0) {
        state_ = State::Done;
    } else {
        state_ = State::Failed;
        error_ = lastLine_.empty() ? "It stopped with code " + std::to_string(code) + "." : lastLine_;
    }
    return state_;
}

void Finder::read() {
    DWORD available = 0;
    while (PeekNamedPipe(output_, nullptr, 0, nullptr, &available, nullptr) && available > 0) {
        char buffer[4096];
        DWORD got = 0;
        if (!ReadFile(output_, buffer, std::min<DWORD>(available, sizeof(buffer)), &got, nullptr) || got == 0) {
            break;
        }
        pending_.append(buffer, got);
    }
    // guess.py reports "<done> of <total> images" after each image.
    std::size_t start = 0;
    for (std::size_t end; (end = pending_.find('\n', start)) != std::string::npos; start = end + 1) {
        std::string line = pending_.substr(start, end - start);
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (!readProgress(line, done_, total_) && !line.empty()) {
            lastLine_ = line;
        }
    }
    pending_.erase(0, start);
}

} // namespace observex
