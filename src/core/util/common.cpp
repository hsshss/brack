#include "util/common.h"

#include <cstdio>
#include <mutex>

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <fstream>

#ifdef _WIN32
#include <windows.h>
#include <shlobj.h>
#else
#include <sys/utsname.h>
#include <unistd.h>
#ifdef __APPLE__
#include <sys/sysctl.h>
#endif
#endif

namespace brack {

size_t midiShortMessageLength(uint8_t status) {
    if (status < 0x80) return 0;
    if (status < 0xC0) return 3;
    if (status < 0xE0) return 2;
    if (status < 0xF0) return 3;
    switch (status) {
        case 0xF1: case 0xF3: return 2;
        case 0xF2: return 3;
        default: return 1;
    }
}

namespace {
std::mutex g_logMutex;
LogSink g_logSink;
thread_local bool t_isAudioThread = false;
}  // namespace

const char* buildArchitecture() {
#if defined(_M_ARM64) || defined(__aarch64__)
    return "arm64";
#elif defined(_M_X64) || defined(_M_ARM64EC) || defined(__x86_64__)
    return "x64";  // ARM64EC: x64 to everything around it, native inside (CMakeLists.txt)
#else
    return "x86";
#endif
}

#ifdef _WIN32
const char* machineArchitecture() {
    static const char* const machine = [] {
        USHORT process = 0, native = 0;
        if (!IsWow64Process2(GetCurrentProcess(), &process, &native)) return buildArchitecture();
        return native == IMAGE_FILE_MACHINE_ARM64 ? "arm64" : native == IMAGE_FILE_MACHINE_AMD64 ? "x64" : "x86";
    }();
    return machine;
}

bool architectureRunsHere(std::string_view arch) {
    const USHORT machine = arch == "x64" ? IMAGE_FILE_MACHINE_AMD64
                           : arch == "x86" ? IMAGE_FILE_MACHINE_I386
                           : arch == "arm64" ? IMAGE_FILE_MACHINE_ARM64
                                             : 0;
    if (!machine) return false;
    // Windows 11 says which machines it runs in user mode, emulated ones included.
    using GetMachineTypeAttributesFn = HRESULT(WINAPI*)(USHORT, int*);
    constexpr int kUserEnabled = 1;
    if (auto query = reinterpret_cast<GetMachineTypeAttributesFn>(
            GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "GetMachineTypeAttributes"))) {
        int attributes = 0;
        if (SUCCEEDED(query(machine, &attributes))) return (attributes & kUserEnabled) != 0;
    }
    USHORT process = 0, native = 0;
    if (!IsWow64Process2(GetCurrentProcess(), &process, &native)) return arch == buildArchitecture();
    if (machine == native) return true;
    return machine == IMAGE_FILE_MACHINE_I386 && (native == IMAGE_FILE_MACHINE_AMD64 || native == IMAGE_FILE_MACHINE_ARM64);
}
#else
const char* machineArchitecture() { return buildArchitecture(); }

namespace {
bool fileExists(const char* path) { return access(path, F_OK) == 0; }

#ifdef __APPLE__
// A native process: hw.optional.arm64. One Rosetta 2 runs: sysctl.proc_translated, the test Apple
// documents for it (macOS 27 reports hw.optional.arm64 to it too).
bool appleSilicon() {
    for (const char* name : {"hw.optional.arm64", "sysctl.proc_translated"}) {
        int value = 0;
        size_t size = sizeof value;
        if (sysctlbyname(name, &value, &size, nullptr, 0) == 0 && value == 1) return true;
    }
    return false;
}
#endif

#ifndef __APPLE__
// An enabled binfmt_misc entry of that name: the kernel hands such programs to an emulator.
bool binfmtEnabled(const char* name) {
    std::ifstream f(std::string("/proc/sys/fs/binfmt_misc/") + name);
    std::string first;
    return std::getline(f, first) && first == "enabled";
}

std::string inPath(const char* program) {
    const char* path = std::getenv("PATH");
    std::string dirs = path ? path : "/usr/local/bin:/usr/bin:/bin";
    for (size_t at = 0; at <= dirs.size();) {
        const size_t end = std::min(dirs.find(':', at), dirs.size());
        const std::string candidate = dirs.substr(at, end - at) + "/" + program;
        if (end > at && access(candidate.c_str(), X_OK) == 0) return candidate;
        at = end + 1;
    }
    return {};
}

// qemu-user leaves a program's loader to be found: in / or under QEMU_LD_PREFIX.
bool qemuFindsLoader(const char* loader) {
    const char* prefix = std::getenv("QEMU_LD_PREFIX");
    return fileExists(loader) || (prefix && *prefix && fileExists((std::string(prefix) + loader).c_str()));
}
#endif
}  // namespace

