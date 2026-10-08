#pragma once
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <thread>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace brack {

enum class LogLevel { Debug = 0, Info = 1, Warning = 2, Error = 3 };

// Global log sink. Thread safe; the sink may be called from any thread.
using LogSink = std::function<void(LogLevel, const std::string&)>;
void setLogSink(LogSink sink);
void log(LogLevel level, std::string message);
inline void logInfo(std::string m) { log(LogLevel::Info, std::move(m)); }
inline void logWarn(std::string m) { log(LogLevel::Warning, std::move(m)); }
inline void logError(std::string m) { log(LogLevel::Error, std::move(m)); }
const char* logLevelName(LogLevel level);

// UTF-8 <-> filesystem path (UTF-16 on Windows).
std::filesystem::path pathFromUtf8(std::string_view utf8);
std::string pathToUtf8(const std::filesystem::path& p);
#ifdef _WIN32
std::wstring widen(std::string_view utf8);
std::string narrow(std::wstring_view wide);
#endif
// UTF-16 <-> UTF-8 on every platform (VST3 strings are UTF-16 everywhere, while wchar_t is
// 32-bit outside Windows). Ill-formed input becomes U+FFFD.
std::string utf16ToUtf8(std::u16string_view utf16);
std::u16string utf8ToUtf16(std::string_view utf8);

// The architecture this build runs as: "x64", "x86" or "arm64".
const char* buildArchitecture();
// The architecture of this computer (a buildArchitecture() name): the build's own, unless the
// build runs under Windows' emulation (an x64 or x86 Brack on an ARM64 PC says "arm64"). Only
// Windows tells them apart: it is what decides which VST3 binaries an x64 process can load.
const char* machineArchitecture();
// Whether this computer runs programs built for `arch` (a buildArchitecture() name): its own
// architecture, or one it emulates (x86 on x64 Windows; x64 and x86 on ARM64 Windows 11).
bool architectureRunsHere(std::string_view arch);
#ifndef _WIN32
// How this computer runs a program built for `arch`: by itself (true, `launcher` left empty),
// through an emulator (true, `launcher` is the command to put before the program), or not at all
// (false); which emulators count is in common.cpp. Whether Brack ships a plugin host for `arch`
// is another question (pluginHostExecutable).
bool architectureLauncher(std::string_view arch, std::vector<std::string>& launcher);
#endif

// Per-user settings folder (not created here):
//   Windows %APPDATA%\brack, macOS ~/Library/Application Support/brack,
//   Linux $XDG_CONFIG_HOME/brack (default ~/.config/brack). Empty if unknown.
std::filesystem::path configDirectory();

// Writes via a temporary file and a rename, so a crash never leaves a half-written file.
bool writeFileAtomic(const std::filesystem::path& path, const std::string& contents, std::string& error);

std::string base64Encode(const std::vector<uint8_t>& data);
bool base64Decode(std::string_view text, std::vector<uint8_t>& out);

// Polls pred every millisecond until it holds or the timeout expires.
template <typename Pred>
bool waitFor(Pred pred, std::chrono::milliseconds timeout) {
    auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!pred()) {
        if (std::chrono::steady_clock::now() > deadline) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return true;
}

// Bytes in a MIDI message that starts with `status` (not SysEx); 0 for a data byte.
size_t midiShortMessageLength(uint8_t status);

// Marks the calling thread as the audio thread (for clap thread_check).
void setCurrentThreadIsAudio(bool isAudio);
bool currentThreadIsAudio();

// Calls fn(i) for every i below `count`, on up to max(4, cores) threads, the calling one among them
// once `first` (if any) has run on it. Returns when all have. Neither may throw.
void parallelFor(size_t count, const std::function<void(size_t)>& fn, const std::function<void()>& first = {});

}  // namespace brack
