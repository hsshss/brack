// Crash containment: a plugin that crashes (an access violation; on Linux, a segmentation fault)
// in process(), in a state save or in init must not take the process down. It is marked crashed,
// goes silent, keeps the state it last saved, and the rest of the rack carries on. A module that
// crashes while being loaded, listed or unloaded is not used again, and a scan goes on past it.
// Also the change counter autosaving relies on.
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "engine.h"
#include "plugin/plugin.h"
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

// Peak of each output channel over `total` frames.
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

bool has(const std::string& s, const std::string& part) { return s.find(part) != std::string::npos; }

// How the test plugins' crash (a write through a null pointer) and a stack overflow are reported.
#ifdef _WIN32
const std::string kFault = "access violation", kOverflow = "stack overflow";
#elif defined(__APPLE__)
const std::string kFault = "segmentation fault", kOverflow = "bus error";  // the stack's guard page
#else
const std::string kFault = "segmentation fault", kOverflow = "segmentation fault";
#endif

struct Stage {
    const char* name;   // the file name part the test plugin looks for (crash_stage.h)
    const char* what;   // how brack reports the call that crashed
    bool whenUnloaded;  // the crash comes once the module is released, not while describing it
};

void copyPlugin(const fs::path& from, const fs::path& to) {
    fs::remove_all(to);
    fs::copy(from, to, fs::copy_options::recursive);
}

// A copy of `plugin` named for `stage`, in `dir`.
fs::path stageCopy(const fs::path& plugin, const fs::path& dir, const std::string& stage) {
    fs::path copy = dir / (plugin.stem().string() + "-crash-at-" + stage + plugin.extension().string());
    copyPlugin(plugin, copy);
    return copy;
}

// Every stage of one format: describing the copy (as a scan does) survives the crash and
// reports it, and the file is refused from then on; a scan of them all still finds the good
// copy; and an instance whose module crashes when unloaded can be removed.
void moduleCrashes(const fs::path& plugin, const std::vector<Stage>& stages, const fs::path& dir,
                   const std::function<bool(const std::string&)>& warned) {
    std::error_code ec;
    fs::remove_all(dir, ec);  // left by an earlier run
    fs::create_directories(dir);
    const fs::path good = dir / plugin.filename();
    copyPlugin(plugin, good);
    std::string err;
    auto goodDescs = describePluginFile(good, err);
    CHECK(goodDescs.size() >= 1);
    if (goodDescs.empty()) return;
    const std::string pluginId = goodDescs[0].id;

    for (const Stage& st : stages) {
        const fs::path copy = stageCopy(plugin, dir, st.name);
        err.clear();
        auto found = describePluginFile(copy, err);  // released again on return
        if (st.whenUnloaded) {
            CHECK(!found.empty());
            CHECK(warned(std::string("crashed in ") + st.what + ": " + kFault));
        } else {
            CHECK(found.empty() && has(err, std::string("crashed in ") + st.what + ": " + kFault));
        }
        std::printf("%s: %s\n", copy.filename().string().c_str(), st.whenUnloaded ? "described" : err.c_str());
        err.clear();
        CHECK(describePluginFile(copy, err).empty());
        CHECK(has(err, "crashed earlier") && has(err, st.what) && has(err, "restart Brack"));
    }

    // A scan, which describes them in a plugin host process, still lists the good copy, and of
    // the others only the one that crashes once unloaded: its plugins were described in full.
    auto all = scanPlugins({dir});
    bool goodListed = false;
    for (auto& d : all) {
        goodListed |= pathFromUtf8(d.path) == fs::weakly_canonical(good);
        CHECK(!has(d.path, "crash-at-") || has(d.path, std::string("crash-at-") + stages.back().name));
    }
    CHECK(goodListed);

    // An instance whose module crashes when the last instance goes can be removed; the good
    // copy is unaffected. In this process, the module is refused from then on; in a plugin
    // host process, only that process saw the crash, and the next one loads it again.
    const fs::path unloads = stageCopy(plugin, dir, std::string(stages.back().name) + "-2");
    for (bool inProcess : {true, false}) {
        Engine e;
        EngineConfig cfg;
        cfg.pluginsInProcess = inProcess;
        CHECK(e.setConfig(cfg, err));
        CHECK(e.startManual(48000, 2, 256, err));
        CHECK(e.addPlugin({"u", pathToUtf8(unloads), pluginId, "", std::nullopt}, false, err) == "u");
        CHECK(e.removePlugin("u", err));
        CHECK(e.snapshot().plugins.empty());
        if (inProcess) {
            CHECK(e.addPlugin({"u", pathToUtf8(unloads), pluginId, "", std::nullopt}, false, err).empty());
            CHECK(has(err, "crashed earlier"));
        } else {
            CHECK(e.addPlugin({"u", pathToUtf8(unloads), pluginId, "", std::nullopt}, false, err) == "u");
        }
        CHECK(e.addPlugin({"g", pathToUtf8(good), pluginId, "", std::nullopt}, false, err) == "g");
    }
}

}  // namespace