bool architectureLauncher(std::string_view arch, std::vector<std::string>& launcher) {
    launcher.clear();
    const std::string_view own = buildArchitecture();
    if (arch == own) return true;
#if defined(__APPLE__)
    // Apple silicon runs arm64 programs, also for a Brack that Rosetta 2 runs (a universal build's
    // x64 half), and x64 programs through Rosetta 2, when it is installed.
    if (!appleSilicon()) return false;
    if (arch == "arm64") return true;
    return arch == "x64" && fileExists("/Library/Apple/usr/libexec/oah/libRosettaRuntime");
#else
    // x64 Linux runs x86 programs when the 32-bit C library is installed (its loader). An x86
    // Brack on an x64 kernel (which says so to a 32-bit process too, as FEX-Emu does) runs x64
    // programs; it ships no x64 plugin host, and that is what it then says.
    if (own == "x64" && arch == "x86") return fileExists("/lib/ld-linux.so.2");
    if (own == "x86" && arch == "x64") {
        utsname u{};
        return uname(&u) == 0 && std::string_view(u.machine) == "x86_64";
    }
    // ARM64 Linux runs x64 and x86 programs through FEX-Emu: registered with the kernel
    // (binfmt_misc), or started as its argument. Or through qemu-user, registered with the kernel,
    // when the program's loader is there for it.
    if (own == "arm64" && (arch == "x64" || arch == "x86")) {
        const bool x64 = arch == "x64";
        if (binfmtEnabled(x64 ? "FEX-x86_64" : "FEX-x86")) return true;
        if (binfmtEnabled(x64 ? "qemu-x86_64" : "qemu-i386") &&
            qemuFindsLoader(x64 ? "/lib64/ld-linux-x86-64.so.2" : "/lib/ld-linux.so.2"))
            return true;
        // FEX-Emu's program is FEX; older releases called it FEXInterpreter.
        for (const char* emulator : {"FEX", "FEXInterpreter"})
            if (std::string path = inPath(emulator); !path.empty()) {
                launcher = {path};
                return true;
            }
        return false;
    }
    // Other ARM64 programs, through qemu-user when it is registered and an ARM64 C library is there.
    if (arch == "arm64") return binfmtEnabled("qemu-aarch64") && qemuFindsLoader("/lib/ld-linux-aarch64.so.1");
    return false;
#endif
}

bool architectureRunsHere(std::string_view arch) {
    std::vector<std::string> launcher;
    return architectureLauncher(arch, launcher);
}
#endif

void setLogSink(LogSink sink) {
    std::lock_guard lock(g_logMutex);
    g_logSink = std::move(sink);
}

const char* logLevelName(LogLevel level) {
    switch (level) {
        case LogLevel::Debug: return "debug";
        case LogLevel::Info: return "info";
        case LogLevel::Warning: return "warning";
        case LogLevel::Error: return "error";
    }
    return "?";
}

