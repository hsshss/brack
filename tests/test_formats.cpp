// VST2 and VST3 hosting, end to end in manual render mode with the test synths:
// scanning (a VST3 bundle, a VST2 library next to a library that is not a plugin), MIDI in, audio
// out, MIDI CC to a parameter, and state through a session round trip.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "engine.h"
#include "plugin/plugin_files.h"
#include "util/common.h"
#include "util/main_thread.h"

using namespace brack;
namespace fs = std::filesystem;

namespace {

int g_failures = 0;
#define CHECK(cond)                                                                \
    do {                                                                           \
        if (!(cond)) {                                                             \
            std::fprintf(stderr, "CHECK failed: %s (line %d)\n", #cond, __LINE__); \
            ++g_failures;                                                          \
        }                                                                          \
    } while (0)

float renderPeak(Engine& e, uint32_t frames, uint32_t total) {
    std::vector<std::vector<float>> buf(2, std::vector<float>(frames));
    float* ptrs[2] = {buf[0].data(), buf[1].data()};
    float peak = 0;
    for (uint32_t done = 0; done < total; done += frames) {
        e.render(ptrs, 2, frames);
        for (auto& b : buf)
            for (float v : b) peak = std::max(peak, std::fabs(v));
    }
    return peak;
}

std::vector<std::string> readLines(const fs::path& p) {
    std::vector<std::string> lines;
    std::ifstream f(p);
    for (std::string l; std::getline(f, l);) lines.push_back(l);
    return lines;
}

bool contains(const std::vector<std::string>& lines, const std::string& s) {
    for (auto& l : lines)
        if (l == s) return true;
    return false;
}

// Plays a note, a SysEx message and CC 7 = 32 (gain to about a quarter) through the plugin;
// returns the session saved afterwards. Peaks: with the note at full and quarter gain.
std::string play(const std::string& path, PluginFormat format, float& fullPeak) {
    Engine e;
    std::string err;
    CHECK(e.startManual(48000, 2, 256, err));
    std::string id = e.addPlugin({"synth", path, "", "", std::nullopt}, true, err);
    if (id != "synth") std::fprintf(stderr, "addPlugin: %s\n", err.c_str());
    CHECK(id == "synth");
    auto snap = e.snapshot();
    CHECK(snap.plugins.size() == 1);
    if (snap.plugins.empty()) return {};
    auto& p = snap.plugins[0];
    CHECK(p.format == format);
    CHECK(p.active && !p.failed);
    CHECK(p.notePorts.size() == 1);
    CHECK(p.audioOutputs.size() == 1 && p.audioOutputs[0].channels == 2);
    CHECK(e.addMidiSource({"keys", MidiSourceKind::Api, ""}, err) == "keys");
    CHECK(e.addMidiRoute({"keys", "synth", 0}, err));

    CHECK(renderPeak(e, 256, 4800) < 1e-6f);
    const uint8_t on[] = {0x90, 0x3C, 0x64}, off[] = {0x80, 0x3C, 0x00}, cc[] = {0xB0, 0x07, 0x20};
    const uint8_t sysex[] = {0xF0, 0x7E, 0x7F, 0x06, 0x01, 0xF7};
    CHECK(e.sendMidiToSource("keys", on, 3) == MidiSend::Sent);
    CHECK(e.sendMidiToPlugin("synth", 0, sysex, sizeof sysex) == MidiSend::Sent);
    fullPeak = renderPeak(e, 256, 9600);
    std::printf("%s peak: %.3f\n", pluginFormatLabel(format), fullPeak);
    CHECK(fullPeak > 0.1f);
    CHECK(e.sendMidiToSource("keys", off, 3) == MidiSend::Sent);
    renderPeak(e, 256, 2400);
    CHECK(renderPeak(e, 256, 2400) < 1e-6f);
    const uint64_t changes = e.changeCount();
    CHECK(e.sendMidiToSource("keys", cc, 3) == MidiSend::Sent);
    renderPeak(e, 256, 256);
    // The VST2 synth reports the change (audioMasterAutomate); MIDI alone counts for nothing.
    if (format == PluginFormat::Vst2) CHECK(e.changeCount() > changes);
    else CHECK(e.changeCount() == changes);
    return e.saveSessionJson();
}

void testFormat(const std::string& path, PluginFormat format, const fs::path& logPath) {
    fs::remove(logPath);
    float fullPeak = 0;
    std::string session = play(path, format, fullPeak);

    // The state (gain set by CC 7) survives a session round trip.
    {
        Engine e;
        std::string err;
        CHECK(e.loadSessionJson(session, err));
        CHECK(e.snapshot().plugins.size() == 1 && e.snapshot().plugins[0].loaded);
        CHECK(e.startManual(48000, 2, 256, err));
        const uint8_t on[] = {0x90, 0x3C, 0x64};
        CHECK(e.sendMidiToPlugin("synth", 0, on, 3) == MidiSend::Sent);
        float peak = renderPeak(e, 256, 9600);
        std::printf("%s peak after CC 7 = 32, reloaded: %.3f\n", pluginFormatLabel(format), peak);
        CHECK(peak > fullPeak * 0.15f && peak < fullPeak * 0.35f);
    }

    auto lines = readLines(logPath);
    if (format == PluginFormat::Vst2) {
        CHECK(contains(lines, "vst2 resume 1"));
        CHECK(contains(lines, "vst2 process-begin"));
        CHECK(contains(lines, "vst2 host took events: 1"));  // audioMasterProcessEvents is 8
        CHECK(contains(lines, "vst2 midi: 90 3C 64"));
        CHECK(contains(lines, "vst2 sysex: F0 7E 7F 06 01 F7"));
        CHECK(contains(lines, "vst2 midi: 80 3C 00"));
        CHECK(contains(lines, "vst2 midi: B0 07 20"));
        CHECK(contains(lines, "vst2 process-end"));
    } else {
        CHECK(contains(lines, "vst3 active 1"));
        CHECK(contains(lines, "vst3 processing 1"));
        CHECK(contains(lines, "vst3 note-on bus=0 ch=0 key=60 vel=0.787"));
        CHECK(contains(lines, "vst3 sysex: F0 7E 7F 06 01 F7"));
        CHECK(contains(lines, "vst3 note-off bus=0 ch=0 key=60"));
        CHECK(contains(lines, "vst3 param 1 = 0.252"));  // CC 7 = 32 through IMidiMapping
        CHECK(contains(lines, "vst3 processing 0"));
    }
    if (g_failures)
        for (auto& l : lines) std::fprintf(stderr, "  log: %s\n", l.c_str());
}

uint32_t swap32(uint32_t v) { return (v >> 24) | ((v >> 8) & 0xFF00) | ((v << 8) & 0xFF0000) | (v << 24); }

// Rewrites a Mach-O binary's CPU type: a thin one's, or each image's of a universal one (in its
// header, big-endian, and in the image's own).
void setMachOCpu(const std::filesystem::path& binary, uint32_t cpu) {
    std::fstream f(binary, std::ios::in | std::ios::out | std::ios::binary);
    uint32_t magic = 0;
    f.read(reinterpret_cast<char*>(&magic), 4);
    if (swap32(magic) != 0xCAFEBABE) {
        f.seekp(4);
        f.write(reinterpret_cast<const char*>(&cpu), 4);
        return;
    }
    uint32_t count = 0;
    f.read(reinterpret_cast<char*>(&count), 4);
    for (uint32_t i = 0; i < swap32(count); ++i) {
        const uint32_t entry = 8 + i * 20, bigCpu = swap32(cpu);
        uint32_t offset = 0;
        f.seekg(entry + 8);
        f.read(reinterpret_cast<char*>(&offset), 4);
        f.seekp(entry);
        f.write(reinterpret_cast<const char*>(&bigCpu), 4);
        f.seekp(swap32(offset) + 4);
        f.write(reinterpret_cast<const char*>(&cpu), 4);
    }
}

void testScan(const fs::path& vst2, const fs::path& vst3) {
    const fs::path dir = fs::temp_directory_path() / ("brack_test_scan-" + std::string(buildArchitecture()));
    fs::remove_all(dir);
    // A bundle: Name.vst3/Contents/<arch folder>/<binary> (Name.vst3 on Windows, Name.so on Linux).
    auto binaryIn = [](const fs::path& bundle) {
        return kCurrentOs == TargetOs::Linux ? fs::path(pathToUtf8(bundle.stem()) + ".so") : bundle.filename();
    };
    const bool bundles = kCurrentOs == TargetOs::MacOS;
    const fs::path bundle = dir / "vst3" / vst3.filename();
    // This build's own folder, not the ARM64EC one an x64 build on an ARM64 PC searches first.
    const std::string arch = vst3ArchFolders(kCurrentOs, buildArchitecture(), buildArchitecture()).front();
    if (bundles) {
        fs::create_directories(bundle);
        fs::copy(vst3, bundle, fs::copy_options::recursive);
    } else {
        fs::create_directories(bundle / "Contents" / arch);
        fs::copy_file(vst3, bundle / "Contents" / arch / binaryIn(bundle));
    }
    fs::create_directories(dir / "vst2");
    if (bundles) fs::create_directories(dir / "vst2" / vst2.filename());
    fs::copy(vst2, dir / "vst2" / vst2.filename(), fs::copy_options::recursive);
    // A library that is not a plugin, as plugin folders often have: skipped without being loaded.
    const fs::path library = kCurrentOs == TargetOs::Linux   ? "libbrack.so"
                             : kCurrentOs == TargetOs::MacOS ? "libbrack.dylib"
                                                             : "brack.dll";
    const fs::path notPlugin = dir / "vst2" / library;
    if (fs::exists(vst2.parent_path() / library)) fs::copy_file(vst2.parent_path() / library, notPlugin);
    // Plugins for an architecture no plugin host here runs, as folders other builds of Brack
    // search hold: skipped without a warning. A VST2 library whose machine (PE, ELF) says so, and
    // a bundle with only that architecture's folder. (test_isolation runs the other architectures'
    // that do run here.) Windows and Linux: ARM64, but RISC-V on ARM64, where the x64 and x86
    // plugin hosts run too (and no bundle, since VST3 names no folder for RISC-V). macOS: x86,
    // which it no longer runs, as Mach-O headers say it.
    const bool riscv = std::string(buildArchitecture()) == "arm64" && kCurrentOs != TargetOs::MacOS;
    constexpr uint32_t kMachOX86 = 7;
    if (bundles) {
        const fs::path foreign = dir / "vst2" / ("other-arch" + vst2.extension().string());
        fs::create_directories(foreign);
        fs::copy(vst2, foreign, fs::copy_options::recursive);
        setMachOCpu(pluginBinaryPath(PluginFormat::Vst2, foreign), kMachOX86);
        const fs::path foreignBundle = dir / "vst3" / "other-arch.vst3";
        fs::create_directories(foreignBundle);
        fs::copy(vst3, foreignBundle, fs::copy_options::recursive);
        setMachOCpu(pluginBinaryPath(PluginFormat::Vst3, foreignBundle), kMachOX86);
    } else {
        const fs::path foreign = dir / "vst2" / ("other-arch" + vst2.extension().string());
        fs::copy_file(vst2, foreign);
        std::fstream f(foreign, std::ios::in | std::ios::out | std::ios::binary);
        uint32_t at = 18;  // ELF: e_machine
        uint16_t machine = riscv ? 243 : 183;  // EM_RISCV, EM_AARCH64
        if constexpr (kCurrentOs == TargetOs::Windows) {
            f.seekg(0x3C);  // PE: the header's offset, then its machine
            f.read(reinterpret_cast<char*>(&at), 4);
            at += 4;
            machine = riscv ? 0x5064 : 0xAA64;  // RISC-V 64, ARM64
        }
        f.seekp(at);
        f.write(reinterpret_cast<const char*>(&machine), 2);
    }
    if (!bundles && !riscv) {
        const fs::path foreignBundle = dir / "vst3" / "other-arch.vst3";
        const std::string folder = vst3ArchFolders(kCurrentOs, "arm64", "arm64").back();
        fs::create_directories(foreignBundle / "Contents" / folder);
        fs::copy_file(vst3, foreignBundle / "Contents" / folder / binaryIn(foreignBundle));
    }
    std::vector<std::string> warnings;
    setLogSink([&](LogLevel l, const std::string& m) {
        if (l >= LogLevel::Warning) warnings.push_back(m);
    });

    auto found = scanPlugins({dir});
    setLogSink(nullptr);
    for (auto& w : warnings) {
        std::printf("scan warning: %s\n", w.c_str());
        CHECK(w.find("other-arch") == std::string::npos);
    }
    CHECK(found.size() == 2);
    bool seen2 = false, seen3 = false;
    for (auto& d : found) {
        std::printf("scan: %s %s '%s' %s %s\n", pluginFormatLabel(d.format), d.id.c_str(), d.name.c_str(),
                    d.isInstrument() ? "instrument" : "-", d.path.c_str());
        if (d.format == PluginFormat::Vst2) {
            seen2 = true;
            CHECK(d.id == "BrT2");
            CHECK(d.name == "brack test synth vst2");
            CHECK(d.vendor == "brack");
            CHECK(d.isInstrument());
        } else if (d.format == PluginFormat::Vst3) {
            seen3 = true;
            CHECK(d.id == "6272616B546573745653543353796E31");
            CHECK(d.name == "brack test synth vst3");
            CHECK(d.isInstrument());
            CHECK(pathFromUtf8(d.path) == fs::weakly_canonical(bundle));  // the bundle, not the binary inside
        }
    }
    CHECK(seen2 && seen3);

    // Windows: every build looks in the 64-bit and the 32-bit Common Files both. Linux: the user's
    // folder and the system's.
    {
        const auto standard = defaultPluginSearchPaths(PluginFormat::Vst3);
        auto searched = [&](const fs::path& p) { return std::find(standard.begin(), standard.end(), p) != standard.end(); };
        auto underVar = [&](const char* var, const char* sub) {
            const char* v = std::getenv(var);
            return v && searched(fs::path(v) / sub);
        };
        if constexpr (kCurrentOs == TargetOs::Windows) {
            CHECK(underVar("CommonProgramW6432", "VST3"));
            CHECK(underVar("CommonProgramFiles(x86)", "VST3"));
        } else if constexpr (kCurrentOs == TargetOs::MacOS) {
            CHECK(underVar("HOME", "Library/Audio/Plug-Ins/VST3"));
            CHECK(searched("/Library/Audio/Plug-Ins/VST3"));
        } else {
            CHECK(underVar("HOME", ".vst3"));
            CHECK(searched("/usr/lib/vst3"));
        }
    }

    // The scan cache: unchanged files come from it (proved by a doctored name), a changed
    // file is described again, and it survives a save and load.
    {
        PluginScanCache cache;
        CHECK(scanPlugins({dir}, &cache).size() == 2);
        CHECK(cache.files.size() == 2);
        const fs::path cacheFile = dir / "cache.json";
        std::string err;
        CHECK(cache.save(cacheFile, err));
        PluginScanCache loaded;
        CHECK(loaded.load(cacheFile));
        CHECK(loaded.files.size() == 2);
        const std::string vst2Key = pathToUtf8(fs::weakly_canonical(dir / "vst2" / vst2.filename()));
        CHECK(loaded.files.count(vst2Key) == 1);
        // The architecture is this build's own choice, whichever build wrote the cache.
        if (loaded.files.count(vst2Key)) {
            loaded.files[vst2Key].plugins.at(0).name = "from cache";
            loaded.files[vst2Key].plugins.at(0).architecture = "another";
        }
        auto names = [](const std::vector<PluginDescription>& ds) {
            std::vector<std::string> n;
            for (auto& d : ds) n.push_back(d.name);
            return n;
        };
        const auto cached = scanPlugins({dir}, &loaded);
        const auto fromCache = std::find_if(cached.begin(), cached.end(), [](auto& d) { return d.name == "from cache"; });
        CHECK(fromCache != cached.end() && fromCache->architecture == buildArchitecture());
        auto again = names(cached);
        fs::last_write_time(pluginBinaryPath(PluginFormat::Vst2, dir / "vst2" / vst2.filename()), fs::file_time_type::clock::now());
        again = names(scanPlugins({dir}, &loaded));
        CHECK(std::find(again.begin(), again.end(), "brack test synth vst2") != again.end());
        CHECK(std::find(again.begin(), again.end(), "from cache") == again.end());
    }

    // Loading from the bundle folder, by class id.
    Engine e;
    std::string err;
    CHECK(!e.addPlugin({"x", pathToUtf8(bundle), "6272616B546573745653543353796E31", "", std::nullopt}, false, err).empty());
    CHECK(e.addPlugin({"y", pathToUtf8(bundle), "00000000000000000000000000000000", "", std::nullopt}, false, err).empty());
    CHECK(e.addPlugin({"z", pathToUtf8(notPlugin), "", "", std::nullopt}, false, err).empty());
    std::printf("not a plugin: %s\n", err.c_str());
    e.clear();
    // Without the error code: under load, something on Windows kept a file here open after the
    // plugin hosts had ended, and the exception ended the test (0xC0000409; design notes, ch. 15).
    std::error_code ec;
    fs::remove_all(dir, ec);
    if (ec) std::printf("left %s behind: %s\n", pathToUtf8(dir).c_str(), ec.message().c_str());
}

}  // namespace

int run(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: test_formats <test-synth-vst2 library> <test-synth.vst3>\n");
        return 2;
    }
    const fs::path logPath = fs::temp_directory_path() / ("brack_test_formats-" + std::string(buildArchitecture()) + ".log");
#ifdef _WIN32
    _putenv(("BRACK_TESTSYNTH_LOG=" + pathToUtf8(logPath)).c_str());
#else
    setenv("BRACK_TESTSYNTH_LOG", logPath.c_str(), 1);
#endif

    testFormat(argv[1], PluginFormat::Vst2, logPath);
    testFormat(argv[2], PluginFormat::Vst3, logPath);
    testScan(pathFromUtf8(argv[1]), pathFromUtf8(argv[2]));
    fs::remove(logPath);

    if (g_failures) {
        std::fprintf(stderr, "%d check(s) failed\n", g_failures);
        return 1;
    }
    std::puts("all checks passed");
    return 0;
}

int main(int argc, char** argv) { return brack::runWithMainLoop([&] { return run(argc, argv); }); }