int run(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: test_crash <brack-test-synth.clap> <brack-test-synth.vst3>\n");
        return 2;
    }
    const std::string synth = argv[1];
    const std::string vst3Synth = argv[2];
    std::mutex logMutex;
    std::vector<std::string> errors, warnings;
    setLogSink([&](LogLevel l, const std::string& m) {
        std::lock_guard lock(logMutex);
        if (l == LogLevel::Error) errors.push_back(m);
        if (l == LogLevel::Warning) warnings.push_back(m);
    });
    auto warned = [&](const std::string& part) {
        std::lock_guard lock(logMutex);
        for (auto& m : warnings)
            if (has(m, part)) return true;
        return false;
    };
    auto loggedError = [&](const std::string& part) {
        for (int i = 0; i < 100; ++i) {  // reported by the host thread's idle pass
            {
                std::lock_guard lock(logMutex);
                for (auto& m : errors)
                    if (m.find(part) != std::string::npos) return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        return false;
    };

    // Plugins in this process, then each in a plugin host process: the same containment.
    for (bool inProcess : {true, false}) {
        std::printf("plugins %s\n", inProcess ? "in this process" : "in plugin host processes");
        {
            std::lock_guard lock(logMutex);
            errors.clear();
        }
        Engine e;
        std::string err;
        EngineConfig cfg;
        cfg.pluginsInProcess = inProcess;
        CHECK(e.setConfig(cfg, err));
        CHECK(e.startManual(48000, 2, 256, err));

        // Changes count; what does not change the session does not.
        uint64_t changes = e.changeCount();
        CHECK(e.addPlugin({"a", synth, "brack.test.synth", "", std::nullopt}, false, err) == "a");
        CHECK(e.addPlugin({"c", synth, "brack.test.synth", "", std::nullopt}, false, err) == "c");
        CHECK(e.setAudioRoutes("a", {{"a", 0, 0, 0, 1.0f}}, err));  // a -> output 1
        CHECK(e.setAudioRoutes("c", {{"c", 0, 0, 1, 1.0f}}, err));  // c -> output 2
        CHECK(e.changeCount() >= changes + 4);
        changes = e.changeCount();
        e.snapshot();
        e.saveSessionJson();  // also the state the plugins fall back on once crashed
        CHECK(e.changeCount() == changes);
        CHECK(e.setMasterGain(0.8f));
        CHECK(e.changeCount() == changes + 1);

        const uint8_t on[] = {0x90, 0x3C, 0x64};
        CHECK(e.sendMidiToPlugin("a", 0, on, 3) == MidiSend::Sent);
        CHECK(e.sendMidiToPlugin("c", 0, on, 3) == MidiSend::Sent);
        auto peaks = renderPeaks(e, 4800);
        CHECK(peaks[0] > 0.1f && peaks[1] > 0.1f);
        CHECK(e.changeCount() == changes + 1);  // playing is not a change

        // A parameter the plugin reports through its output events is (CC 7: a value, CC 8:
        // the end of a gesture; see test_synth.cpp).
        changes = e.changeCount();
        const uint8_t cc7[] = {0xB0, 0x07, 0x40}, cc8[] = {0xB0, 0x08, 0x00};
        CHECK(e.sendMidiToPlugin("a", 0, cc7, 3) == MidiSend::Sent);
        renderPeaks(e, 512);
        CHECK(e.changeCount() > changes);
        changes = e.changeCount();
        CHECK(e.sendMidiToPlugin("a", 0, cc8, 3) == MidiSend::Sent);
        renderPeaks(e, 512);
        CHECK(e.changeCount() > changes);

        // c crashes on the audio thread (here, the caller of render()).
        const uint8_t crashNow[] = {0xF0, 0x7D, 0x63, 0x01, 0xF7};
        CHECK(e.sendMidiToPlugin("c", 0, crashNow, sizeof crashNow) == MidiSend::Sent);
        renderPeaks(e, 512);
        peaks = renderPeaks(e, 4800);
        std::printf("after c crashed: out1 %.3f, out2 %.3f\n", peaks[0], peaks[1]);
        CHECK(peaks[0] > 0.1f);  // a plays on
        CHECK(peaks[1] == 0.0f);  // c is silent
        auto snap = e.snapshot();
        const auto* c = find(snap, "c");
        CHECK(c && c->crashed && has(c->status, "crashed in process: " + kFault));
        if (c) std::printf("c: %s\n", c->status.c_str());
        CHECK(find(snap, "a") && !find(snap, "a")->crashed);
        CHECK(loggedError("crashed in process"));

        // Its editor cannot be opened; the session keeps its last saved state.
        CHECK(!e.setPluginGuiVisible("c", true, err) && err.find("crashed") != std::string::npos);
        CHECK(count(e.saveSessionJson(), "\"state\"") == 2);

        // a crashes in a state save (on the host thread): the session still gets its state.
        const uint8_t crashOnSave[] = {0xF0, 0x7D, 0x63, 0x02, 0xF7};
        CHECK(e.sendMidiToPlugin("a", 0, crashOnSave, sizeof crashOnSave) == MidiSend::Sent);
        renderPeaks(e, 512);
        std::string session = e.saveSessionJson();
        CHECK(count(session, "\"state\"") == 2);
        snap = e.snapshot();
        CHECK(find(snap, "a") && find(snap, "a")->crashed &&
              find(snap, "a")->status.find("crashed in save state") != std::string::npos);
        CHECK(renderPeaks(e, 4800)[0] == 0.0f);

        // A crashed plugin can be removed (it is not called to do so).
        CHECK(e.removePlugin("c", err));
        CHECK(e.snapshot().plugins.size() == 1);

        // A plugin that crashes while being created: the add fails, nothing else does.
        CHECK(e.addPlugin({"x", synth, "brack.test.synth-crash-init", "", std::nullopt}, false, err).empty());
        std::printf("crash in init: %s\n", err.c_str());
        CHECK(err.find("crashed in init") != std::string::npos);
        CHECK(e.snapshot().plugins.size() == 1);

        // A new instance of the same plugin still works.
        CHECK(e.addPlugin({"b", synth, "brack.test.synth", "", std::nullopt}, false, err) == "b");
        CHECK(e.setAudioRoutes("b", {{"b", 0, 0, 1, 1.0f}}, err));
        CHECK(e.sendMidiToPlugin("b", 0, on, 3) == MidiSend::Sent);
        CHECK(renderPeaks(e, 4800)[1] > 0.1f);

        // Running out of stack is caught too, and again after that: on Linux the handler runs on
        // a stack of its own, and on Windows the stack's guard page is put back.
        const uint8_t overflow[] = {0xF0, 0x7D, 0x63, 0x08, 0xF7};
        CHECK(e.addPlugin({"d", synth, "brack.test.synth", "", std::nullopt}, false, err) == "d");
        for (const char* id : {"b", "d"}) {
            CHECK(e.sendMidiToPlugin(id, 0, overflow, sizeof overflow) == MidiSend::Sent);
            renderPeaks(e, 512);
            snap = e.snapshot();
            const auto* p = find(snap, id);
            CHECK(p && p->crashed && has(p->status, "crashed in process: " + kOverflow));
            if (p) std::printf("%s: %s\n", id, p->status.c_str());
        }

#ifdef _WIN32
        // As is a C++ exception thrown out of the plugin, a structured exception there. On Linux
        // it unwinds through the plugin's own copy of the unwinder (runtimes linked statically)
        // into Brack's, which aborts: the process ends, as with __fastfail.
        const uint8_t throwNow[] = {0xF0, 0x7D, 0x63, 0x09, 0xF7};
        CHECK(e.addPlugin({"t", synth, "brack.test.synth", "", std::nullopt}, false, err) == "t");
        CHECK(e.sendMidiToPlugin("t", 0, throwNow, sizeof throwNow) == MidiSend::Sent);
        renderPeaks(e, 512);
        snap = e.snapshot();
        const auto* t = find(snap, "t");
        CHECK(t && t->crashed && has(t->status, "crashed in process: uncaught C++ exception"));
        if (t) std::printf("t: %s\n", t->status.c_str());
#endif
    }  // the engine goes, with the crashed "a" in it

    // The copies that crashed stay loaded until the process exits; the next run replaces them.
    const fs::path tmp = fs::temp_directory_path() / ("brack_test_crash-" + std::string(buildArchitecture()));
    moduleCrashes(pathFromUtf8(synth),
                  {{"init", "clap_entry.init", false},
                   {"factory", "clap_entry.get_factory", false},
                   {"count", "listing its plugins", false},
                   {"deinit", "clap_entry.deinit", true}},
                  tmp / "clap", warned);
#ifdef _WIN32
    const Stage entry{"InitDll", "InitDll", false}, exit{"ExitDll", "unloading", true};
#elif defined(__APPLE__)
    const Stage entry{"bundleEntry", "bundleEntry", false}, exit{"bundleExit", "unloading", true};
#else
    const Stage entry{"ModuleEntry", "ModuleEntry", false}, exit{"ModuleExit", "unloading", true};
#endif
    moduleCrashes(pathFromUtf8(vst3Synth),
                  {entry, {"GetPluginFactory", "GetPluginFactory", false}, {"countClasses", "listing its plugins", false},
                   exit},
                  tmp / "vst3", warned);
    setLogSink(nullptr);

    if (g_failures) {
        std::fprintf(stderr, "%d check(s) failed\n", g_failures);
        return 1;
    }
    std::puts("all checks passed");
    return 0;
}

int main(int argc, char** argv) { return brack::runWithMainLoop([&] { return run(argc, argv); }); }
