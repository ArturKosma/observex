#include "engine/CrashLog.h"

#include <windows.h>

#include <dbghelp.h>

#include <algorithm>
#include <atomic>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <exception>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace observex {

namespace {

// Where it writes: set by installCrashLog, read at the crash (fixed buffers, nothing to make then).
wchar_t gLogFile[1024] = {};
wchar_t gDumpFolder[1024] = {};
std::atomic<bool> gInstalled{false};
std::atomic<bool> gCrashed{false}; // Only the first crash is told (others follow it as the process goes down).
LPTOP_LEVEL_EXCEPTION_FILTER gPrevious = nullptr;

std::string utf8(const wchar_t* text) {
    if (text == nullptr || text[0] == L'\0') {
        return {};
    }
    const int size = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<std::size_t>(std::max(size, 1)), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text, -1, out.data(), size, nullptr, nullptr);
    out.resize(std::strlen(out.c_str()));
    return out;
}

// observex.log's line start (core/Log): "2026-11-15 22:47:55.085  ".
std::string stamp() {
    SYSTEMTIME now{};
    GetLocalTime(&now);
    char text[32];
    std::snprintf(text, sizeof(text), "%04u-%02u-%02u %02u:%02u:%02u.%03u  ", now.wYear, now.wMonth, now.wDay,
                  now.wHour, now.wMinute, now.wSecond, now.wMilliseconds);
    return text;
}

// The module `address` is in (its file name) and the offset in it.
std::string placeOf(DWORD64 address) {
    HMODULE module = nullptr;
    char text[400];
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(address), &module) &&
        module != nullptr) {
        wchar_t path[MAX_PATH] = {};
        GetModuleFileNameW(module, path, MAX_PATH);
        const wchar_t* name = std::wcsrchr(path, L'\\');
        std::snprintf(text, sizeof(text), "%s+0x%llx", utf8(name != nullptr ? name + 1 : path).c_str(),
                      static_cast<unsigned long long>(address - reinterpret_cast<DWORD64>(module)));
    } else {
        std::snprintf(text, sizeof(text), "0x%llx", static_cast<unsigned long long>(address));
    }
    return text;
}

std::string describe(const EXCEPTION_RECORD* record) {
    char text[200];
    switch (record->ExceptionCode) {
    case EXCEPTION_ACCESS_VIOLATION: {
        const ULONG_PTR how = record->NumberParameters >= 2 ? record->ExceptionInformation[0] : 0;
        std::snprintf(text, sizeof(text), "access violation %s 0x%llx",
                      how == 0   ? "reading"
                      : how == 8 ? "executing"
                                 : "writing",
                      static_cast<unsigned long long>(record->NumberParameters >= 2 ? record->ExceptionInformation[1]
                                                                                     : 0));
        return text;
    }
    case EXCEPTION_STACK_OVERFLOW:
        return "stack overflow";
    case EXCEPTION_ILLEGAL_INSTRUCTION:
        return "illegal instruction";
    case EXCEPTION_INT_DIVIDE_BY_ZERO:
        return "integer division by zero";
    case EXCEPTION_IN_PAGE_ERROR:
        return "in-page error";
    case 0xE06D7363:
        return "C++ exception nothing caught";
    default:
        std::snprintf(text, sizeof(text), "exception 0x%08lx", static_cast<unsigned long>(record->ExceptionCode));
        return text;
    }
}

std::string threadName() {
    std::string name;
    PWSTR description = nullptr;
    if (SUCCEEDED(GetThreadDescription(GetCurrentThread(), &description)) && description != nullptr) {
        name = utf8(description);
        LocalFree(description);
    }
    return name.empty() ? "unnamed" : name;
}

// Keeps the newest kCrashDumps - 1 dumps (a new one follows), deleting older ones (all ours: observex-*.dmp).
void pruneDumps() {
    std::vector<std::wstring> dumps;
    WIN32_FIND_DATAW found{};
    const std::wstring pattern = std::wstring(gDumpFolder) + L"\\observex-*.dmp";
    HANDLE search = FindFirstFileW(pattern.c_str(), &found);
    if (search == INVALID_HANDLE_VALUE) {
        return;
    }
    do {
        dumps.emplace_back(found.cFileName);
    } while (FindNextFileW(search, &found));
    FindClose(search);
    std::sort(dumps.begin(), dumps.end()); // Named by time.
    for (std::size_t i = 0; i + (kCrashDumps - 1) < dumps.size(); ++i) {
        DeleteFileW((std::wstring(gDumpFolder) + L"\\" + dumps[i]).c_str());
    }
}