void log(LogLevel level, std::string message) {
    std::lock_guard lock(g_logMutex);
    if (g_logSink) {
        g_logSink(level, message);
    } else if (level >= LogLevel::Info) {
        std::fprintf(stderr, "[%s] %s\n", logLevelName(level), message.c_str());
    }
}

void setCurrentThreadIsAudio(bool isAudio) { t_isAudioThread = isAudio; }
bool currentThreadIsAudio() { return t_isAudioThread; }

#ifdef _WIN32
std::wstring widen(std::string_view s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
    return w;
}
std::string narrow(std::wstring_view w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}
std::filesystem::path pathFromUtf8(std::string_view utf8) { return std::filesystem::path(widen(utf8)); }
std::string pathToUtf8(const std::filesystem::path& p) { return narrow(p.native()); }
#else
std::filesystem::path pathFromUtf8(std::string_view utf8) { return std::filesystem::path(std::string(utf8)); }
std::string pathToUtf8(const std::filesystem::path& p) { return p.string(); }
#endif

std::string utf16ToUtf8(std::u16string_view s) {
    std::string out;
    out.reserve(s.size());
    auto put = [&](uint32_t c) {
        if (c < 0x80) {
            out += (char)c;
        } else if (c < 0x800) {
            out += (char)(0xC0 | (c >> 6));
            out += (char)(0x80 | (c & 0x3F));
        } else if (c < 0x10000) {
            out += (char)(0xE0 | (c >> 12));
            out += (char)(0x80 | ((c >> 6) & 0x3F));
            out += (char)(0x80 | (c & 0x3F));
        } else {
            out += (char)(0xF0 | (c >> 18));
            out += (char)(0x80 | ((c >> 12) & 0x3F));
            out += (char)(0x80 | ((c >> 6) & 0x3F));
            out += (char)(0x80 | (c & 0x3F));
        }
    };
    for (size_t i = 0; i < s.size(); ++i) {
        const uint32_t c = s[i];
        if (c >= 0xD800 && c < 0xDC00 && i + 1 < s.size() && s[i + 1] >= 0xDC00 && s[i + 1] < 0xE000) {
            put(0x10000 + ((c - 0xD800) << 10) + (s[++i] - 0xDC00));
        } else if (c >= 0xD800 && c < 0xE000) {
            put(0xFFFD);  // a lone surrogate
        } else {
            put(c);
        }
    }
    return out;
}

std::u16string utf8ToUtf16(std::string_view s) {
    std::u16string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size();) {
        const unsigned char b = (unsigned char)s[i];
        const int len = b < 0x80 ? 1 : (b >> 5) == 0x6 ? 2 : (b >> 4) == 0xE ? 3 : (b >> 3) == 0x1E ? 4 : 0;
        uint32_t c = len == 1 ? b : len == 2 ? (b & 0x1F) : len == 3 ? (b & 0x0F) : (b & 0x07);
        bool ok = len > 0 && i + len <= s.size();
        for (int k = 1; ok && k < len; ++k) {
            const unsigned char cb = (unsigned char)s[i + k];
            ok = (cb & 0xC0) == 0x80;
            c = (c << 6) | (cb & 0x3F);
        }
        // Overlong forms, surrogates and values past U+10FFFF are not characters.
        static const uint32_t kMin[] = {0, 0, 0x80, 0x800, 0x10000};
        if (ok && (c < kMin[len] || (c >= 0xD800 && c < 0xE000) || c > 0x10FFFF)) ok = false;
        if (!ok) {
            out += u'\uFFFD';
            ++i;
            continue;
        }
        if (c >= 0x10000) {
            out += (char16_t)(0xD800 + ((c - 0x10000) >> 10));
            out += (char16_t)(0xDC00 + ((c - 0x10000) & 0x3FF));
        } else {
            out += (char16_t)c;
        }
        i += len;
    }
    return out;
}

