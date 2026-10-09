// Plugin host processes. A plugin that takes its process down in a way Brack never sees coming
// (a crash on a thread of its own or in its window procedure, abort(), __fastfail(), a hang)
// leaves Brack and the rest of the rack running; a plugin sounds the same in a host process as
// in Brack's; host processes end with Brack, however it ends; a plugin built for the other
// architecture runs too, when that architecture's build is there to lend its plugin host; the
// plugin host's audio thread is real-time (on Linux, where this user may have that); a session's
// plugins load at once; an editor's window fits it (on Windows, and X11); a VST3 plugin finds the
// host thread's run loop through its host context (Linux). Also prints what a block costs.
//
//   test_isolation <brack-test-synth.clap> <vst2 synth> <vst3 synth> [<other architectures' synths>...]
//   test_isolation --block-cost <brack-test-synth.clap> [<frames>]
#ifdef _WIN32
#include <windows.h>
#include <avrt.h>
#include <tlhelp32.h>
#else
#include <fcntl.h>
#include <sched.h>
#include <signal.h>
#include <sys/resource.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <unistd.h>
#endif
#ifdef __APPLE__
#import <AppKit/AppKit.h>
#include <CoreGraphics/CoreGraphics.h>
#include <libproc.h>
#include <sys/proc.h>
#endif

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "engine.h"
#include "plugin/plugin.h"
#include "plugin/plugin_files.h"
#include "remote/plugin_host.h"
#include "util/common.h"
#include "util/main_thread.h"
#ifdef __linux__
#include "util/realtime_linux.h"
#elif defined(__APPLE__)
#include "util/realtime_mac.h"
#endif

using namespace brack;

