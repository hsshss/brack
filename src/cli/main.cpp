// brack command line tool.
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include "engine.h"
#include "midi/midi_input.h"
#include "report.h"
#include "util/common.h"
#include "util/main_thread.h"

#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>
#else
#include <csignal>
#endif

using namespace brack;

namespace {

std::atomic<bool> g_quit{false};
uint32_t g_durationSec = 0;  // 0 = until Ctrl+C

#ifdef _WIN32
BOOL WINAPI onConsoleCtrl(DWORD) {
    g_quit = true;
    return TRUE;  // handled: let main() shut down cleanly (virtual MIDI ports must be torn down)
}
#else
void onSignal(int) { g_quit = true; }  // as above, for Ctrl+C and kill
#endif

void usage() {
    std::puts(
        "Brack - CLAP / VST3 / VST2 instrument host\n"
        "\n"
        "usage:\n"
        "  brack-cli devices [--json]              list audio output devices\n"
        "  brack-cli midi-inputs [--json]          list MIDI input ports\n"
        "  brack-cli scan [--dir DIR]... [--json]  list plugins in the standard locations (+ DIR)\n"
        "  brack-cli virtual-check                 tell whether virtual MIDI ports can be published\n"
        "  brack-cli run SESSION.json [--gui]      run a session until Ctrl+C\n"
        "  brack-cli play PLUGIN [options]         host one plugin (.clap, .vst3 or VST2 .dll) until Ctrl+C\n"
        "\n"
        "play options:\n"
        "  --plugin-id ID     plugin inside the file (default: first)\n"
        "  --virtual NAME     publish a virtual MIDI port NAME routed to the plugin (repeatable)\n"
        "  --midi-in NAME     open MIDI input NAME and route it to the plugin (repeatable)\n"
        "  --gui              open the plugin editor\n"
        "  --save FILE        write the resulting session to FILE on exit\n"
        "\n"
        "audio options (run/play; override the session):\n"
        "  --device NAME      output device (default: system default)\n"
        "  --exclusive        WASAPI exclusive mode (Windows; ignored elsewhere)\n"
        "  --device-rate HZ   device rate (exclusive mode only)\n"
        "  --buffer FRAMES    device buffer size\n"
        "  --channels N       output channels\n"
        "  --rate HZ          plugin processing rate (converted to the device rate)\n"
        "  --block FRAMES     plugin block size\n"
        "  --quality Q        resampler quality: standard | high | ultra\n"
        "  --in-process       run plugins inside brack-cli, not in a plugin host process each\n"
        "  --load-serially    load a session's plugins one after another, not all at once\n"
        "  --duration SEC     stop after SEC seconds instead of waiting for Ctrl+C");
}

struct Args {
    std::vector<std::string> list;
    size_t i = 0;
    bool more() const { return i < list.size(); }
    std::string next() { return i < list.size() ? list[i++] : std::string(); }
};

bool parseAudioOption(const std::string& a, Args& args, EngineConfig& cfg, bool& handled) {
    handled = true;
    auto num = [&](uint32_t& dst) {
        std::string v = args.next();
        try {
            dst = (uint32_t)std::stoul(v);
            return true;
        } catch (...) {
            std::fprintf(stderr, "%s needs a number\n", a.c_str());
            return false;
        }
    };
    if (a == "--device") cfg.audio.deviceName = args.next();
    else if (a == "--exclusive") cfg.audio.exclusive = true;
    else if (a == "--in-process") cfg.pluginsInProcess = true;
    else if (a == "--load-serially") cfg.loadPluginsSerially = true;
    else if (a == "--device-rate") return num(cfg.audio.sampleRate);
    else if (a == "--buffer") return num(cfg.audio.bufferFrames);
    else if (a == "--channels") return num(cfg.audio.channels);
    else if (a == "--rate") return num(cfg.processSampleRate);
    else if (a == "--block") return num(cfg.blockSize);
    else if (a == "--duration") return num(g_durationSec);
    else if (a == "--quality") {
        if (!parseResamplerQuality(args.next(), cfg.resamplerQuality)) {
            std::fprintf(stderr, "--quality must be standard, high or ultra\n");
            return false;
        }
    } else handled = false;
    return true;
}

void printSummary(Engine& engine) {
    auto s = engine.snapshot();
    std::printf("output : %s, %u Hz, %u ch, period %u\n", s.deviceName.c_str(), s.outputSampleRate, s.outputChannels,
                s.periodFrames);
    std::printf("plugins: %u Hz, block %u%s\n", s.processSampleRate, s.config.blockSize,
                s.resampling ? (std::string(", SRC ") + resamplerQualityName(s.config.resamplerQuality) + " (" +
                                std::to_string(s.resamplerLatencyMs).substr(0, 5) + " ms)")
                                   .c_str()
                             : "");
    for (auto& p : s.plugins)
        std::printf("plugin : %-16s %s [%s]\n", p.id.c_str(), p.name.c_str(), p.status.c_str());
    for (auto& src : s.sources)
        std::printf("midi   : %-16s %s \"%s\" [%s]\n", src.id.c_str(), midiSourceKindName(src.kind), src.name.c_str(),
                    src.status.c_str());
    for (auto& r : s.midiRoutes) std::printf("route  : %s -> %s:%u\n", r.source.c_str(), r.plugin.c_str(), r.notePort);
}

int waitForQuit(Engine& engine) {
    std::puts(g_durationSec ? "running" : "running - press Ctrl+C to stop");
    auto end = std::chrono::steady_clock::now() + std::chrono::seconds(g_durationSec);
    while (!g_quit && (!g_durationSec || std::chrono::steady_clock::now() < end))
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    auto s = engine.snapshot();
    std::printf("stopping... (CPU %.1f%%)\n", s.cpuLoad * 100.0f);
    engine.stop();
    return 0;
}

int cmdRun(Args& args) {
    std::string session = args.next();
    if (session.empty()) {
        usage();
        return 2;
    }
    Engine engine;
    std::string err;
    if (!engine.loadSessionFile(session, err)) {
        std::fprintf(stderr, "error: %s\n", err.c_str());
        return 1;
    }
    EngineConfig cfg = engine.config();
    bool gui = false;
    while (args.more()) {
        std::string a = args.next();
        bool handled;
        if (!parseAudioOption(a, args, cfg, handled)) return 2;
        if (handled) continue;
        if (a == "--gui") gui = true;
        else {
            std::fprintf(stderr, "unknown option %s\n", a.c_str());
            return 2;
        }
    }
    if (!engine.setConfig(cfg, err) || !engine.startDevice(err)) {
        std::fprintf(stderr, "error: %s\n", err.c_str());
        return 1;
    }
    if (gui)
        for (auto& p : engine.snapshot().plugins)
            if (p.hasGui && !engine.setPluginGuiVisible(p.id, true, err)) std::fprintf(stderr, "gui: %s\n", err.c_str());
    printSummary(engine);
    return waitForQuit(engine);
}

int cmdPlay(Args& args) {
    PluginConfig pc;
    pc.path = args.next();
    if (pc.path.empty()) {
        usage();
        return 2;
    }
    EngineConfig cfg;
    std::vector<std::string> virtuals, inputs;
    bool gui = false;
    std::string saveTo;
    while (args.more()) {
        std::string a = args.next();
        bool handled;
        if (!parseAudioOption(a, args, cfg, handled)) return 2;
        if (handled) continue;
        if (a == "--plugin-id") pc.pluginId = args.next();
        else if (a == "--virtual") virtuals.push_back(args.next());
        else if (a == "--midi-in") inputs.push_back(args.next());
        else if (a == "--gui") gui = true;
        else if (a == "--save") saveTo = args.next();
        else {
            std::fprintf(stderr, "unknown option %s\n", a.c_str());
            return 2;
        }
    }

    Engine engine;
    std::string err;
    engine.setConfig(cfg, err);
    std::string pid = engine.addPlugin(pc, true, err);
    if (pid.empty()) {
        std::fprintf(stderr, "error: %s\n", err.c_str());
        return 1;
    }
    auto connect = [&](MidiSourceKind kind, const std::string& name) {
        std::string sid = engine.addMidiSource({"", kind, name}, err);
        if (sid.empty() || !engine.addMidiRoute({sid, pid, 0}, err)) std::fprintf(stderr, "midi: %s\n", err.c_str());
    };
    for (auto& v : virtuals) connect(MidiSourceKind::Virtual, v);
    for (auto& in : inputs) connect(MidiSourceKind::Hardware, in);

    if (!engine.startDevice(err)) {
        std::fprintf(stderr, "error: %s\n", err.c_str());
        return 1;
    }
    if (gui && !engine.setPluginGuiVisible(pid, true, err)) std::fprintf(stderr, "gui: %s\n", err.c_str());
    printSummary(engine);
    int rc = waitForQuit(engine);
    if (!saveTo.empty()) {
        if (!engine.saveSessionFile(saveTo, err)) std::fprintf(stderr, "save: %s\n", err.c_str());
        else std::printf("session saved to %s\n", saveTo.c_str());
    }
    return rc;
}

int run(int argc, char** argv) {
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCtrlHandler(&onConsoleCtrl, TRUE);
    // Plugin editors (--in-process) scale by the process's DPI awareness (see src/host/main.cpp).
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    // Use the UTF-8 command line regardless of the active code page.
    int wargc = 0;
    LPWSTR* wargv = CommandLineToArgvW(GetCommandLineW(), &wargc);
    Args args;
    for (int i = 1; i < wargc; ++i) args.list.push_back(narrow(wargv[i]));
    LocalFree(wargv);
#else
    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);
    Args args;
    for (int i = 1; i < argc; ++i) args.list.emplace_back(argv[i]);
#endif
    (void)argc;
    (void)argv;

