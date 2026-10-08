// End-to-end engine test in manual render mode with the test synth.
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
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

}  // namespace

int run(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: test_engine <brack-test-synth.clap>\n");
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

    std::printf(g_failures ? "FAILED (%d)\n" : "PASS\n", g_failures);
    return g_failures ? 1 : 0;
}

int main(int argc, char** argv) { return brack::runWithMainLoop([&] { return run(argc, argv); }); }
