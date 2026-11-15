#pragma once

#include <filesystem>

namespace observex {

// Says what crashed observex (the user asked, 2026-11-15 night, after a crash inside TensorRT's nvinfer_11.dll that
// Windows' own report kept from us: the app runs as administrator, so its reports are too, and no debugger is here to
// read them). Once installed, a crash on any thread (an access violation, or any exception nothing caught; also an
// uncaught C++ exception ending in std::terminate) appends to `logFile` (observex.log, in its line format):
//   CRASH: <the exception> at <module>+<offset> on thread <its name> (<id>)
//     #0 <module>+<offset> <function> <file>:<line>
//     ... (the call chain, up to kCrashFrames; functions and lines where the module's PDB is next to it, else "near"
//          its nearest export, a guess)
//     dump: <file>
// and writes a minidump of the process into `dumpFolder` (observex-YYYY-MM-DD_HH-MM-SS.dmp; the last kCrashDumps kept).
// Windows then goes on with the crash as before (its report, the app closing). Install early; a later call moves where
// it writes and makes it the process' crash handler again if a library (CUDA, TensorRT) put its own in since.
void installCrashLog(const std::filesystem::path& logFile, const std::filesystem::path& dumpFolder);

inline constexpr int kCrashFrames = 48;
inline constexpr int kCrashDumps = 5;

// A crash on purpose, on a thread of its own named "observex crash test", a few calls deep (observex_bench --crash);
// `uncaught`: by a C++ exception nothing catches instead.
[[noreturn]] void crashOnPurpose(bool uncaught = false);

} // namespace observex
