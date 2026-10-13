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
    auto levels = [&](json& to, const char* name, const std::vector<EngineSnapshot::Level>& list) {
        json peaks = json::array(), clips = json::array();
        for (auto& l : list) {
            peaks.push_back(l.peak);
            clips.push_back(l.clips);
        }
        to[std::string(name) + "Peaks"] = peaks;
        to[std::string(name) + "Clips"] = clips;
    };
    j["cpuLoad"] = s.load.average;
    j["cpuPeakLoad"] = s.load.peak;
    j["overruns"] = s.overruns;
    levels(j, "output", s.output);
    levels(j, "masterInput", s.masterInput);
    levels(j, "masterOutput", s.masterOutput);
    j["masterLoad"] = s.masterLoad.average;
    j["masterPeakLoad"] = s.masterLoad.peak;
    // An effect has the instrument's meters too, empty and zero, as it has "effects".
    auto plugin = [&](const EngineSnapshot::Plugin& p) {
        json np = json::array(), ao = json::array();
        for (auto& n : p.notePorts)
            np.push_back({{"name", n.name},
                          {"midi", (n.dialects & CLAP_NOTE_DIALECT_MIDI) != 0},
                          {"clap", (n.dialects & CLAP_NOTE_DIALECT_CLAP) != 0}});
        // Each port with its share of outputLevels; "main" is where the chain's effects act.
        const uint32_t main = p.audioOutputs.empty() ? 0 : mainPort(p.audioOutputs);
        size_t level = 0;
        for (uint32_t i = 0; i < p.audioOutputs.size(); ++i) {
            const AudioPortInfo& a = p.audioOutputs[i];
            json peaks = json::array(), clips = json::array();
            for (uint32_t c = 0; c < a.channels; ++c, ++level) {
                if (level >= p.outputLevels.size()) continue;
                peaks.push_back(p.outputLevels[level].peak);
                clips.push_back(p.outputLevels[level].clips);
            }
            ao.push_back(
                {{"name", a.name}, {"channels", a.channels}, {"main", i == main}, {"peaks", peaks}, {"clips", clips}});
        }
        json jp{{"id", p.id},
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
                {"audioOutputs", ao}};
        jp["load"] = p.load.average;
        jp["peakLoad"] = p.load.peak;
        jp["midiMessages"] = p.midiMessages;
        return jp;
    };
    auto effects = [&](const std::vector<EngineSnapshot::Plugin>& list) {
        json a = json::array();
        for (auto& e : list) {
            json je = plugin(e);
            je["bypass"] = e.bypass;
            je["effects"] = json::array();
            a.push_back(je);
        }
        return a;
    };
    json plugins = json::array();
    for (auto& p : s.plugins) {
        json jp = plugin(p);
        jp["bypass"] = false;
        jp["effects"] = effects(p.effects);
        plugins.push_back(jp);
    }
    j["plugins"] = plugins;
    j["masterEffects"] = effects(s.masterEffects);
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