// The dump's file, made now (empty if it could not be).
std::wstring writeDump(EXCEPTION_POINTERS* info) {
    if (gDumpFolder[0] == L'\0') {
        return {};
    }
    CreateDirectoryW(gDumpFolder, nullptr);
    pruneDumps();
    SYSTEMTIME now{};
    GetLocalTime(&now);
    wchar_t path[1200];
    HANDLE file = INVALID_HANDLE_VALUE;
    for (int n = 1; n <= 9 && file == INVALID_HANDLE_VALUE; ++n) { // Another crash this second: -2, -3...
        wchar_t again[8] = {};
        if (n > 1) {
            std::swprintf(again, 8, L"-%d", n);
        }
        std::swprintf(path, sizeof(path) / sizeof(path[0]), L"%ls\\observex-%04u-%02u-%02u_%02u-%02u-%02u%ls.dmp",
                      gDumpFolder, now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond, again);
        file = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    }
    if (file == INVALID_HANDLE_VALUE) {
        return {};
    }
    MINIDUMP_EXCEPTION_INFORMATION exception{};
    exception.ThreadId = GetCurrentThreadId();
    exception.ExceptionPointers = info;
    exception.ClientPointers = FALSE;
    const auto kind = static_cast<MINIDUMP_TYPE>(MiniDumpWithThreadInfo | MiniDumpWithUnloadedModules |
                                                 MiniDumpWithIndirectlyReferencedMemory);
    const BOOL written = MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), file, kind,
                                           info != nullptr ? &exception : nullptr, nullptr, nullptr);
    CloseHandle(file);
    if (!written) {
        DeleteFileW(path);
        return {};
    }
    return path;
}

// Appends the crash's lines to the log: `what`, then the call chain from `context` (changed by the walk).
void report(const std::string& what, CONTEXT* context, EXCEPTION_POINTERS* info) {
    const std::string at = stamp(); // One time for all its lines.
    std::string text = at + "CRASH: " + what;
    if (info != nullptr) {
        text += " at " + placeOf(reinterpret_cast<DWORD64>(info->ExceptionRecord->ExceptionAddress));
    }
    text += " on thread " + threadName() + " (" + std::to_string(GetCurrentThreadId()) + ")\n";

    const HANDLE process = GetCurrentProcess();
    SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES | SYMOPT_FAIL_CRITICAL_ERRORS |
                  SYMOPT_NO_PROMPTS);
    const bool symbols = SymInitializeW(process, nullptr, TRUE) != FALSE; // The modules' own folders, their PDBs.
    STACKFRAME64 frame{};
    frame.AddrPC.Offset = context->Rip;
    frame.AddrPC.Mode = AddrModeFlat;
    frame.AddrFrame.Offset = context->Rbp;
    frame.AddrFrame.Mode = AddrModeFlat;
    frame.AddrStack.Offset = context->Rsp;
    frame.AddrStack.Mode = AddrModeFlat;
    alignas(SYMBOL_INFO) char symbolSpace[sizeof(SYMBOL_INFO) + 512] = {};
    auto* symbol = reinterpret_cast<SYMBOL_INFO*>(symbolSpace);
    for (int i = 0; i < kCrashFrames; ++i) {
        if (!StackWalk64(IMAGE_FILE_MACHINE_AMD64, process, GetCurrentThread(), &frame, context, nullptr,
                         SymFunctionTableAccess64, SymGetModuleBase64, nullptr) ||
            frame.AddrPC.Offset == 0) {
            break;
        }
        const DWORD64 address = frame.AddrPC.Offset;
        std::string line = at + "  #" + std::to_string(i) + " " + placeOf(address);
        symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
        symbol->MaxNameLen = 511;
        DWORD64 displacement = 0;
        if (symbols && SymFromAddr(process, address, &displacement, symbol)) {
            // A module without its PDB (Windows', TensorRT's) gives only its exports: the nearest one before, a guess.
            line += (symbol->Flags & SYMFLAG_EXPORT) != 0 ? " near " : " ";
            line += symbol->Name;
            IMAGEHLP_LINE64 source{};
            source.SizeOfStruct = sizeof(source);
            DWORD column = 0;
            if (SymGetLineFromAddr64(process, address, &column, &source) && source.FileName != nullptr) {
                const char* name = std::strrchr(source.FileName, '\\');
                line += " " + std::string(name != nullptr ? name + 1 : source.FileName) + ":" +
                        std::to_string(source.LineNumber);
            }
        }
        text += line + "\n";
    }
    if (symbols) {
        SymCleanup(process);
    }
    // The log first (what matters most), then the dump, then where it is.
    HANDLE file = CreateFileW(gLogFile, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return;
    }
    DWORD done = 0;
    WriteFile(file, text.data(), static_cast<DWORD>(text.size()), &done, nullptr);
    FlushFileBuffers(file);
    const std::wstring dump = writeDump(info);
    const std::string where = at + "  dump: " + (dump.empty() ? std::string("not written") : utf8(dump.c_str())) +
                              "\n";
    WriteFile(file, where.data(), static_cast<DWORD>(where.size()), &done, nullptr);
    CloseHandle(file);
}