namespace {

int g_failures = 0;
#define CHECK(cond)                                                                \
    do {                                                                           \
        if (!(cond)) {                                                             \
            std::fprintf(stderr, "CHECK failed: %s (line %d)\n", #cond, __LINE__); \
            ++g_failures;                                                          \
        }                                                                          \
    } while (0)

bool has(const std::string& s, const std::string& part) { return s.find(part) != std::string::npos; }

void printWarnings() {
    setLogSink([](LogLevel l, const std::string& m) {
        if (l >= LogLevel::Warning) std::printf("  [%s] %s\n", logLevelName(l), m.c_str());
    });
}

size_t count(const std::string& s, const std::string& what) {
    size_t n = 0;
    for (size_t at = s.find(what); at != std::string::npos; at = s.find(what, at + 1)) ++n;
    return n;
}

const EngineSnapshot::Plugin* find(const EngineSnapshot& s, const std::string& id) {
    for (auto& p : s.plugins)
        if (p.id == id) return &p;
    return nullptr;
}

// Peak of outputs 1 and 2 over `total` frames.
std::vector<float> renderPeaks(Engine& e, uint32_t total) {
    const uint32_t frames = 256;
    std::vector<std::vector<float>> buf(2, std::vector<float>(frames));
    float* ptrs[2] = {buf[0].data(), buf[1].data()};
    std::vector<float> peak(2, 0.0f);
    for (uint32_t done = 0; done < total; done += frames) {
        e.render(ptrs, 2, frames);
        for (int c = 0; c < 2; ++c)
            for (float v : buf[c]) peak[c] = std::max(peak[c], std::fabs(v));
    }
    return peak;
}

// Engine with plugins in host processes (the default), rendering manually.
void start(Engine& e) {
    std::string err;
    CHECK(!e.config().pluginsInProcess);
    CHECK(e.startManual(48000, 2, 256, err));
}

bool reportsAs(const std::string& s, const std::string& reports) {
    for (size_t at = 0; at <= reports.size();) {
        const size_t end = std::min(reports.find('|', at), reports.size());
        if (has(s, reports.substr(at, end - at))) return true;
        at = end + 1;
    }
    return false;
}

struct Kind {
    uint8_t code;        // the SysEx that sets it off (test_synth.cpp)
    const char* name;
    const char* report;  // part of what Brack reports; or of either, split by '|'
    bool editor;         // needs the editor open
};

// Plugin "c" goes down the way `kind` says, while "a" plays on in its own process.
void goesDown(const std::string& synth, const Kind& kind) {
    std::printf("%s:\n", kind.name);
    Engine e;
    std::string err;
    start(e);
    CHECK(e.addPlugin({"a", synth, "brack.test.synth", "", std::nullopt}, false, err) == "a");
    CHECK(e.addPlugin({"c", synth, "brack.test.synth", "", std::nullopt}, false, err) == "c");
    CHECK(e.setAudioRoutes("a", {{"a", 0, 0, 0, 1.0f}}, err));
    CHECK(e.setAudioRoutes("c", {{"c", 0, 0, 1, 1.0f}}, err));
    e.saveSessionJson();  // the state "c" keeps once it is gone
    if (kind.editor) CHECK(e.setPluginGuiVisible("c", true, err));

    const uint8_t on[] = {0x90, 0x3C, 0x64};
    CHECK(e.sendMidiToPlugin("a", 0, on, 3) == MidiSend::Sent);
    CHECK(e.sendMidiToPlugin("c", 0, on, 3) == MidiSend::Sent);
    auto peaks = renderPeaks(e, 4800);
    CHECK(peaks[0] > 0.1f && peaks[1] > 0.1f);

    const uint8_t sysex[] = {0xF0, 0x7D, 0x63, kind.code, 0xF7};
    CHECK(e.sendMidiToPlugin("c", 0, sysex, sizeof sysex) == MidiSend::Sent);
    const auto t0 = std::chrono::steady_clock::now();
    const EngineSnapshot::Plugin* c = nullptr;
    EngineSnapshot snap;
    while (std::chrono::steady_clock::now() - t0 < std::chrono::seconds(10)) {
        renderPeaks(e, 1024);
        snap = e.snapshot();
        c = find(snap, "c");
        if (c && c->crashed) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    CHECK(c && c->crashed);
    if (c) std::printf("  c (%.1f s): %s\n", secs, c->status.c_str());
    CHECK(c && reportsAs(c->status, kind.report));
    CHECK(c && !c->guiOpen);

    peaks = renderPeaks(e, 4800);
    std::printf("  a %.3f, c %.3f\n", peaks[0], peaks[1]);
    CHECK(peaks[0] > 0.1f);
    CHECK(peaks[1] == 0.0f);
    CHECK(count(e.saveSessionJson(), "\"state\"") == 2);

    bool event = false;  // queued by the host thread's next idle pass
    for (int i = 0; i < 100 && !event; ++i) {
        for (EngineEvent ev; e.pollEvent(ev);)
            if (ev.type == EngineEvent::Type::PluginCrashed && ev.id == "c") event = reportsAs(ev.message, kind.report);
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    CHECK(event);
    CHECK(e.removePlugin("c", err));
    CHECK(e.snapshot().plugins.size() == 1);
}

// A few timed notes on the plugin at `path`, both outputs interleaved by block.
std::vector<float> renderNotes(const std::string& path, bool inProcess, EngineSnapshot* snap = nullptr) {
    Engine e;
    std::string err;
    EngineConfig cfg;
    cfg.pluginsInProcess = inProcess;
    CHECK(e.setConfig(cfg, err));
    CHECK(e.startManual(48000, 2, 256, err));
    CHECK(e.addPlugin({"p", path, "", "", std::nullopt}, true, err) == "p");
    const uint8_t on[] = {0x90, 0x3C, 0x64}, on2[] = {0x90, 0x43, 0x50}, off[] = {0x80, 0x3C, 0x00};
    CHECK(e.sendMidiToPlugin("p", 0, on, 3, 0) == MidiSend::Sent);
    CHECK(e.sendMidiToPlugin("p", 0, on2, 3, 1000) == MidiSend::Sent);
    CHECK(e.sendMidiToPlugin("p", 0, off, 3, 3000) == MidiSend::Sent);
    std::vector<float> out;
    std::vector<std::vector<float>> buf(2, std::vector<float>(256));
    float* ptrs[2] = {buf[0].data(), buf[1].data()};
    for (int b = 0; b < 32; ++b) {
        e.render(ptrs, 2, 256);
        out.insert(out.end(), buf[0].begin(), buf[0].end());
        out.insert(out.end(), buf[1].begin(), buf[1].end());
    }
    if (snap) *snap = e.snapshot();
    return out;
}

// The same notes give the same samples in this process and in a host process.
void soundsTheSame(const std::string& path) {
    EngineSnapshot local1, remote1;
    const auto local = renderNotes(path, true, &local1), remote = renderNotes(path, false, &remote1);
    CHECK(local1.plugins.size() == 1 && !local1.plugins[0].separateProcess &&
          local1.plugins[0].architecture == buildArchitecture());
    CHECK(remote1.plugins.size() == 1 && remote1.plugins[0].separateProcess &&
          remote1.plugins[0].architecture == buildArchitecture());
    float peak = 0;
    for (float v : local) peak = std::max(peak, std::fabs(v));
    std::printf("%s: peak %.3f, %s\n", path.c_str(), peak, local == remote ? "same samples" : "DIFFERENT");
    CHECK(peak > 0.1f);
    CHECK(local == remote);
}

// The other architecture's build of the synth runs in that architecture's plugin host, even
// with plugins in this process otherwise, and sounds like this architecture's build of it.
void otherArchitecture(const std::string& synth, const std::string& otherSynth) {
    std::error_code ec;
    if (otherSynth.empty() || !std::filesystem::exists(pathFromUtf8(otherSynth), ec)) {
        std::printf("other architecture: skipped, no build of the test synth for it (%s)\n", otherSynth.c_str());
        return;
    }
    const std::string arch = pluginArchitecture(pathFromUtf8(otherSynth));
    CHECK(!arch.empty() && arch != buildArchitecture());
    if (!architectureRunsHere(arch)) {
        std::printf("other architecture: skipped, this computer does not run %s programs\n", arch.c_str());
        return;
    }
    if (!std::filesystem::is_regular_file(pluginHostExecutable(arch), ec)) {
        std::printf("other architecture: skipped, no plugin host for %s beside this build\n", arch.c_str());
        return;
    }
    const auto ours = renderNotes(synth, true);
    for (bool inProcess : {true, false}) {
        EngineSnapshot snap;
        const auto theirs = renderNotes(otherSynth, inProcess, &snap);
        CHECK(snap.plugins.size() == 1 && snap.plugins[0].separateProcess && snap.plugins[0].architecture == arch);
        float diff = 0, peak = 0;
        for (size_t i = 0; i < ours.size() && i < theirs.size(); ++i) {
            diff = std::max(diff, std::fabs(ours[i] - theirs[i]));
            peak = std::max(peak, std::fabs(theirs[i]));
        }
        std::printf("%s synth from this %s Brack: peak %.3f, at most %g from ours\n", arch.c_str(), buildArchitecture(),
                    peak, diff);
        CHECK(theirs.size() == ours.size() && peak > 0.1f && diff < 1e-4f);  // their sin(), our sin()
    }

    // A scan lists it, as the other architecture's.
    bool listed = false;
    for (auto& d : Engine::scanPlugins({pathToUtf8(pathFromUtf8(otherSynth).parent_path())}, {}, false))
        if (d.architecture == arch && d.id == "brack.test.synth") listed = true;
    CHECK(listed);
}

#ifdef __APPLE__
// Rewrites a Mach-O binary's CPU type: a thin one's, or each image's of a universal one (in its
// header, big-endian, and in the image's own).
void setMachOCpu(const std::filesystem::path& binary, uint32_t cpu) {
    std::fstream f(binary, std::ios::in | std::ios::out | std::ios::binary);
    uint32_t magic = 0;
    f.read(reinterpret_cast<char*>(&magic), 4);
    if (__builtin_bswap32(magic) != 0xCAFEBABE) {
        f.seekp(4);
        f.write(reinterpret_cast<const char*>(&cpu), 4);
        return;
    }
    uint32_t count = 0;
    f.read(reinterpret_cast<char*>(&count), 4);
    for (uint32_t i = 0; i < __builtin_bswap32(count); ++i) {
        const uint32_t entry = 8 + i * 20, bigCpu = __builtin_bswap32(cpu);
        uint32_t offset = 0;
        f.seekg(entry + 8);
        f.read(reinterpret_cast<char*>(&offset), 4);
        f.seekp(entry);
        f.write(reinterpret_cast<const char*>(&bigCpu), 4);
        f.seekp(__builtin_bswap32(offset) + 4);
        f.write(reinterpret_cast<const char*>(&cpu), 4);
    }
}
#endif

void markArchitecture(const std::filesystem::path& path, const std::string& arch) {
#ifdef __APPLE__
    setMachOCpu(pluginBinaryPath(PluginFormat::Clap, path), arch == "arm64" ? 0x0100000C : arch == "x64" ? 0x01000007 : 7);
#else
    std::fstream f(path, std::ios::in | std::ios::out | std::ios::binary);
#endif
#ifdef _WIN32
    uint32_t at = 0;
    f.seekg(0x3C);
    f.read(reinterpret_cast<char*>(&at), 4);
    at += 4;
    const uint16_t machine = arch == "arm64" ? 0xAA64 : arch == "x64" ? 0x8664 : 0x014C;
#elif !defined(__APPLE__)
    const uint32_t at = 18;  // e_machine
    const uint16_t machine = arch == "arm64" ? 183 : arch == "x64" ? 62 : 3;
#endif
#ifndef __APPLE__
    f.seekp(at);
    f.write(reinterpret_cast<const char*>(&machine), sizeof machine);
#endif
}

// A plugin for an architecture this computer runs but this build ships no plugin host for (an x86
// build on an x64 kernel, an x64 build on Apple silicon): loading it says that, not that the
// computer does not run it.
void hostNotShipped(const std::string& synth) {
    std::string arch;
    std::error_code ec;
    for (const char* a : {"arm64", "x64", "x86"})
        if (a != std::string_view(buildArchitecture()) && architectureRunsHere(a) &&
            !std::filesystem::is_regular_file(pluginHostExecutable(a), ec)) {
            arch = a;
            break;
        }
#ifdef __linux__
    // An x86 build on an x64 kernel (an x64 PC, or FEX-Emu) runs x64 programs and ships no x64 host.
    utsname u{};
    if (std::string_view(buildArchitecture()) == "x86" && uname(&u) == 0 && std::string_view(u.machine) == "x86_64")
        CHECK(arch == "x64");
#endif
    if (arch.empty()) {
        std::printf("every architecture that runs here has its plugin host beside this build: nothing to refuse\n");
        return;
    }
    const auto dir = std::filesystem::temp_directory_path() / ("brack_test_isolation-" + std::string(buildArchitecture()) + "-unshipped");
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir);
    const auto copy = dir / (arch + "-synth.clap");
    std::filesystem::copy(pathFromUtf8(synth), copy, std::filesystem::copy_options::recursive);
    markArchitecture(copy, arch);
    Engine e;
    std::string err;
    start(e);
    CHECK(e.addPlugin({"p", pathToUtf8(copy), "", "", std::nullopt}, true, err).empty());
    std::printf("%s plugin, no plugin host for it here: %s\n", arch.c_str(), err.c_str());
    CHECK(has(err, "no plugin host for " + arch + " plugins"));
    CHECK(!has(err, "does not run"));
    std::filesystem::remove_all(dir, ec);
}

// A plugin for an architecture this computer does not run (a copy of the synth marked so):
// loading it says so, and a scan passes over it without a word.
void architectureNotRunHere(const std::string& synth) {
    std::string arch;
    for (const char* a : {"arm64", "x64", "x86"})
        if (!architectureRunsHere(a)) {
            arch = a;
            break;
        }
    if (arch.empty()) {
        std::printf("every architecture runs here: nothing to refuse\n");
        return;
    }
    const auto dir = std::filesystem::temp_directory_path() / ("brack_test_isolation-" + std::string(buildArchitecture()));
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir);
    const auto copy = dir / (arch + "-synth.clap");
    std::filesystem::copy(pathFromUtf8(synth), copy, std::filesystem::copy_options::recursive);
    markArchitecture(copy, arch);
    CHECK(pluginArchitecture(copy) == arch);
    Engine e;
    std::string err;
    start(e);
    CHECK(e.addPlugin({"p", pathToUtf8(copy), "", "", std::nullopt}, true, err).empty());
    std::printf("%s plugin here: %s\n", arch.c_str(), err.c_str());
    CHECK(has(err, "this computer does not run " + arch + " programs"));
    std::vector<std::string> warnings;
    setLogSink([&](LogLevel l, const std::string& m) {
        if (l >= LogLevel::Warning) warnings.push_back(m);
    });
    CHECK(Engine::scanPlugins({pathToUtf8(dir)}, {}, false).empty());
    // Nor from the scan cache, which a build that runs it may have written: the entry stays for
    // that build.
    PluginScanCache cache;
    PluginScanCache::Entry entry;
    entry.size = 0;
    entry.time = std::numeric_limits<int64_t>::min();
    const auto binary = pluginBinaryPath(PluginFormat::Clap, copy);
    for (const auto& f : std::filesystem::is_directory(copy) ? std::filesystem::recursive_directory_iterator(copy)
                                                              : std::filesystem::recursive_directory_iterator())
        if (f.is_regular_file()) {
            entry.size += f.file_size();
            entry.time = std::max<int64_t>(entry.time, f.last_write_time().time_since_epoch().count());
        }
    if (!std::filesystem::is_directory(copy)) {
        entry.size = std::filesystem::file_size(copy);
        entry.time = std::filesystem::last_write_time(copy).time_since_epoch().count();
    }
    PluginDescription d;
    d.name = "described by another build";
    d.path = pathToUtf8(copy);
    d.architecture = arch;
    entry.plugins.push_back(d);
    const std::string key = pathToUtf8(std::filesystem::weakly_canonical(copy));
    cache.files[key] = entry;
    CHECK(scanPlugins({dir}, &cache).empty());
    CHECK(cache.files.count(key) == 1);
    // Once the file has changed (its time alone), the entry is not used, and goes.
    std::filesystem::last_write_time(binary, std::filesystem::last_write_time(binary) + std::chrono::seconds(10));
    CHECK(scanPlugins({dir}, &cache).empty());
    CHECK(cache.files.count(key) == 0);
    printWarnings();
    CHECK(warnings.empty());
    std::filesystem::remove_all(dir, ec);
}

#ifndef _WIN32
// A plugin host gets Brack's end of a socket and the control block as descriptors 3 and 4. With
// Brack's 0 and 1 closed, those land on 0 or 1 and on 3 if it is free (macOS; on Linux Brack holds
// 3 and 4 by then): the host's own places. Moved there as they were, the socket overwrote the
// control block (exit code 100; it happened now and then when a session's hosts started at once).
void lowDescriptors(const std::string& synth) {
    Engine e;
    std::string err;
    start(e);
    std::fflush(stdout);
    const int in = fcntl(0, F_DUPFD_CLOEXEC, 100), out = fcntl(1, F_DUPFD_CLOEXEC, 100);
    close(0);
    close(1);
    const bool overlaps = fcntl(3, F_GETFD) < 0;
    const std::string id = e.addPlugin({"p", synth, "", "", std::nullopt}, true, err);
    // Back where Brack took none (its end of the socket may be 0 now).
    if (fcntl(0, F_GETFD) < 0) dup2(in, 0);
    if (fcntl(1, F_GETFD) < 0) dup2(out, 1);
    close(in);
    close(out);
    std::printf("plugin host started with descriptors 0, 1%s free: %s\n", overlaps ? " and 3" : " (not 3)",
                id.empty() ? err.c_str() : "loaded");
    CHECK(id == "p");
    const uint8_t on[] = {0x90, 0x3C, 0x64};
    CHECK(e.sendMidiToPlugin("p", 0, on, 3) == MidiSend::Sent);
    CHECK(renderPeaks(e, 4800)[0] > 0.1f);
}
#endif

// A path relative to Brack's current directory means the same to the host process.
void relativePath(const std::string& synth) {
    std::error_code ec;
    const std::string relative = pathToUtf8(std::filesystem::relative(pathFromUtf8(synth), ec));
    Engine e;
    std::string err;
    start(e);
    CHECK(!ec && e.addPlugin({"p", relative, "", "", std::nullopt}, true, err) == "p");
    std::printf("relative path %s: %s\n", relative.c_str(), err.empty() ? "loaded" : err.c_str());
    CHECK(e.snapshot().plugins.size() == 1 && e.snapshot().plugins[0].path == relative);
}

#ifdef _WIN32
// The ids of running processes whose parent is `parent`.
std::vector<DWORD> childrenOf(DWORD parent) {
    std::vector<DWORD> ids;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    PROCESSENTRY32W pe{sizeof pe};
    for (BOOL ok = Process32FirstW(snap, &pe); ok; ok = Process32NextW(snap, &pe))
        if (pe.th32ParentProcessID == parent) ids.push_back(pe.th32ProcessID);
    CloseHandle(snap);
    return ids;
}

// A Brack process killed outright (here, a copy of this test holding one plugin) takes its
// plugin host processes with it.
void endsWithBrack(const std::string& self, const std::string& synth) {
    SECURITY_ATTRIBUTES inherit{sizeof inherit, nullptr, TRUE};
    HANDLE readEnd = nullptr, writeEnd = nullptr;
    CreatePipe(&readEnd, &writeEnd, &inherit, 0);
    SetHandleInformation(readEnd, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOW si{sizeof si};
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = writeEnd;
    si.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    std::wstring cmd = L"\"" + widen(self) + L"\" --hold \"" + widen(synth) + L"\"";
    PROCESS_INFORMATION pi{};
    CHECK(CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE, 0, nullptr, nullptr, &si, &pi));
    CloseHandle(writeEnd);
    char ready[8] = {};
    DWORD n = 0;
    CHECK(ReadFile(readEnd, ready, 5, &n, nullptr) && std::string(ready, n) == "ready");
    const std::vector<DWORD> hosts = childrenOf(pi.dwProcessId);
    std::printf("Brack process %lu with %zu plugin host process(es), killed\n", pi.dwProcessId, hosts.size());
    CHECK(hosts.size() == 1);
    std::vector<HANDLE> handles;
    for (DWORD id : hosts) handles.push_back(OpenProcess(SYNCHRONIZE, FALSE, id));
    TerminateProcess(pi.hProcess, 1);
    for (HANDLE h : handles) {
        CHECK(h && WaitForSingleObject(h, 5000) == WAIT_OBJECT_0);
        if (h) CloseHandle(h);
    }
    CloseHandle(readEnd);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
}

// The editor's window fits the plugin's view, though the synth (like JUCE's editors) knows its size
// only once it has a parent window. The plugin host is per-monitor DPI aware as a whole: JUCE's
// editors scale by the process's awareness, and drew beyond their window when only the host thread
// was per-monitor aware.
void editorFits(const std::string& synth) {
    Engine e;
    start(e);
    std::string err;
    CHECK(e.addPlugin({"p", synth, "", "", std::nullopt}, false, err) == "p");
    CHECK(e.setPluginGuiVisible("p", true, err));
    const std::vector<DWORD> hosts = childrenOf(GetCurrentProcessId());
    CHECK(hosts.size() == 1);
    if (hosts.size() != 1) return;
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, hosts[0]);
    CHECK(process && GetAwarenessFromDpiAwarenessContext(GetDpiAwarenessContextForProcess(process)) ==
                         DPI_AWARENESS_PER_MONITOR_AWARE);
    if (process) CloseHandle(process);
    struct Find {
        DWORD pid;
        HWND frame;
    } find{hosts[0], nullptr};
    EnumWindows(
        [](HWND w, LPARAM l) {
            auto* f = reinterpret_cast<Find*>(l);
            DWORD pid = 0;
            GetWindowThreadProcessId(w, &pid);
            wchar_t cls[64] = {};
            GetClassNameW(w, cls, 64);
            if (pid == f->pid && !std::wcscmp(cls, L"BrackPluginEditor")) f->frame = w;
            return f->frame ? FALSE : TRUE;
        },
        reinterpret_cast<LPARAM>(&find));
    const HWND view = find.frame ? FindWindowExW(find.frame, nullptr, L"Static", nullptr) : nullptr;
    CHECK(view);
    RECT frame{}, viewRect{};
    if (view) {
        // In pixels, not this DPI-unaware test's scaled-down units.
        const DPI_AWARENESS_CONTEXT before = SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        GetClientRect(find.frame, &frame);
        GetClientRect(view, &viewRect);
        SetThreadDpiAwarenessContext(before);
    }
    std::printf("editor window %ldx%ld, plugin's view %ldx%ld\n", frame.right, frame.bottom, viewRect.right,
                viewRect.bottom);
    CHECK(view && frame.right == viewRect.right && frame.bottom == viewRect.bottom);
    CHECK(e.setPluginGuiVisible("p", false, err));
}

// The plugin host's audio thread joins MMCSS's "Pro Audio" task, which runs it in the real-time
// priority range (16 to 31). It joins with its first block, so blocks are rendered meanwhile.
void realtimeAudioThread(const std::string& synth) {
    Engine e;
    start(e);
    std::string err;
    CHECK(e.addPlugin({"p", synth, "", "", std::nullopt}, true, err) == "p");
    const std::vector<DWORD> hosts = childrenOf(GetCurrentProcessId());
    CHECK(hosts.size() == 1);
    if (hosts.size() != 1) return;
    // ThreadBasicInformation: the thread's current priority, which GetThreadPriority() does not tell.
    struct BasicInformation {
        LONG exitStatus;
        void* teb;
        void* clientId[2];
        ULONG_PTR affinityMask;
        LONG priority, basePriority;
    };
    using Query = LONG(NTAPI*)(HANDLE, int, void*, ULONG, ULONG*);
    const auto query = (Query)(void*)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQueryInformationThread");
    LONG highest = 0;
    for (int i = 0; i < 200 && highest < 16; ++i) {
        renderPeaks(e, 256);
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
        THREADENTRY32 te{sizeof te};
        for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
            if (te.th32OwnerProcessID != hosts[0]) continue;
            HANDLE t = OpenThread(THREAD_QUERY_INFORMATION, FALSE, te.th32ThreadID);
            BasicInformation info{};
            if (t && query(t, 0, &info, sizeof info, nullptr) == 0) highest = std::max(highest, info.priority);
            if (t) CloseHandle(t);
        }
        CloseHandle(snap);
        if (highest < 16) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    std::printf("plugin host's highest thread priority: %ld\n", highest);
    CHECK(highest >= 16);
}
#else
#ifdef BRACK_TEST_X11
}  // namespace
#include <X11/Xatom.h>
#include <X11/Xlib.h>
namespace {

std::string nameOf(Display* d, Window w) {
    char* n = nullptr;
    if (!XFetchName(d, w, &n) || !n) return {};
    std::string name = n;
    XFree(n);
    return name;
}

// The window under `w` whose name starts with `prefix`, if any.
Window findNamed(Display* d, Window w, const std::string& prefix) {
    if (nameOf(d, w).rfind(prefix, 0) == 0) return w;
    Window root = 0, parent = 0, *kids = nullptr;
    unsigned count = 0;
    if (!XQueryTree(d, w, &root, &parent, &kids, &count)) return 0;
    Window found = 0;
    for (unsigned i = 0; i < count && !found; ++i) found = findNamed(d, kids[i], prefix);
    if (kids) XFree(kids);
    return found;
}

// Waits up to 2 s for `w`'s name to hold `part`.
bool nameComes(Display* d, Window w, const std::string& part) {
    for (int i = 0; i < 200; ++i) {
        if (has(nameOf(d, w), part)) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return false;
}

// X resources (the root window's RESOURCE_MANAGER) with Xft.dpi set to `dpi`.
std::string withDpi(const std::string& resources, int dpi) {
    std::string out;
    size_t at = 0;
    while (at < resources.size()) {
        size_t end = resources.find('\n', at);
        if (end == std::string::npos) end = resources.size();
        const std::string line = resources.substr(at, end - at);
        if (line.rfind("Xft.dpi:", 0) != 0) out += line + "\n";
        at = end + 1;
    }
    return out + "Xft.dpi:\t" + std::to_string(dpi) + "\n";
}

// The editor's window fits the plugin's view, though the synth knows its size only once it has a
// parent window. The plugin host watches the synth's own X11 connection for it
// (posix-fd-support), and embeds it by XEmbed: the synth renames its view by what it has heard
// there (test_synth.cpp). Given the keyboard focus, the editor's window passes it on to the view.
// With BRACK_TEST_XFT_DPI=1, the desktop's Xft.dpi changes for a moment, and the editor hears of
// it (set_scale); not by default, since every program on the display sees it. Not tried without a
// display.
void editorFits(const std::string& synth) {
    Display* d = XOpenDisplay(nullptr);
    if (!d) {
        std::printf("editor: no X11 display, not tried\n");
        return;
    }
    Engine e;
    start(e);
    std::string err;
    CHECK(e.addPlugin({"p", synth, "", "", std::nullopt}, false, err) == "p");
    CHECK(e.setPluginGuiVisible("p", true, err));
    Window view = 0;
    for (int i = 0; i < 200 && !view; ++i) {
        view = findNamed(d, DefaultRootWindow(d), "brack test synth: embedded, exposed");
        if (!view) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    CHECK(view);
    unsigned vw = 0, vh = 0, fw = 0, fh = 0;
    Window frame = 0;
    if (view) {
        Window root = 0, *kids = nullptr;
        unsigned count = 0, border = 0, depth = 0;
        int x = 0, y = 0;
        XQueryTree(d, view, &root, &frame, &kids, &count);
        if (kids) XFree(kids);
        XGetGeometry(d, view, &root, &x, &y, &vw, &vh, &border, &depth);
        XGetGeometry(d, frame, &root, &x, &y, &fw, &fh, &border, &depth);
    }
    std::printf("editor window %ux%u, plugin's view %ux%u\n", fw, fh, vw, vh);
    CHECK(view && vw == 320 && vh == 200 && fw == vw && fh == vh);

    if (view) {
        XSetInputFocus(d, frame, RevertToParent, CurrentTime);
        XSync(d, False);
        Window focus = 0;
        for (int i = 0; i < 200 && focus != view; ++i) {
            int revert = 0;
            XGetInputFocus(d, &focus, &revert);
            if (focus != view) std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        const bool told = nameComes(d, view, "active, focused");
        std::printf("editor given the focus: on the plugin's view %s, %s\n", focus == view ? "yes" : "no",
                    nameOf(d, view).c_str());
        CHECK(focus == view && told);
    }

    const char* dpi = std::getenv("BRACK_TEST_XFT_DPI");
    if (view && dpi && !std::strcmp(dpi, "1")) {
        const Window root = DefaultRootWindow(d);
        const Atom resourceManager = XInternAtom(d, "RESOURCE_MANAGER", False);
        Atom type = 0;
        int format = 0;
        unsigned long count = 0, after = 0;
        unsigned char* data = nullptr;
        const bool had = XGetWindowProperty(d, root, resourceManager, 0, 1 << 20, False, XA_STRING, &type, &format, &count,
                                            &after, &data) == Success &&
                         data;
        const std::string before = had ? std::string(reinterpret_cast<char*>(data), count) : std::string();
        if (data) XFree(data);
        const std::string changed = withDpi(before, has(before, "Xft.dpi:\t144") ? 192 : 144);
        XChangeProperty(d, root, resourceManager, XA_STRING, 8, PropModeReplace,
                        reinterpret_cast<const unsigned char*>(changed.data()), (int)changed.size());
        XSync(d, False);
        const bool heard = nameComes(d, view, "scale ");
        std::printf("Xft.dpi changed: %s\n", nameOf(d, view).c_str());
        if (had)
            XChangeProperty(d, root, resourceManager, XA_STRING, 8, PropModeReplace,
                            reinterpret_cast<const unsigned char*>(before.data()), (int)before.size());
        else
            XDeleteProperty(d, root, resourceManager);
        XSync(d, False);
        CHECK(heard);
    }
    CHECK(e.setPluginGuiVisible("p", false, err));
    XCloseDisplay(d);
}
#endif

#ifdef __APPLE__
std::vector<pid_t> childrenOf(pid_t parent) {
    std::vector<pid_t> ids(256);
    const int n = proc_listchildpids(parent, ids.data(), (int)(ids.size() * sizeof(pid_t)));
    ids.resize(n > 0 ? (size_t)n : 0);
    std::erase_if(ids, [](pid_t pid) {
        proc_bsdinfo info{};
        return proc_pidinfo(pid, PROC_PIDTBSDINFO, 0, &info, sizeof info) != sizeof info || info.pbi_status == SZOMB;
    });
    return ids;
}

bool processExists(pid_t pid) {
    proc_bsdinfo info{};
    return proc_pidinfo(pid, PROC_PIDTBSDINFO, 0, &info, sizeof info) == sizeof info && info.pbi_status != SZOMB;
}
#else
// The ids of running processes whose parent is `parent` (from /proc).
std::vector<pid_t> childrenOf(pid_t parent) {
    std::vector<pid_t> ids;
    std::error_code ec;
    for (auto& entry : std::filesystem::directory_iterator("/proc", ec)) {
        std::ifstream stat(entry.path() / "stat");
        std::string line;
        if (!std::getline(stat, line)) continue;
        // pid (comm) state ppid ...: comm may hold spaces, so read from the last ')'.
        const size_t close = line.rfind(')');
        if (close == std::string::npos) continue;
        char state = 0;
        int ppid = 0;
        if (std::sscanf(line.c_str() + close + 1, " %c %d", &state, &ppid) == 2 && ppid == parent)
            ids.push_back((pid_t)std::atoi(line.c_str()));
    }
    return ids;
}

bool processExists(pid_t pid) {
    std::ifstream stat("/proc/" + std::to_string(pid) + "/stat");
    std::string line;
    if (!std::getline(stat, line)) return false;
    const size_t close = line.rfind(')');
    return close != std::string::npos && line.size() > close + 2 && line[close + 2] != 'Z';
}
#endif

// A Brack process killed outright (here, a copy of this test holding one plugin) takes its
// plugin host processes with it.
void endsWithBrack(const std::string& self, const std::string& synth) {
    int out[2];
    CHECK(pipe(out) == 0);
    const pid_t child = fork();
    if (child == 0) {
        dup2(out[1], 1);
        close(out[0]);
        close(out[1]);
        execl(self.c_str(), self.c_str(), "--hold", synth.c_str(), (char*)nullptr);
        _exit(127);
    }
    close(out[1]);
    char ready[8] = {};
    const ssize_t n = read(out[0], ready, 5);
    CHECK(n == 5 && std::string(ready, 5) == "ready");
    const std::vector<pid_t> hosts = childrenOf(child);
    std::printf("Brack process %d with %zu plugin host process(es), killed\n", (int)child, hosts.size());
    CHECK(hosts.size() == 1);
    kill(child, SIGKILL);
    waitpid(child, nullptr, 0);
    for (pid_t host : hosts) {
        bool gone = false;
        for (int i = 0; i < 500 && !gone; ++i) {
            gone = !processExists(host);
            if (!gone) std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        CHECK(gone);
    }
    close(out[0]);
}

#ifdef __linux__
// A VST3 plugin finds the host thread's run loop through its host context, editor or not, here and
// in a plugin host: the test synth runs a timer there and logs at terminate() whether it ticked.
void vst3RunLoop(const std::string& vst3) {
    for (bool inProcess : {true, false}) {
        const std::filesystem::path log =
            std::filesystem::temp_directory_path() / ("brack_test_isolation-run-loop-" + std::to_string(getpid()) + ".log");
        std::filesystem::remove(log);
        setenv("BRACK_TESTSYNTH_LOG", log.c_str(), 1);
        {
            Engine e;
            std::string err;
            EngineConfig cfg;
            cfg.pluginsInProcess = inProcess;
            CHECK(e.setConfig(cfg, err));
            CHECK(e.startManual(48000, 2, 256, err));
            CHECK(e.addPlugin({"p", vst3, "", "", std::nullopt}, true, err) == "p");
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }
        unsetenv("BRACK_TESTSYNTH_LOG");
        std::ifstream in(log);
        const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        std::printf("VST3 timer on the run loop of its host context (%s): %s\n", inProcess ? "in this process" : "in a plugin host",
                    has(text, "vst3 run loop: ticked") ? "ticked" : "did not tick");
        CHECK(has(text, "vst3 run loop: ticked"));
        std::filesystem::remove(log);
    }
}
#endif

#ifdef __APPLE__
std::vector<CGSize> windowSizes(pid_t pid) {
    std::vector<CGSize> sizes;
    CFArrayRef list = CGWindowListCopyWindowInfo(kCGWindowListOptionOnScreenOnly, kCGNullWindowID);
    if (!list) return sizes;
    for (NSDictionary* w in (__bridge NSArray*)list) {
        if ([w[(__bridge NSString*)kCGWindowOwnerPID] intValue] != pid) continue;
        CGRect bounds{};
        if (CGRectMakeWithDictionaryRepresentation((__bridge CFDictionaryRef)w[(__bridge NSString*)kCGWindowBounds], &bounds))
            sizes.push_back(bounds.size);
    }
    CFRelease(list);
    return sizes;
}

void editorFits(const std::string& synth) {
    const std::filesystem::path log =
        std::filesystem::temp_directory_path() / ("brack_test_isolation-editor-" + std::to_string(getpid()) + ".log");
    std::filesystem::remove(log);
    setenv("BRACK_TESTSYNTH_LOG", log.c_str(), 1);
    __block NSRect frame;
    dispatch_sync(dispatch_get_main_queue(), ^{
        frame = [NSWindow frameRectForContentRect:NSMakeRect(0, 0, 320, 200)
                                        styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                                                  NSWindowStyleMaskMiniaturizable];
    });
    for (bool inProcess : {false, true}) {
        Engine e;
        std::string err;
        EngineConfig cfg;
        cfg.pluginsInProcess = inProcess;
        CHECK(e.setConfig(cfg, err));
        CHECK(e.startManual(48000, 2, 256, err));
        CHECK(e.addPlugin({"p", synth, "", "", std::nullopt}, false, err) == "p");
        CHECK(e.setPluginGuiVisible("p", true, err));
        const std::vector<pid_t> hosts = childrenOf(getpid());
        const pid_t owner = inProcess ? getpid() : hosts.empty() ? -1 : hosts[0];
        bool fits = false;
        for (int i = 0; i < 200 && !fits; ++i) {
            for (CGSize size : windowSizes(owner)) fits = fits || CGSizeEqualToSize(size, frame.size);
            if (!fits) std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        std::printf("editor window (%s) %s %.0fx%.0f\n", inProcess ? "in this process" : "in a plugin host",
                    fits ? "is" : "is not", frame.size.width, frame.size.height);
        CHECK(fits);
        CHECK(e.setPluginGuiVisible("p", false, err));

        __block NSWindow* app = nil;
        dispatch_sync(dispatch_get_main_queue(), ^{
            app = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, 500, 400) styleMask:NSWindowStyleMaskTitled
                                                backing:NSBackingStoreBuffered defer:NO];
            app.releasedWhenClosed = NO;
        });
        const bool embedded = e.showPluginGuiIn("p", (__bridge void*)app.contentView, err);
        if (inProcess) {
            __block NSRect view = NSZeroRect;
            dispatch_sync(dispatch_get_main_queue(), ^{
                if (NSView* editor = app.contentView.subviews.firstObject) view = editor.frame;
            });
            std::printf("editor in the application's view: %s at %.0f,%.0f %.0fx%.0f\n", embedded ? "open" : err.c_str(),
                        view.origin.x, view.origin.y, view.size.width, view.size.height);
            CHECK(embedded && NSEqualRects(view, NSMakeRect(0, 200, 320, 200)));  // the top left, the parent not flipped
            CHECK(e.setPluginGuiVisible("p", false, err));
        } else {
            std::printf("editor in the application's view, from a plugin host: %s\n", err.c_str());
            CHECK(!embedded && has(err, "only with plugins in process"));
        }
        dispatch_sync(dispatch_get_main_queue(), ^{
            [app close];
        });
        CHECK(e.removePlugin("p", err));
    }
    unsetenv("BRACK_TESTSYNTH_LOG");
    std::ifstream in(log);
    const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    std::printf("%zu of 3 editors recorded a 320x200 view in one of that size\n", count(text, "editor: view 320x200 in 320x200"));
    CHECK(count(text, "editor: view 320x200 in 320x200") == 3);
    std::filesystem::remove(log);
}

void realtimeAudioThread(const std::string& synth) {
    std::vector<std::string> logged;
    std::mutex mutex;
    setLogSink([&](LogLevel, const std::string& m) {
        std::lock_guard lock(mutex);
        logged.push_back(m);
    });
    {
        Engine e;
        start(e);
        std::string err;
        CHECK(e.addPlugin({"p", synth, "", "", std::nullopt}, true, err) == "p");
        std::string found;
        for (int i = 0; i < 200 && found.empty(); ++i) {
            renderPeaks(e, 256);
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            std::lock_guard lock(mutex);
            for (auto& m : logged)
                if (has(m, "plugin hosts' audio threads")) found = m;
        }
        std::printf("%s\n", found.empty() ? "plugin hosts' audio threads: nothing logged" : found.c_str());
        CHECK(found == "plugin hosts' audio threads: real-time (time-constraint policy)");
    }
    printWarnings();
}
#else
// The plugin host's audio thread is real-time: SCHED_FIFO when the test runs with an rtprio limit,
// SCHED_RR when RealtimeKit grants it; at normal priority (printed) where this user may have neither.
void realtimeAudioThread(const std::string& synth) {
    Engine e;
    start(e);
    std::string err;
    CHECK(e.addPlugin({"p", synth, "", "", std::nullopt}, true, err) == "p");
    const std::vector<pid_t> hosts = childrenOf(getpid());
    CHECK(hosts.size() == 1);
    if (hosts.size() != 1) return;
    std::string found;
    for (int i = 0; i < 200 && found.empty(); ++i) {  // the host names its audio thread after "create"
        std::error_code ec;
        for (auto& task : std::filesystem::directory_iterator("/proc/" + std::to_string(hosts[0]) + "/task", ec)) {
            const pid_t tid = (pid_t)std::atoi(task.path().filename().c_str());
            const int policy = sched_getscheduler(tid) & ~SCHED_RESET_ON_FORK;
            sched_param param{};
            if ((policy == SCHED_FIFO || policy == SCHED_RR) && sched_getparam(tid, &param) == 0)
                found = std::string(policy == SCHED_FIFO ? "SCHED_FIFO " : "SCHED_RR ") + std::to_string(param.sched_priority);
        }
        if (found.empty()) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    rlimit rtprio{};
    getrlimit(RLIMIT_RTPRIO, &rtprio);
    std::printf("plugin host's audio thread: %s (rtprio limit %llu)\n", found.empty() ? "normal priority" : found.c_str(),
                (unsigned long long)rtprio.rlim_cur);
    if (rtprio.rlim_cur > 0) CHECK(found.rfind("SCHED_FIFO", 0) == 0);
}
#endif
#endif

// A session's plugins load at once, each in its plugin host: four that each take a second to take
// their state load in about the time of one. They land in the rack in the session's order, with
// one that fails among them kept in its place. Then the engine activates them at once too.
void parallelLoad(const std::string& synth) {
    std::vector<uint8_t> state(12);
    const float gain = 0.25f;
    const uint32_t ms = 1000;  // to load, and to activate
    std::memcpy(state.data(), &gain, 4);
    std::memcpy(state.data() + 4, &ms, 4);
    std::memcpy(state.data() + 8, &ms, 4);
    std::string path;
    for (char c : synth) path += c == '\\' ? std::string("\\\\") : std::string(1, c);
    const std::vector<std::string> ids = {"p0", "p1", "missing", "p2", "p3"};
    std::string plugins;
    for (auto& id : ids) {
        if (!plugins.empty()) plugins += ",";
        plugins += "{\"id\":\"" + id + "\",\"path\":\"" + (id == "missing" ? path.substr(0, path.size() - 5) + "-missing.clap" : path) +
                   "\",\"state\":\"" + base64Encode(state) + "\"}";
    }
    Engine e;
    std::string err;
    const auto began = std::chrono::steady_clock::now();
    CHECK(e.loadSessionJson("{\"format\":\"brack-session\",\"plugins\":[" + plugins + "]}", err));
    const auto took = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - began);
    std::printf("4 plugins taking %u ms each to load: %lld ms\n", ms, (long long)took.count());
    CHECK(took.count() < 2500);
    const EngineSnapshot snap = e.snapshot();
    CHECK(snap.plugins.size() == ids.size());
    for (size_t i = 0; i < snap.plugins.size() && i < ids.size(); ++i) {
        CHECK(snap.plugins[i].id == ids[i]);
        CHECK(snap.plugins[i].loaded == (ids[i] != "missing"));
        if (snap.plugins[i].loaded != (ids[i] != "missing"))
            std::fprintf(stderr, "  %s: %s\n", ids[i].c_str(), snap.plugins[i].status.c_str());
    }

    const auto starting = std::chrono::steady_clock::now();
    CHECK(e.startManual(48000, 2, 256, err));
    const auto started = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - starting);
    std::printf("4 plugins taking %u ms each to activate: %lld ms\n", ms, (long long)started.count());
    CHECK(started.count() < 2500);
    for (auto& p : e.snapshot().plugins) CHECK(p.active == (p.id != "missing"));
}

// With loadPluginsSerially, a session's plugins load, and activate, one after another: two that each
// take half a second take a second.
void serialLoad(const std::string& synth) {
    std::vector<uint8_t> state(12);
    const float gain = 0.25f;
    const uint32_t ms = 500;  // to load, and to activate
    std::memcpy(state.data(), &gain, 4);
    std::memcpy(state.data() + 4, &ms, 4);
    std::memcpy(state.data() + 8, &ms, 4);
    std::string path;
    for (char c : synth) path += c == '\\' ? std::string("\\\\") : std::string(1, c);
    std::string plugins;
    for (const char* id : {"p0", "p1"}) {
        if (!plugins.empty()) plugins += ",";
        plugins += std::string("{\"id\":\"") + id + "\",\"path\":\"" + path + "\",\"state\":\"" + base64Encode(state) + "\"}";
    }
    Engine e;
    std::string err;
    const auto began = std::chrono::steady_clock::now();
    CHECK(e.loadSessionJson("{\"format\":\"brack-session\",\"engine\":{\"loadPluginsSerially\":true},\"plugins\":[" + plugins +
                                "]}",
                            err));
    const auto took = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - began);
    std::printf("2 plugins taking %u ms each to load, one after another: %lld ms\n", ms, (long long)took.count());
    CHECK(e.config().loadPluginsSerially);
    CHECK(took.count() >= 2 * ms);
    for (auto& p : e.snapshot().plugins) CHECK(p.loaded);

    const auto starting = std::chrono::steady_clock::now();
    CHECK(e.startManual(48000, 2, 256, err));
    const auto started = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - starting);
    std::printf("2 plugins taking %u ms each to activate, one after another: %lld ms\n", ms, (long long)started.count());
    CHECK(started.count() >= 2 * ms);
    for (auto& p : e.snapshot().plugins) CHECK(p.active);
}

int hold(const std::string& synth) {
    Engine e;
    std::string err;
    if (!e.startManual(48000, 2, 256, err) || e.addPlugin({"p", synth, "", "", std::nullopt}, true, err).empty()) {
        std::fprintf(stderr, "hold: %s\n", err.c_str());
        return 1;
    }
    std::fputs("ready", stdout);
    std::fflush(stdout);
    for (;;) std::this_thread::sleep_for(std::chrono::seconds(1));
}

// The rendering thread's priority, raised as Brack raises its audio output thread's.
std::string raiseThisThread() {
#ifdef _WIN32
    DWORD task = 0;
    return AvSetMmThreadCharacteristicsW(L"Pro Audio", &task) ? "real-time (MMCSS Pro Audio)" : "normal priority";
#elif defined(__APPLE__)
    const std::string why = realtime::makeThisThreadRealtime(48000, 256);
    return why.empty() ? "real-time (time-constraint policy)" : "normal priority (" + why + ")";
#else
    realtime::fallBackOnOverrun();
    realtime::makeRealtime(getpid(), realtime::threadId(), "rendering thread");
    const int policy = sched_getscheduler(0) & ~SCHED_RESET_ON_FORK;
    sched_param param{};
    sched_getparam(0, &param);
    if (policy != SCHED_FIFO && policy != SCHED_RR) return "normal priority";
    return std::string(policy == SCHED_FIFO ? "SCHED_FIFO " : "SCHED_RR ") + std::to_string(param.sched_priority);
#endif
}

#ifdef __APPLE__
struct SchedCounts {
    uint64_t sleeps = 0, migrations = 0;
};
SchedCounts schedCounts(pid_t pid) {
    proc_taskinfo info{};
    if (proc_pidinfo(pid, PROC_PIDTASKINFO, 0, &info, sizeof info) != sizeof info) return {};
    return {(uint64_t)info.pti_csw, 0};  // macOS counts no migrations
}
SchedCounts hostsSchedCounts() {
    SchedCounts all;
    for (pid_t host : childrenOf(getpid())) all.sleeps += schedCounts(host).sleeps;
    return all;
}
SchedCounts ownSchedCounts() { return schedCounts(getpid()); }
#elif !defined(_WIN32)
// Scheduler counts of a process's threads, summed: how often they slept (voluntary context
// switches) and moved to another CPU.
struct SchedCounts {
    uint64_t sleeps = 0, migrations = 0;
};
SchedCounts schedCounts(pid_t pid) {
    SchedCounts c;
    std::error_code ec;
    for (auto& task : std::filesystem::directory_iterator("/proc/" + std::to_string(pid) + "/task", ec)) {
        std::ifstream status(task.path() / "status"), sched(task.path() / "sched");
        for (std::string line; std::getline(status, line);)
            if (line.rfind("voluntary_ctxt_switches:", 0) == 0) c.sleeps += std::strtoull(line.c_str() + 24, nullptr, 10);
        for (std::string line; std::getline(sched, line);)
            if (line.rfind("se.nr_migrations", 0) == 0) c.migrations += std::strtoull(line.c_str() + line.find(':') + 1, nullptr, 10);
    }
    return c;
}
SchedCounts hostsSchedCounts() {
    SchedCounts all;
    for (pid_t host : childrenOf(getpid())) {
        const SchedCounts c = schedCounts(host);
        all.sleeps += c.sleeps;
        all.migrations += c.migrations;
    }
    return all;
}
SchedCounts ownSchedCounts() {
    SchedCounts c;
    std::ifstream status("/proc/thread-self/status"), sched("/proc/thread-self/sched");
    for (std::string line; std::getline(status, line);)
        if (line.rfind("voluntary_ctxt_switches:", 0) == 0) c.sleeps = std::strtoull(line.c_str() + 24, nullptr, 10);
    for (std::string line; std::getline(sched, line);)
        if (line.rfind("se.nr_migrations", 0) == 0) c.migrations = std::strtoull(line.c_str() + line.find(':') + 1, nullptr, 10);
    return c;
}
#endif

// What a block costs with plugins in this process and in host processes (printed, not
// checked: it depends on the machine). Rendered on a real-time thread, where this user may have
// that, in runs short enough for RealtimeKit's limit on running without sleeping. On Linux, also
// how often per block, in host processes, the plugin hosts' threads and the rendering thread
// slept and moved to another CPU. Blocks back to back, or `paced` as a device asks for them: each
// at its own time (64 frames: every 1.33 ms), the time between them spent asleep.
void blockCost(const std::string& synth, const std::vector<uint32_t>& frameCounts, const std::vector<int>& pluginCounts,
               bool paced = false) {
    std::printf("rendering thread: %s\n", raiseThisThread().c_str());
    for (uint32_t frames : frameCounts) {
        for (int plugins : pluginCounts) {
            double us[2] = {};
            std::string sched;
            for (bool inProcess : {true, false}) {
                Engine e;
                std::string err;
                EngineConfig cfg;
                cfg.pluginsInProcess = inProcess;
                cfg.blockSize = frames;
                CHECK(e.setConfig(cfg, err));
                CHECK(e.startManual(48000, 2, frames, err));
                for (int i = 0; i < plugins; ++i) {
                    const std::string id = "p" + std::to_string(i);
                    CHECK(e.addPlugin({id, synth, "brack.test.synth", "", std::nullopt}, true, err) == id);
                    const uint8_t on[] = {0x90, (uint8_t)(48 + i), 0x64};
                    e.sendMidiToPlugin(id, 0, on, 3);
                }
                std::vector<std::vector<float>> buf(2, std::vector<float>(frames));
                float* ptrs[2] = {buf[0].data(), buf[1].data()};
                for (int i = 0; i < 200; ++i) e.render(ptrs, 2, frames);  // warm up
                const int runs = paced ? 1 : 8, blocks = paced ? 400 : 500;
                const auto period = std::chrono::nanoseconds((int64_t)frames * 1000000000 / 48000);
                std::chrono::steady_clock::duration took{};
#ifndef _WIN32
                SchedCounts hosts{}, own{};
#endif
                for (int r = 0; r < runs; ++r) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
#ifndef _WIN32
                    const SchedCounts hosts0 = hostsSchedCounts(), own0 = ownSchedCounts();
#endif
                    if (paced) {
                        auto next = std::chrono::steady_clock::now();
                        for (int i = 0; i < blocks; ++i) {
                            next += period;
                            std::this_thread::sleep_until(next);
                            const auto t0 = std::chrono::steady_clock::now();
                            e.render(ptrs, 2, frames);
                            took += std::chrono::steady_clock::now() - t0;
                        }
                    } else {
                        const auto t0 = std::chrono::steady_clock::now();
                        for (int i = 0; i < blocks; ++i) e.render(ptrs, 2, frames);
                        took += std::chrono::steady_clock::now() - t0;
                    }
#ifndef _WIN32
                    const SchedCounts hosts1 = hostsSchedCounts(), own1 = ownSchedCounts();
                    hosts.sleeps += hosts1.sleeps - hosts0.sleeps;
                    hosts.migrations += hosts1.migrations - hosts0.migrations;
                    own.sleeps += own1.sleeps - own0.sleeps;
                    own.migrations += own1.migrations - own0.migrations;
#endif
                }
                us[inProcess ? 0 : 1] = std::chrono::duration<double, std::micro>(took).count() / (runs * blocks);
#ifndef _WIN32
                if (!inProcess) {
                    char buf[160];
                    const double n = runs * blocks;
                    std::snprintf(buf, sizeof buf, "; per block: hosts slept %.2f, moved %.2f; rendering thread slept %.2f, moved %.2f",
                                  hosts.sleeps / n, hosts.migrations / n, own.sleeps / n, own.migrations / n);
                    sched = buf;
                }
#endif
            }
            std::printf("%3u frames (%.2f ms)%s, %d plugin(s): %.1f us in this process, %.1f us in host processes%s\n",
                        frames, frames * 1000.0 / 48000, paced ? " paced" : "", plugins, us[0], us[1], sched.c_str());
        }
    }
}

}  // namespace

