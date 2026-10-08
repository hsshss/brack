#include "report.h"

#include <nlohmann/json.hpp>

namespace brack {

using nlohmann::json;

static const char* modeName(EngineMode m) {
    switch (m) {
        case EngineMode::Stopped: return "stopped";
        case EngineMode::Device: return "device";
        case EngineMode::Manual: return "manual";
    }
    return "stopped";
}

std::string snapshotToJson(const EngineSnapshot& s) {
    json j;
    j["mode"] = modeName(s.mode);
    j["device"] = s.deviceName;
    j["deviceStalled"] = s.deviceStalled;
    j["outputSampleRate"] = s.outputSampleRate;
    j["processSampleRate"] = s.processSampleRate;
    j["outputChannels"] = s.outputChannels;
    j["periodFrames"] = s.periodFrames;
    j["blockSize"] = s.config.blockSize;
    j["resampling"] = s.resampling;
    j["resamplerQuality"] = resamplerQualityName(s.config.resamplerQuality);
    j["resamplerLatencyMs"] = s.resamplerLatencyMs;
    j["masterGain"] = s.masterGain;
    j["cpuLoad"] = s.cpuLoad;
    j["outputPeaks"] = s.outputPeaks;
    json plugins = json::array();
    for (auto& p : s.plugins) {
        json np = json::array(), ao = json::array();
        for (auto& n : p.notePorts)
            np.push_back({{"name", n.name},
                          {"midi", (n.dialects & CLAP_NOTE_DIALECT_MIDI) != 0},
                          {"clap", (n.dialects & CLAP_NOTE_DIALECT_CLAP) != 0}});
        for (auto& a : p.audioOutputs) ao.push_back({{"name", a.name}, {"channels", a.channels}, {"main", a.isMain}});
        plugins.push_back({{"id", p.id},
                           {"name", p.name},
                           {"format", pluginFormatName(p.format)},
                           {"pluginId", p.pluginId},
                           {"path", p.path},
                           {"status", p.status},
                           {"loaded", p.loaded},
                           {"active", p.active},
                           {"failed", p.failed},
                           {"crashed", p.crashed},
                           {"architecture", p.architecture},
                           {"separateProcess", p.separateProcess},
                           {"hasGui", p.hasGui},
                           {"guiOpen", p.guiOpen},
                           {"latency", p.latency},
                           {"notePorts", np},
                           {"audioOutputs", ao}});
    }
    j["plugins"] = plugins;
    json sources = json::array();
    for (auto& src : s.sources)
        sources.push_back({{"id", src.id},
                           {"kind", midiSourceKindName(src.kind)},
                           {"name", src.name},
                           {"ok", src.ok},
                           {"status", src.status},
                           {"messages", src.messages},
                           {"dropped", src.dropped}});
    j["midiSources"] = sources;
    json mr = json::array();
    for (auto& r : s.midiRoutes) mr.push_back({{"source", r.source}, {"plugin", r.plugin}, {"notePort", r.notePort}});
    j["midiRoutes"] = mr;
    json ar = json::array();
    for (auto& r : s.audioRoutes)
        ar.push_back({{"plugin", r.plugin}, {"port", r.port}, {"channel", r.channel}, {"output", r.output}, {"gain", r.gain}});
    j["audioRoutes"] = ar;
    return j.dump(2);
}

std::string audioDevicesToJson(const std::vector<AudioDeviceInfo>& devices) {
    json j = json::array();
    for (auto& d : devices)
        j.push_back({{"name", d.name}, {"default", d.isDefault}, {"sampleRates", d.sampleRates}, {"maxChannels", d.maxChannels}});
    return j.dump(2);
}

std::string midiInputsToJson(const std::vector<std::string>& names) { return json(names).dump(2); }

std::string pluginsToJson(const std::vector<PluginDescription>& plugins) {
    json j = json::array();
    for (auto& p : plugins)
        j.push_back({{"format", pluginFormatName(p.format)},
                     {"path", p.path},
                     {"id", p.id},
                     {"name", p.name},
                     {"vendor", p.vendor},
                     {"version", p.version},
                     {"instrument", p.isInstrument()},
                     {"architecture", p.architecture},
                     {"features", p.features}});
    return j.dump(2);
}

}  // namespace brack
