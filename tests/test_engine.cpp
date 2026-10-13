// End-to-end engine test in manual render mode with the test synth.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "engine.h"
#include "util/common.h"
#include "util/main_thread.h"

using namespace brack;

namespace {

int g_failures = 0;
#define CHECK(cond)                                                       \
    do {                                                                  \
        if (!(cond)) {                                                    \
            std::fprintf(stderr, "CHECK failed: %s (line %d)\n", #cond, __LINE__); \
            ++g_failures;                                                 \
        }                                                                 \
    } while (0)

float renderPeak(Engine& e, uint32_t channels, uint32_t frames, uint32_t total) {
    std::vector<std::vector<float>> buf(channels, std::vector<float>(frames));
    std::vector<float*> ptrs;
    for (auto& b : buf) ptrs.push_back(b.data());
    float peak = 0;
    for (uint32_t done = 0; done < total; done += frames) {
        e.render(ptrs.data(), channels, frames);
        for (auto& b : buf)
            for (float v : b) peak = std::max(peak, std::fabs(v));
    }
    return peak;
}

std::vector<std::string> readLines(const std::filesystem::path& p) {
    std::vector<std::string> lines;
    std::ifstream f(p);
    for (std::string l; std::getline(f, l);) lines.push_back(l);
    return lines;
}

bool containsInOrder(const std::vector<std::string>& lines, const std::vector<std::string>& expected) {
    size_t k = 0;
    for (auto& l : lines)
        if (k < expected.size() && l == expected[k]) ++k;
    return k == expected.size();
}

// Each output's peak over `total` frames, after a buffer to let a change settle (through the SRC).
std::vector<float> settledPeaks(Engine& e, uint32_t total) {
    renderPeak(e, 2, 512, 2048);
    std::vector<std::vector<float>> buf(2, std::vector<float>(512));
    float* ptrs[2] = {buf[0].data(), buf[1].data()};
    std::vector<float> peak(2, 0.0f);
    for (uint32_t done = 0; done < total; done += 512) {
        e.render(ptrs, 2, 512);
        for (int c = 0; c < 2; ++c)
            for (float v : buf[c]) peak[c] = std::max(peak[c], std::fabs(v));
    }
    return peak;
}

bool about(float value, float expected) { return std::fabs(value - expected) <= expected * 0.03f; }

std::vector<std::string> effectIds(const std::vector<EngineSnapshot::Plugin>& effects) {
    std::vector<std::string> ids;
    for (auto& p : effects) ids.push_back(p.id);
    return ids;
}

std::vector<uint8_t> gainState(float gain) {
    std::vector<uint8_t> s(sizeof gain);
    std::memcpy(s.data(), &gain, sizeof gain);
    return s;
}

// Effects on an instrument and on the master: in this process, then in plugin host processes.
// The gains (test_effect/) each halve what comes in, so a chain is heard by its peak alone.
void chains(const std::string& synth, const std::string& gain, const std::string& gainVst2, const std::string& gainVst3) {
    for (bool inProcess : {true, false}) {
        std::printf("effects %s\n", inProcess ? "in this process" : "in plugin host processes");
        Engine e;
        std::string err;
        EngineConfig cfg;
        cfg.pluginsInProcess = inProcess;
        cfg.processSampleRate = 96000;  // the master's effects come before the SRC
        CHECK(e.setConfig(cfg, err));
        CHECK(e.startManual(48000, 2, 512, err));
        CHECK(e.addPlugin({"s", synth, "brack.test.synth", "", std::nullopt}, true, err) == "s");
        const uint8_t on[] = {0x90, 0x3C, 0x64};
        CHECK(e.sendMidiToPlugin("s", 0, on, 3) == MidiSend::Sent);
        const float dry = settledPeaks(e, 4800)[0];
        CHECK(dry > 0.1f);

        // One effect, then one of each format in the chain, each halving.
        CHECK(e.addEffect("s", 0, {"g", gain, "brack.test.gain", "", std::nullopt}, err) == "g");
        CHECK(about(settledPeaks(e, 4800)[0], dry * 0.5f));
        CHECK(e.addEffect("s", 1, {"g3", gainVst3, "", "", std::nullopt}, err) == "g3");
        CHECK(e.addEffect("s", 9, {"g2", gainVst2, "", "", std::nullopt}, err) == "g2");  // clamped: the end
        auto peaks = settledPeaks(e, 4800);
        std::printf("  dry %.3f, through CLAP, VST3, VST2 %.3f %.3f\n", dry, peaks[0], peaks[1]);
        CHECK(about(peaks[0], dry * 0.125f) && about(peaks[1], dry * 0.125f));
        auto snap = e.snapshot();
        CHECK(snap.plugins.size() == 1 && effectIds(snap.plugins[0].effects) == std::vector<std::string>({"g", "g3", "g2"}));
        for (auto& fx : snap.plugins[0].effects) CHECK(fx.loaded && fx.active && fx.separateProcess == !inProcess);

        // Bypassed, an effect lets what comes to it through.
        CHECK(e.setEffectBypass("g3", true, err));
        CHECK(about(settledPeaks(e, 4800)[0], dry * 0.25f));
        CHECK(e.snapshot().plugins[0].effects[1].bypass);
        CHECK(e.setEffectBypass("g3", false, err));
        CHECK(!e.setEffectBypass("s", true, err));  // an instrument

        // A mono effect takes the bus's first channel and leaves the second as it was.
        CHECK(e.addEffect("s", 0, {"m", gain, "brack.test.gain-mono", "", std::nullopt}, err) == "m");
        peaks = settledPeaks(e, 4800);
        CHECK(about(peaks[0], dry * 0.0625f) && about(peaks[1], dry * 0.125f));
        CHECK(e.removePlugin("m", err));

        // Moved among the chain's effects; a state, the name and the order survive a session.
        CHECK(e.movePlugin("g2", 0, err));
        CHECK(effectIds(e.snapshot().plugins[0].effects) == std::vector<std::string>({"g2", "g", "g3"}));
        CHECK(e.setPluginState("g", gainState(0.25f), err));
        CHECK(about(settledPeaks(e, 4800)[0], dry * 0.0625f));
        CHECK(e.setPluginName("g", "Quarter", err));
        CHECK(e.setEffectBypass("g2", true, err));

        // The master's effects take the mix.
        CHECK(e.addEffect("", 0, {"mg", gain, "brack.test.gain", "", std::nullopt}, err) == "mg");
        CHECK(about(settledPeaks(e, 4800)[0], dry * 0.0625f));
        CHECK(e.snapshot().masterEffects.size() == 1);
        const std::string session = e.saveSessionJson();

        // What is not an effect, or not a plugin's to have.
        CHECK(e.addEffect("s", 0, {"x", synth, "brack.test.synth", "", std::nullopt}, err).empty());
        std::printf("  an instrument as an effect: %s\n", err.c_str());
        CHECK(err.find("not an effect") != std::string::npos);
        // A file without a plugin id gives the first of the kind asked for.
        CHECK(e.addEffect("s", 0, {"x", synth, "", "", std::nullopt}, err).empty());
        CHECK(err.find("no effect in the file") != std::string::npos);
        CHECK(e.addPlugin({"x", gain, "", "", std::nullopt}, true, err).empty());
        CHECK(err.find("no instrument in the file") != std::string::npos);
        CHECK(e.addEffect("nope", 0, {"", gain, "", "", std::nullopt}, err).empty());
        CHECK(e.addEffect("s", 0, {"s", gain, "", "", std::nullopt}, err).empty());  // the id is taken
        CHECK(e.addMidiSource({"keys", MidiSourceKind::Api, ""}, err) == "keys");
        CHECK(!e.addMidiRoute({"keys", "g", 0}, err));
        CHECK(e.sendMidiToPlugin("g", 0, on, 3) == MidiSend::UnknownId);

        // An instrument goes with its effects; the master's last effect, with its chain.
        CHECK(e.removePlugin("mg", err));
        CHECK(e.snapshot().masterEffects.empty());
        CHECK(e.removePlugin("s", err));
        CHECK(e.snapshot().plugins.empty() && !e.setPluginName("g", "x", err));

        Engine r;
        CHECK(r.loadSessionJson(session, err));
        CHECK(r.startManual(48000, 2, 512, err));
        snap = r.snapshot();
        CHECK(snap.plugins.size() == 1 && effectIds(snap.plugins[0].effects) == std::vector<std::string>({"g2", "g", "g3"}));
        CHECK(snap.plugins.size() == 1 && snap.plugins[0].effects[1].name == "Quarter" && snap.plugins[0].effects[0].bypass);
        CHECK(snap.masterEffects.size() == 1 && snap.masterEffects[0].active);
        CHECK(r.sendMidiToPlugin("s", 0, on, 3) == MidiSend::Sent);
        peaks = settledPeaks(r, 4800);
        std::printf("  from the session %.3f (expected %.3f)\n", peaks[0], dry * 0.0625f);
        CHECK(about(peaks[0], dry * 0.0625f));
        CHECK(r.saveSessionJson() == session);

        // An effect that fails to process is passed over, as if bypassed, and says so.
        CHECK(r.setPluginState("g", gainState(1000.0f), err));
        peaks = settledPeaks(r, 4800);
        bool failed = false;
        for (int i = 0; i < 100 && !failed; ++i) {  // a plugin host tells within an idle pass
            failed = r.snapshot().plugins[0].effects[1].failed;
            if (!failed) std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        std::printf("  an effect failing to process: %s, %.3f (expected %.3f)\n", failed ? "failed" : "not failed",
                    peaks[0], dry * 0.25f);
        CHECK(failed && about(peaks[0], dry * 0.25f));
    }
}

// While an effect loads (the slow gain: 300 ms), its instrument plays on, and the mix is never
// heard without the master's effects.
void editWhilePlaying(const std::string& synth, const std::string& gain) {
    for (bool inProcess : {true, false}) {
        Engine e;
        std::string err;
        EngineConfig cfg;
        cfg.pluginsInProcess = inProcess;
        CHECK(e.setConfig(cfg, err));
        CHECK(e.startManual(48000, 2, 256, err));
        CHECK(e.addPlugin({"s", synth, "brack.test.synth", "", std::nullopt}, true, err) == "s");
        const uint8_t on[] = {0x90, 0x3C, 0x64};
        CHECK(e.sendMidiToPlugin("s", 0, on, 3) == MidiSend::Sent);
        CHECK(e.addEffect("", 0, {"mg", gain, "brack.test.gain", "", std::nullopt}, err) == "mg");
        // Each block's peak, rendered on a thread of its own, paced as by a device, while `edit` runs.
        auto during = [&](const std::function<void()>& edit) {
            std::atomic<bool> done{false};
            std::vector<float> peaks;
            std::thread audio([&] {
                std::vector<std::vector<float>> buf(2, std::vector<float>(256));
                float* ptrs[2] = {buf[0].data(), buf[1].data()};
                while (!done) {
                    e.render(ptrs, 2, 256);
                    float peak = 0;
                    for (auto& b : buf)
                        for (float v : b) peak = std::max(peak, std::fabs(v));
                    peaks.push_back(peak);
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }
            });
            edit();
            done = true;
            audio.join();
            return peaks;
        };
        auto silent = [](const std::vector<float>& peaks) {
            return (size_t)std::count_if(peaks.begin(), peaks.end(), [](float p) { return p < 1e-4f; });
        };

        const float before = settledPeaks(e, 4800)[0];
        auto peaks = during([&] { CHECK(e.addEffect("s", 0, {"si", gain, "brack.test.gain-slow", "", std::nullopt}, err) == "si"); });
        std::printf("effect added to a playing instrument (%s): %zu of %zu blocks silent\n",
                    inProcess ? "in this process" : "in plugin host processes", silent(peaks), peaks.size());
        CHECK(peaks.size() >= 10 && silent(peaks) <= 2);

        const float level = settledPeaks(e, 4800)[0];
        CHECK(about(level, before * 0.5f));
        peaks = during([&] { CHECK(e.addEffect("", 1, {"sm", gain, "brack.test.gain-slow", "", std::nullopt}, err) == "sm"); });
        const float loudest = *std::max_element(peaks.begin(), peaks.end());
        std::printf("effect added to the master: %zu of %zu blocks silent, loudest %.3f (through its effects %.3f)\n",
                    silent(peaks), peaks.size(), loudest, level);
        CHECK(peaks.size() >= 10 && silent(peaks) <= 2 && loudest <= level * 1.05f);
        CHECK(about(settledPeaks(e, 4800)[0], level * 0.5f));
    }
}

// An effect for a chain in a plugin host loads on the caller's thread: the host thread answers
// meanwhile, and the chain may change under it.
void effectLoadsAside(const std::string& synth, const std::string& gain) {
    Engine e;
    std::string err;
    EngineConfig cfg;
    cfg.pluginsInProcess = false;
    CHECK(e.setConfig(cfg, err));
    CHECK(e.startManual(48000, 2, 256, err));
    CHECK(e.addPlugin({"s", synth, "brack.test.synth", "", std::nullopt}, true, err) == "s");
    // `add` on a thread of its own, `meanwhile` on this one once the effect has begun loading.
    auto alongside = [](const std::function<void()>& add, const std::function<void()>& meanwhile) {
        std::thread t(add);
        std::this_thread::sleep_for(std::chrono::milliseconds(100));  // of the slow gain's 300
        meanwhile();
        t.join();
    };

    using Clock = std::chrono::steady_clock;
    Clock::duration longest{};
    std::atomic<bool> added{false};
    alongside([&] {
        std::string error;
        CHECK(e.addEffect("s", 0, {"a", gain, "brack.test.gain-slow", "", std::nullopt}, error) == "a");
        added = true;
    }, [&] {
        while (!added) {
            const auto t0 = Clock::now();
            e.config();  // a round trip to the host thread
            longest = std::max(longest, Clock::now() - t0);
        }
    });
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(longest).count();
    std::printf("the host thread while an effect loads in a plugin host: answers within %lld ms\n", (long long)ms);
    CHECK(ms < 150);

    // Two at once into one chain: one after the other.
    std::string e1, e2, err1, err2;
    alongside([&] { e1 = e.addEffect("s", 9, {"b", gain, "brack.test.gain-slow", "", std::nullopt}, err1); },
              [&] { e2 = e.addEffect("s", 9, {"c", gain, "brack.test.gain-slow", "", std::nullopt}, err2); });
    CHECK(e1 == "b" && e2 == "c");
    CHECK(e.snapshot().plugins[0].effects.size() == 3);

    // Loaded again, or removed, meanwhile: the effect fails, and the rest is as it should be.
    for (bool remove : {false, true}) {
        std::string id, error;
        alongside([&] { id = e.addEffect("s", 0, {"x", gain, "brack.test.gain-slow", "", std::nullopt}, error); },
                  [&] { CHECK(remove ? e.removePlugin("s", err) : e.reloadPlugin("s", err)); });
        std::printf("%s meanwhile: %s\n", remove ? "removed" : "loaded again", error.c_str());
        CHECK(id.empty() && error.find("removed or loaded again") != std::string::npos);
        auto snap = e.snapshot();
        if (remove) {
            CHECK(snap.plugins.empty());
        } else {
            CHECK(snap.plugins.size() == 1 && effectIds(snap.plugins[0].effects) == std::vector<std::string>({"a", "b", "c"}));
            for (auto& fx : snap.plugins[0].effects) CHECK(fx.loaded && fx.active);
        }
    }
}

// A session that leaves ids out, or gives an effect a taken one: every plugin gets an id of its own.
void sessionIds(const std::string& synth, const std::string& gain) {
    auto quoted = [](const std::string& s) {
        std::string q = "\"";
        for (char c : s) q += c == '\\' ? std::string("\\\\") : std::string(1, c);
        return q + "\"";
    };
    auto plugin = [&](const std::string& id, const std::string& path, const std::string& extra = "") {
        return "{" + (id.empty() ? "" : "\"id\": " + quoted(id) + ", ") + "\"path\": " + quoted(path) +
               ", \"name\": \"Gain\"" + extra + "}";
    };
    const std::string fx = plugin("", gain), dup = plugin("dup", gain);
    const std::string text = R"({"format": "brack-session", "engine": {"pluginsInProcess": true}, "plugins": [)" +
                             plugin("", synth, ", \"effects\": [" + fx + ", " + fx + ", " + dup + ", " + dup + "]") + ", " +
                             plugin("t", synth, ", \"effects\": [" + dup + "]") + R"(], "masterEffects": [)" +
                             plugin("t", gain) + ", " + fx + "]}";
    Engine e;
    std::string err;
    CHECK(e.loadSessionJson(text, err));
    const EngineSnapshot snap = e.snapshot();
    std::vector<std::string> ids;
    for (auto& p : snap.plugins) {
        ids.push_back(p.id);
        for (auto& f : p.effects) ids.push_back(f.id);
    }
    for (auto& f : snap.masterEffects) ids.push_back(f.id);
    std::printf("session ids:");
    for (auto& id : ids) std::printf(" %s", id.c_str());
    std::printf("\n");
    const std::set<std::string> distinct(ids.begin(), ids.end());
    CHECK(ids.size() == 9 && distinct.size() == ids.size() && !distinct.count(""));
}

// A note that arrives while an instrument's chain changes sounds once it has changed instead of
// being lost: a reload loads the slow gain again (300 ms), and the note-on comes meanwhile.
void midiWhileChanging(const std::string& synth, const std::string& gain) {
    Engine e;
    std::string err;
    EngineConfig cfg;
    cfg.pluginsInProcess = false;
    CHECK(e.setConfig(cfg, err));
    CHECK(e.startManual(48000, 2, 256, err));
    CHECK(e.addPlugin({"s", synth, "brack.test.synth", "", std::nullopt}, true, err) == "s");
    CHECK(e.addMidiSource({"keys", MidiSourceKind::Api, ""}, err) == "keys");
    CHECK(e.addMidiRoute({"keys", "s", 0}, err));
    CHECK(e.addEffect("s", 0, {"slow", gain, "brack.test.gain-slow", "", std::nullopt}, err) == "slow");
    CHECK(renderPeak(e, 2, 256, 4800) < 1e-6f);  // silence before the note
    std::thread reload([&] {
        std::string error;
        CHECK(e.reloadPlugin("s", error));
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(100));  // of the slow gain's 300
    const uint8_t on[] = {0x90, 0x3C, 0x64};
    CHECK(e.sendMidiToSource("keys", on, 3) == MidiSend::Sent);
    renderPeak(e, 2, 256, 2560);  // takes the note-on from its source while the chain reloads
    reload.join();
    const float peak = renderPeak(e, 2, 256, 9600);
    std::printf("a note-on while a chain reloads: peak %.3f once reloaded\n", peak);
    CHECK(peak > 0.01f);
}

// Meters: what an instrument's chain puts out, routed or not; past full scale; the mix before and
// after the master's chain; each chain's load, timed in a plugin host too; the MIDI an instrument took;
// zero once unwritten for a while.
void meters(const std::string& synth, const std::string& gain) {
    for (bool inProcess : {true, false}) {
        std::printf("meters %s\n", inProcess ? "in this process" : "in plugin host processes");
        Engine e;
        std::string err;
        EngineConfig cfg;
        cfg.pluginsInProcess = inProcess;
        CHECK(e.setConfig(cfg, err));
        CHECK(e.startManual(48000, 2, 256, err));
        CHECK(e.addPlugin({"s", synth, "brack.test.synth", "", std::nullopt}, true, err) == "s");
        CHECK(e.setAudioRoutes("s", {{"s", 0, 0, 0, 1.0f}}, err));  // R is not routed
        CHECK(e.addMidiSource({"keys", MidiSourceKind::Api, ""}, err) == "keys");
        CHECK(e.addMidiRoute({"keys", "s", 0}, err));
        const uint8_t on[] = {0x90, 0x3C, 0x64}, cc[] = {0xB0, 0x07, 0x7F};
        CHECK(e.sendMidiToSource("keys", on, 3) == MidiSend::Sent);
        CHECK(e.sendMidiToPlugin("s", 0, cc, 3) == MidiSend::Sent);
        CHECK(e.sendMidiToPlugin("s", 0, cc, 3) == MidiSend::Sent);
        CHECK(e.sendMidiToPlugin("s", 1, cc, 3) == MidiSend::Sent);  // it has no port 1: dropped, not counted
        settledPeaks(e, 4800);

        EngineSnapshot snap = e.snapshot();
        const EngineSnapshot::Plugin& s = snap.plugins[0];
        std::printf("  instrument L %.3f R %.3f, load %.4f (max %.4f), %llu MIDI messages\n", s.outputLevels[0].peak,
                    s.outputLevels[1].peak, s.load.average, s.load.peak, (unsigned long long)s.midiMessages);
        CHECK(s.outputLevels.size() == 2 && s.outputLevels[0].peak > 0.1f && about(s.outputLevels[1].peak, s.outputLevels[0].peak));
        CHECK(s.outputLevels[0].clips == 0 && s.outputLevels[1].clips == 0);
        CHECK(s.load.average > 0 && s.load.peak > 0);
        CHECK(s.midiMessages == 3);
        CHECK(snap.load.average > 0);
        CHECK(snap.output.size() == 2 && snap.output[0].peak > 0.1f && snap.output[1].peak == 0);
        CHECK(snap.masterInput.size() == 2 && about(snap.masterInput[0].peak, s.outputLevels[0].peak));
        e.refreshSnapshot();
        CHECK(e.cachedSnapshot().plugins[0].midiMessages == 3);

        // Past full scale, in the chain: before the routes' gains, which bring it back under.
        CHECK(e.addEffect("s", 0, {"g", gain, "brack.test.gain", "", std::nullopt}, err) == "g");
        CHECK(e.setPluginState("g", gainState(8.0f), err));
        CHECK(e.setAudioRoutes("s", {{"s", 0, 0, 0, 0.25f}}, err));
        settledPeaks(e, 4800);
        snap = e.snapshot();
        const uint64_t clips = snap.plugins[0].outputLevels[0].clips;
        std::printf("  through 8x: %.3f, %llu blocks past full scale; the mix %.3f\n", snap.plugins[0].outputLevels[0].peak,
                    (unsigned long long)clips, snap.masterInput[0].peak);
        CHECK(snap.plugins[0].outputLevels[0].peak > 1.0f && clips > 0);
        CHECK(snap.masterInput[0].peak < 1.0f && snap.masterInput[0].clips == 0);
        settledPeaks(e, 4800);
        CHECK(e.snapshot().plugins[0].outputLevels[0].clips > clips);

        // The master's effect halves the mix: its input and output.
        CHECK(e.addEffect("", 0, {"mg", gain, "brack.test.gain", "", std::nullopt}, err) == "mg");
        settledPeaks(e, 4800);
        snap = e.snapshot();
        std::printf("  master in %.3f out %.3f, load %.4f\n", snap.masterInput[0].peak, snap.masterOutput[0].peak,
                    snap.masterLoad.average);
        CHECK(snap.masterInput[0].peak > 0.1f && about(snap.masterOutput[0].peak, snap.masterInput[0].peak * 0.5f));
        CHECK(snap.masterLoad.average > 0);

        // Nothing rendered for longer than kMeterStaleNs: levels and loads read 0, the clips stay.
        std::this_thread::sleep_for(std::chrono::nanoseconds(kMeterStaleNs) + std::chrono::milliseconds(50));
        snap = e.snapshot();
        const EngineSnapshot::Plugin& idle = snap.plugins[0];
        CHECK(idle.outputLevels[0].peak == 0 && idle.load.average == 0 && idle.load.peak == 0);
        CHECK(snap.output[0].peak == 0 && snap.masterOutput[0].peak == 0 && snap.load.average == 0 && snap.masterLoad.peak == 0);
        CHECK(idle.outputLevels[0].clips > clips);
    }
}

}  // namespace

int run(int argc, char** argv) {
    if (argc < 5) {
        std::fprintf(stderr, "usage: test_engine <brack-test-synth.clap> <brack-test-gain.clap> <gain vst2> <gain vst3>\n");
        return 2;
    }
    const std::string synth = argv[1];
    auto logPath = std::filesystem::temp_directory_path() / ("brack_test_engine-" + std::string(buildArchitecture()) + ".log");
    std::filesystem::remove(logPath);
#ifdef _WIN32
    _putenv(("BRACK_TESTSYNTH_LOG=" + pathToUtf8(logPath)).c_str());
#else
    setenv("BRACK_TESTSYNTH_LOG", pathToUtf8(logPath).c_str(), 1);
#endif

    std::string session;
    {
        Engine e;
        std::string err;
        EngineConfig cfg;
        cfg.processSampleRate = 96000;  // render plugins at 96k, output at 44.1k through the SRC
        cfg.blockSize = 128;
        CHECK(e.setConfig(cfg, err));

        std::string a = e.addPlugin({"a", synth, "brack.test.synth", "", std::nullopt}, true, err);
        std::string b = e.addPlugin({"b", synth, "brack.test.synth-clap", "", std::nullopt}, true, err);
        CHECK(a == "a");
        CHECK(b == "b");
        CHECK(e.setPluginName("a", "Lead", err));
        CHECK(e.snapshot().plugins[0].name == "Lead" && e.snapshot().plugins[0].id == "a");
        CHECK(e.setPluginName("a", "", err));  // empty: back to the plugin's own name
        CHECK(e.snapshot().plugins[0].name == "brack test synth");
        CHECK(!e.setPluginName("nope", "x", err));
        CHECK(e.setPluginName("a", "Lead", err));
        CHECK(e.movePlugin("b", 0, err));
        CHECK(e.snapshot().plugins[0].id == "b" && e.snapshot().plugins[1].id == "a");
        CHECK(e.movePlugin("b", 99, err));  // clamped to the end
        CHECK(e.snapshot().plugins[0].id == "a" && e.snapshot().plugins[1].id == "b");
        CHECK(!e.movePlugin("nope", 0, err));
        std::string src = e.addMidiSource({"keys", MidiSourceKind::Api, ""}, err);
        CHECK(src == "keys");
        CHECK(e.addMidiSource({"pads", MidiSourceKind::Api, ""}, err) == "pads");
        CHECK(e.moveMidiSource("pads", 0, err));
        CHECK(e.snapshot().sources[0].id == "pads" && e.snapshot().sources[1].id == "keys");
        CHECK(e.removeMidiSource("pads", err));
        CHECK(e.addMidiRoute({"keys", "a", 0}, err));
        CHECK(e.addMidiRoute({"keys", "b", 0}, err));
        CHECK(!e.addMidiRoute({"keys", "a", 5}, err));  // no such note port

        CHECK(e.startManual(44100, 2, 512, err));
        auto snap = e.snapshot();
        CHECK(snap.resampling);
        CHECK(snap.outputSampleRate == 44100 && snap.processSampleRate == 96000);
        CHECK(snap.plugins.size() == 2 && snap.plugins[0].active && snap.plugins[1].active);

        CHECK(renderPeak(e, 2, 512, 4410) < 1e-6f);  // silence before any note

        const uint8_t on[] = {0x90, 0x3C, 0x64}, off[] = {0x80, 0x3C, 0x00}, onVel0[] = {0x91, 0x40, 0x00};
        const uint8_t sysex[] = {0xF0, 0x7E, 0x7F, 0x06, 0x01, 0xF7}, shortSysex[] = {0xF0, 0x7D, 0x01, 0xF7};
        const uint8_t cc[] = {0xB0, 0x07, 0x7F}, bad[] = {0x3C, 0x64};
        CHECK(e.sendMidiToSource("keys", on, 3) == MidiSend::Sent);
        CHECK(e.sendMidiToPlugin("a", 0, sysex, sizeof sysex) == MidiSend::Sent);
        CHECK(e.sendMidiToPlugin("a", 0, shortSysex, sizeof shortSysex) == MidiSend::Sent);
        CHECK(e.sendMidiToPlugin("a", 0, cc, 3) == MidiSend::Sent);
        CHECK(e.sendMidiToPlugin("a", 0, bad, 2) == MidiSend::Malformed);  // no status byte
        CHECK(e.sendMidiToPlugin("nope", 0, on, 3) == MidiSend::UnknownId);
        float peak = renderPeak(e, 2, 512, 22050);
        std::printf("peak while note held: %.3f\n", peak);
        CHECK(peak > 0.1f);

        CHECK(e.sendMidiToSource("keys", off, 3) == MidiSend::Sent);
        CHECK(e.sendMidiToSource("keys", onVel0, 3) == MidiSend::Sent);
        renderPeak(e, 2, 512, 4410);
        CHECK(renderPeak(e, 2, 512, 4410) < 1e-3f);  // released (SRC tail has decayed)

        // Master gain: silent at 0, and back. The first buffer after a change is a ramp.
        CHECK(!e.setMasterGain(-1.0f));
        CHECK(e.setMasterGain(0.0f));
        CHECK(e.sendMidiToSource("keys", on, 3) == MidiSend::Sent);
        renderPeak(e, 2, 512, 512);
        CHECK(renderPeak(e, 2, 512, 4410) == 0.0f);
        CHECK(e.setMasterGain(0.5f));
        CHECK(e.snapshot().masterGain == 0.5f);
        renderPeak(e, 2, 512, 512);
        float half = renderPeak(e, 2, 512, 8820);
        std::printf("peak at master 0.5: %.3f\n", half);
        CHECK(half > 0.05f && half < peak * 0.6f);
        CHECK(e.sendMidiToSource("keys", off, 3) == MidiSend::Sent);

        // Audio routing: send plugin "a" to output 1 only.
        CHECK(e.setAudioRoutes("a", {{"a", 0, 0, 1, 1.0f}}, err));
        CHECK(e.setAudioRoutes("b", {}, err));
        session = e.saveSessionJson();
        e.stop();
    }

    // Both instances are destroyed now; check what they received.
    auto lines = readLines(logPath);
    for (auto& l : lines) std::printf("  log: %s\n", l.c_str());
    CHECK(containsInOrder(lines, {"midi port=0: 90 3C 64", "sysex port=0: F0 7E 7F 06 01 F7", "sysex port=0: F0 7D 01 F7",
                                  "midi port=0: B0 07 7F",
                                  "midi port=0: 80 3C 00", "midi port=0: 91 40 00"}));
    CHECK(containsInOrder(lines, {"note-on port=0: ch=0 key=60 vel=0.7874", "note-off port=0: ch=0 key=60",
                                  "note-off port=0: ch=1 key=64"}));

    // A new configuration restarts the engine. MIDI still waiting, not yet taken or timed
    // ahead, is delivered after it, and render positions run on.
    {
        Engine e;
        std::string err;
        EngineConfig cfg;
        cfg.blockSize = 128;
        CHECK(e.setConfig(cfg, err));
        CHECK(e.addPlugin({"a", synth, "brack.test.synth", "", std::nullopt}, true, err) == "a");
        CHECK(e.addMidiSource({"keys", MidiSourceKind::Api, ""}, err) == "keys");
        CHECK(e.addMidiRoute({"keys", "a", 0}, err));
        CHECK(e.startManual(48000, 2, 256, err));
        const uint8_t on[] = {0x92, 70, 0x64}, off[] = {0x82, 70, 0}, on2[] = {0x92, 72, 0x64}, off2[] = {0x82, 72, 0};
        CHECK(e.sendMidiToSource("keys", on, 3) == MidiSend::Sent);
        renderPeak(e, 2, 256, 1024);
        const uint64_t pos = e.renderPosition();
        CHECK(e.sendMidiToSource("keys", on2, 3, pos + 512) == MidiSend::Sent);
        CHECK(e.sendMidiToSource("keys", off2, 3, pos + 1024) == MidiSend::Sent);
        renderPeak(e, 2, 256, 256);
        CHECK(e.sendMidiToSource("keys", off, 3) == MidiSend::Sent);
        cfg.blockSize = 64;
        CHECK(e.setConfig(cfg, err));
        CHECK(e.renderPosition() == pos + 256);
        renderPeak(e, 2, 256, 2048);
        e.stop();
    }
    lines = readLines(logPath);
    CHECK(containsInOrder(lines, {"midi port=0: 92 46 64", "midi port=0: 82 46 00", "midi port=0: 92 48 64",
                                  "midi port=0: 82 48 00"}));

    // Session round trip.
    {
        Engine e;
        std::string err;
        CHECK(e.loadSessionJson(session, err));
        auto s = e.snapshot();
        CHECK(s.plugins.size() == 2);
        CHECK(s.plugins[0].name == "Lead");  // the display name survives the session round trip
        CHECK(s.sources.size() == 1 && s.sources[0].id == "keys");
        CHECK(s.midiRoutes.size() == 2);
        CHECK(s.audioRoutes.size() == 1 && s.audioRoutes[0].output == 1);
        CHECK(s.config.processSampleRate == 96000 && s.config.blockSize == 128);
        CHECK(s.masterGain == 0.5f);  // saved with the session
        CHECK(e.saveSessionJson() == session);

        // A session that turns out to be broken part way through leaves the rack and the
        // running engine as they were.
        CHECK(e.startManual(48000, 2, 256, err));
        const std::string before = e.saveSessionJson();
        const uint64_t changes = e.changeCount();
        const char* broken[] = {
            R"({"format":"brack-session","plugins":[{"id":5}]})",
            R"({"format":"brack-session","plugins":[{"id":"x","state":7}]})",
            R"({"format":"brack-session","midiSources":[{"id":"k","kind":"api"}],"audioRoutes":[{"plugin":"x","gain":"loud"}]})",
            R"({"format":"brack-session","midiRoutes":[{"source":1}]})",
        };
        for (const char* text : broken) {
            err.clear();
            CHECK(!e.loadSessionJson(text, err));
            std::printf("broken session: %s\n", err.c_str());
            CHECK(err.find("invalid session") != std::string::npos);
            CHECK(e.saveSessionJson() == before);
            CHECK(e.snapshot().mode == EngineMode::Manual);
            CHECK(e.changeCount() == changes);
        }

        CHECK(e.removePlugin("a", err));
        CHECK(e.snapshot().midiRoutes.size() == 1);
    }

    // A session that loads but names an output device that is not there: the load succeeds
    // (the rack has been replaced, which callers must be told) and the engine stays stopped.
    {
        Engine e;
        std::string err;
        if (!e.startDevice(err)) {
            std::printf("skipped the device restart check: %s\n", err.c_str());
        } else {
            const std::string missing = R"({"format":"brack-session","audio":{"device":"brack test: no such device"},
                "midiSources":[{"id":"only","kind":"api"}]})";
            err.clear();
            CHECK(e.loadSessionJson(missing, err));
            std::printf("device restart: %s\n", err.empty() ? "(no error)" : err.c_str());
            auto s = e.snapshot();
            CHECK(s.sources.size() == 1 && s.sources[0].id == "only");
            CHECK(s.mode == EngineMode::Stopped);
        }
    }

    chains(synth, argv[2], argv[3], argv[4]);
    sessionIds(synth, argv[2]);
    editWhilePlaying(synth, argv[2]);
    effectLoadsAside(synth, argv[2]);
    midiWhileChanging(synth, argv[2]);
    meters(synth, argv[2]);

    std::printf(g_failures ? "FAILED (%d)\n" : "PASS\n", g_failures);
    return g_failures ? 1 : 0;
}

int main(int argc, char** argv) { return brack::runWithMainLoop([&] { return run(argc, argv); }); }