LONG WINAPI onException(EXCEPTION_POINTERS* info) {
    if (!gCrashed.exchange(true)) {
        CONTEXT context = *info->ContextRecord;
        report(describe(info->ExceptionRecord), &context, info);
    }
    return gPrevious != nullptr ? gPrevious(info) : EXCEPTION_CONTINUE_SEARCH;
}

// abort() (std::terminate on an uncaught C++ exception ends there): told before it goes on.
void onAbort(int) {
    if (!gCrashed.exchange(true)) {
        std::string what = "abort (an uncaught C++ exception, or std::terminate)";
        if (const std::exception_ptr thrown = std::current_exception()) {
            try {
                std::rethrow_exception(thrown);
            } catch (const std::exception& e) {
                what += ": " + std::string(e.what());
            } catch (...) {
            }
        }
        CONTEXT context{};
        RtlCaptureContext(&context);
        report(what, &context, nullptr);
    }
}

void copyPath(wchar_t* to, std::size_t size, const std::filesystem::path& path) {
    const std::wstring text = std::filesystem::absolute(path).wstring();
    wcsncpy_s(to, size, text.c_str(), _TRUNCATE);
}

// A few calls deep, so the chain shows them (not folded into one).
volatile std::uintptr_t gNowhere = 0x10;
__declspec(noinline) void crashHere() {
    *reinterpret_cast<volatile int*>(gNowhere) = 1;
}
__declspec(noinline) void crashDeeper(int depth) {
    if (depth > 0) {
        crashDeeper(depth - 1);
    } else {
        crashHere();
    }
    gNowhere = gNowhere + 0; // Not a tail call.
}

} // namespace

void installCrashLog(const std::filesystem::path& logFile, const std::filesystem::path& dumpFolder) {
    copyPath(gLogFile, sizeof(gLogFile) / sizeof(gLogFile[0]), logFile);
    copyPath(gDumpFolder, sizeof(gDumpFolder) / sizeof(gDumpFolder[0]), dumpFolder);
    // Ours again if a library put its own in since (it goes on after ours).
    const LPTOP_LEVEL_EXCEPTION_FILTER previous = SetUnhandledExceptionFilter(onException);
    if (previous != onException) {
        gPrevious = previous;
    }
    if (gInstalled.exchange(true)) {
        return;
    }
    std::signal(SIGABRT, onAbort);
    // Room on the stack for the report when the stack itself overflowed.
    ULONG reserve = 64 * 1024;
    SetThreadStackGuarantee(&reserve);
}

void crashOnPurpose(bool uncaught) {
    std::thread thread([uncaught] {
        SetThreadDescription(GetCurrentThread(), L"observex crash test");
        if (uncaught) {
            throw std::runtime_error("thrown on purpose"); // Out of the thread: std::terminate.
        }
        crashDeeper(3);
    });
    thread.join();
    std::abort(); // Never reached.
}

} // namespace observex