int run(int argc, char** argv) {
    if (argc == 3 && std::string(argv[1]) == "--hold") return hold(argv[2]);
    // Alone, and one block size at a time: RealtimeKit allows a user 25 requests in 20 s.
    if ((argc == 3 || argc == 4) && std::string(argv[1]) == "--block-cost") {
        printWarnings();
        const std::vector<uint32_t> frames = argc == 4 ? std::vector<uint32_t>{(uint32_t)std::atoi(argv[3])}
                                                       : std::vector<uint32_t>{64, 256};
        blockCost(argv[2], frames, {1, 2, 4, 8});
        blockCost(argv[2], frames, {1, 2, 4, 8}, true);
        setLogSink(nullptr);
        return g_failures ? 1 : 0;
    }
    if (argc < 4) {
        std::fprintf(stderr, "usage: test_isolation <brack-test-synth.clap> <vst2 synth> <vst3 synth>\n");
        return 2;
    }
    const std::string synth = argv[1];
    printWarnings();

#ifdef _WIN32
    const Kind kinds[] = {
        {0x03, "access violation on a thread of its own", "crashed in a thread or window of its own: access violation", false},
        {0x07, "access violation in its window procedure", "crashed in a thread or window of its own: access violation", true},
        // The synth's CRT (a static one) ends abort() with __fastfail(), where the CPU has it: not
        // x86 programs under ARM64 Windows' emulation, where it exits with 3.
        {0x04, "abort()",
         IsProcessorFeaturePresent(PF_FASTFAIL_AVAILABLE) ? "crashed: fast fail (0xC0000409)" : "ended unexpectedly (exit code 3)",
         false},
        {0x05, "__fastfail()", "crashed: fast fail (0xC0000409)", false},
        {0x06, "process() never returns", "stopped responding in process", false},
        {0x0A, "stack overflow on a thread of its own", "crashed in a thread or window of its own: stack overflow", false},
    };
#else
    // Outside a guarded call, a signal the plugin host records (brack_link_posix.cpp); inside
    // process(), the guard's report (an illegal instruction on x86, a trap on arm64). The synth
    // has no editor here.
    const Kind kinds[] = {
        {0x03, "segmentation fault on a thread of its own", "crashed in the plugin: segmentation fault", false},
#ifdef __APPLE__
        // Recorded from the Mach exception, the plugin's thread having no stack for the handler.
        {0x0A, "stack overflow on a thread of its own", "crashed in the plugin: stack overflow, bus error (0x0000000A) at brack-test-synth+",
         false},
#else
        // Recorded on a stack of its own, where it tells where (plugin_threads_linux.cpp).
        {0x0A, "stack overflow on a thread of its own", "crashed in the plugin: segmentation fault (0x0000000B) at brack-test-synth.clap+", false},
#endif
        {0x04, "abort()", "crashed in the plugin: abort", false},
        {0x05, "__builtin_trap()", "crashed in process:", false},
        {0x06, "process() never returns", "stopped responding in process", false},
    };
#endif
#ifdef __APPLE__
    realtimeAudioThread(synth);  // first: Brack logs the outcome only when it changes
#endif
    for (const Kind& k : kinds) goesDown(synth, k);

    for (int i = 1; i <= 3; ++i) soundsTheSame(argv[i]);
    for (int i = 4; i < argc; ++i) otherArchitecture(synth, argv[i]);
    relativePath(synth);
#ifndef _WIN32
    lowDescriptors(synth);
#endif
    architectureNotRunHere(synth);
    hostNotShipped(synth);
    endsWithBrack(argv[0], synth);
#if defined(_WIN32) || defined(__APPLE__) || defined(BRACK_TEST_X11)
    editorFits(synth);
#endif
#ifndef __APPLE__
    realtimeAudioThread(synth);
#endif
#ifdef __linux__
    vst3RunLoop(argv[3]);
#endif
    parallelLoad(synth);
    serialLoad(synth);
    blockCost(synth, {64, 256}, {1, 8});

    setLogSink(nullptr);
    if (g_failures) {
        std::fprintf(stderr, "%d check(s) failed\n", g_failures);
        return 1;
    }
    std::puts("all checks passed");
    return 0;
}

int main(int argc, char** argv) {
    return runWithMainLoop([&] { return run(argc, argv); });
}