std::filesystem::path configDirectory() {
#ifdef _WIN32
    PWSTR p = nullptr;
    std::filesystem::path dir;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &p))) dir = std::filesystem::path(p) / L"brack";
    CoTaskMemFree(p);
    return dir;
#else
    const char* home = std::getenv("HOME");
#if defined(__APPLE__)
    return home ? std::filesystem::path(home) / "Library/Application Support/brack" : std::filesystem::path();
#else
    if (const char* xdg = std::getenv("XDG_CONFIG_HOME"); xdg && *xdg) return std::filesystem::path(xdg) / "brack";
    return home ? std::filesystem::path(home) / ".config/brack" : std::filesystem::path();
#endif
#endif
}

bool writeFileAtomic(const std::filesystem::path& path, const std::string& contents, std::string& error) {
    std::error_code ec;
    if (path.has_parent_path()) std::filesystem::create_directories(path.parent_path(), ec);
    // Per process: two brack processes saving the same file must not share the temporary one.
#ifdef _WIN32
    const unsigned long pid = GetCurrentProcessId();
#else
    const unsigned long pid = (unsigned long)getpid();
#endif
    auto tmp = path;
    tmp += "." + std::to_string(pid) + ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!(f << contents) || !f.flush()) {
            error = "cannot write " + pathToUtf8(tmp);
            return false;
        }
    }
    std::filesystem::rename(tmp, path, ec);  // replaces an existing file
    if (ec) {
        error = "cannot replace " + pathToUtf8(path) + ": " + ec.message();
        std::filesystem::remove(tmp, ec);
        return false;
    }
    return true;
}

static const char kB64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string base64Encode(const std::vector<uint8_t>& d) {
    std::string out;
    out.reserve((d.size() + 2) / 3 * 4);
    size_t i = 0;
    for (; i + 2 < d.size(); i += 3) {
        uint32_t v = (d[i] << 16) | (d[i + 1] << 8) | d[i + 2];
        out += kB64[(v >> 18) & 63];
        out += kB64[(v >> 12) & 63];
        out += kB64[(v >> 6) & 63];
        out += kB64[v & 63];
    }
    if (i + 1 == d.size()) {
        uint32_t v = d[i] << 16;
        out += kB64[(v >> 18) & 63];
        out += kB64[(v >> 12) & 63];
        out += "==";
    } else if (i + 2 == d.size()) {
        uint32_t v = (d[i] << 16) | (d[i + 1] << 8);
        out += kB64[(v >> 18) & 63];
        out += kB64[(v >> 12) & 63];
        out += kB64[(v >> 6) & 63];
        out += '=';
    }
    return out;
}

bool base64Decode(std::string_view text, std::vector<uint8_t>& out) {
    out.clear();
    uint32_t acc = 0;
    int bits = 0;
    for (char c : text) {
        int v;
        if (c >= 'A' && c <= 'Z') v = c - 'A';
        else if (c >= 'a' && c <= 'z') v = c - 'a' + 26;
        else if (c >= '0' && c <= '9') v = c - '0' + 52;
        else if (c == '+') v = 62;
        else if (c == '/') v = 63;
        else if (c == '=' || c == '\n' || c == '\r' || c == ' ') continue;
        else return false;
        acc = (acc << 6) | (uint32_t)v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(uint8_t((acc >> bits) & 0xFF));
        }
    }
    return true;
}

void parallelFor(size_t count, const std::function<void(size_t)>& fn, const std::function<void()>& first) {
    std::atomic<size_t> next{0};
    auto work = [&] {
        for (size_t i; (i = next++) < count;) fn(i);
    };
    const size_t helpers = std::min<size_t>(count, std::max(4u, std::thread::hardware_concurrency())) - (count && !first ? 1 : 0);
    std::vector<std::thread> threads;
    for (size_t i = 0; i < helpers; ++i) threads.emplace_back(work);
    if (first) first();
    work();
    for (auto& t : threads) t.join();
}

}  // namespace brack