    std::string cmd = args.next();
    auto hasJson = [&] {
        for (auto& a : args.list)
            if (a == "--json") return true;
        return false;
    };

    if (cmd == "devices") {
        auto devices = listAudioOutputDevices();
        if (hasJson()) std::puts(audioDevicesToJson(devices).c_str());
        else
            for (auto& d : devices) {
                std::string rates;
                for (auto r : d.sampleRates) rates += (rates.empty() ? "" : ", ") + std::to_string(r);
                std::printf("%s %s  (%s Hz)\n", d.isDefault ? "*" : " ", d.name.c_str(), rates.c_str());
            }
        return 0;
    }
    if (cmd == "midi-inputs") {
        auto names = listHardwareMidiInputs();
        if (hasJson()) std::puts(midiInputsToJson(names).c_str());
        else
            for (auto& n : names) std::printf("%s\n", n.c_str());
        return 0;
    }
    if (cmd == "scan") {
        std::vector<std::string> dirs;
        while (args.more()) {
            std::string a = args.next();
            if (a == "--dir") dirs.push_back(args.next());
        }
        Engine engine;
        auto plugins = engine.scanPlugins(dirs);
        if (hasJson()) std::puts(pluginsToJson(plugins).c_str());
        else
            for (auto& p : plugins)
                std::printf("%s %-4s %-40s %s%s\n    %s\n", p.isInstrument() ? "[inst]" : "[    ]",
                            pluginFormatLabel(p.format), p.name.c_str(), p.id.c_str(),
                            p.architecture == buildArchitecture() ? "" : (" (" + p.architecture + ")").c_str(),
                            p.path.c_str());
        return 0;
    }
    if (cmd == "virtual-check") {
        std::string reason;
        bool ok = virtualMidiAvailable(reason);
        std::printf("virtual MIDI ports: %s%s%s\n", ok ? "available" : "not available", reason.empty() ? "" : " - ",
                    reason.c_str());
        if (ok && virtualMidiRemovalHangsService())
            std::printf("warning: this Windows MIDI Services build deadlocks when a virtual port is removed\n"
                        "         (microsoft/MIDI#1047): after Brack exits, MIDI stops working until reboot.\n"
                        "         Fixed by the late-November 2026 Windows update.\n");
        return ok ? 0 : 1;
    }
    if (cmd == "run") return cmdRun(args);
    if (cmd == "play") return cmdPlay(args);
    usage();
    return cmd.empty() || cmd == "help" || cmd == "--help" || cmd == "-h" ? 0 : 2;
}

}  // namespace

int main(int argc, char** argv) { return runWithMainLoop([&] { return run(argc, argv); }); }
