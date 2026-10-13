// Session (de)serialisation for the engine.
#include <fstream>
#include <nlohmann/json.hpp>
#include <sstream>

#include "engine.h"
#include "engine_internal.h"
#include "host_thread.h"
#include "plugin/plugin_files.h"
#include "util/common.h"
#include "util/midi_queue.h"

namespace brack {

using json = nlohmann::ordered_json;  // keep keys in a readable order in saved files

std::string Engine::saveSessionJson() {
    json j;
    host_->invoke([&] {
        j["format"] = "brack-session";
        j["engine"] = {{"processSampleRate", config_.processSampleRate},
                       {"blockSize", config_.blockSize},
                       {"resamplerQuality", resamplerQualityName(config_.resamplerQuality)},
                       {"pluginsInProcess", config_.pluginsInProcess},
                       {"loadPluginsSerially", config_.loadPluginsSerially}};
        j["audio"] = {{"device", config_.audio.deviceName},
                      {"sampleRate", config_.audio.sampleRate},
                      {"channels", config_.audio.channels},
                      {"bufferFrames", config_.audio.bufferFrames},
                      {"exclusive", config_.audio.exclusive}};
        j["masterGain"] = masterGain_.load();
        json sources = json::array();
        for (auto& s : sources_)
            sources.push_back({{"id", s->cfg.id}, {"kind", midiSourceKindName(s->cfg.kind)}, {"name", s->cfg.name}});
        j["midiSources"] = sources;

        auto plugin = [](const PluginConfig& cfg, HostedPlugin* inst) {
            json jp = {{"id", cfg.id}, {"path", cfg.path}, {"pluginId", cfg.pluginId}, {"name", cfg.name}};
            std::vector<uint8_t> state;
            if (inst) {
                if (inst->saveState(state)) jp["state"] = base64Encode(state);
            } else if (cfg.state) {
                jp["state"] = base64Encode(*cfg.state);  // keep what we could not load
            }
            return jp;
        };
        auto effects = [&](const PluginSlot& s) {
            json list = json::array();
            for (auto& e : s.effects) {
                json je = plugin(e.cfg, e.inst);
                je["bypass"] = e.bypass;
                list.push_back(je);
            }
            return list;
        };
        json plugins = json::array();
        for (auto& p : plugins_) {
            json jp = plugin(p->cfg, p->instrument());
            if (!p->effects.empty()) jp["effects"] = effects(*p);
            plugins.push_back(jp);
        }
        j["plugins"] = plugins;
        if (!master_->effects.empty()) j["masterEffects"] = effects(*master_);

        json mr = json::array();
        for (auto& r : midiRoutes_) mr.push_back({{"source", r.source}, {"plugin", r.plugin}, {"notePort", r.notePort}});
        j["midiRoutes"] = mr;
        json ar = json::array();
        for (auto& r : audioRoutes_)
            ar.push_back({{"plugin", r.plugin}, {"port", r.port}, {"channel", r.channel}, {"output", r.output}, {"gain", r.gain}});
        j["audioRoutes"] = ar;
    });
    return j.dump(2);
}

void Engine::clear() {
    host_->invoke([&] {
        {
            std::unique_lock lock(injectMutex_);
            injectPlugins_.clear();
            injectSources_.clear();
        }
        for (auto& s : sources_) s->port.reset();
        for (PluginSlot* p : slots()) p->closeEditors();
        if (mode_ == EngineMode::Device) {  // see quiesceSlot() for manual mode
            for (PluginSlot* p : slots())
                if (p->chain && p->chain->isActive()) p->chain->requestStopProcessing();
            for (PluginSlot* p : slots())
                if (p->chain && p->chain->isActive())
                    waitFor([&] { return p->chain->stopProcessingAcknowledged(); }, std::chrono::milliseconds(2000));
        }
        auto oldPlugins = std::move(plugins_);
        auto oldMaster = std::exchange(master_, std::make_unique<PluginSlot>());
        auto oldSources = std::move(sources_);
        plugins_.clear();
        sources_.clear();
        midiRoutes_.clear();
        audioRoutes_.clear();
        if (mode_ != EngineMode::Stopped) publishGraph();  // empty graph; waits for the audio thread
        oldPlugins.clear();
        oldMaster.reset();
        oldSources.clear();
    });
    markChanged();
}

namespace {

// A session file read in full. Nothing in the engine changes until the whole file has been
// read, so a file broken anywhere leaves the rack (and the running engine) as it was.
struct SessionData {
    EngineConfig config;
    float masterGain = 1.0f;
    std::vector<MidiSourceConfig> sources;
    struct Plugin {
        PluginConfig cfg;
        std::vector<Engine::EffectSlot> effects;
    };
    std::vector<Plugin> plugins;
    std::vector<Engine::EffectSlot> masterEffects;
    std::vector<MidiRoute> midiRoutes;
    std::vector<AudioRoute> audioRoutes;
    std::vector<std::string> problems;  // entries skipped while reading; logged when applied
};

bool parseSession(const std::string& text, SessionData& out, std::string& error) {
    json j;
    try {
        j = json::parse(text);
    } catch (const std::exception& e) {
        error = std::string("invalid session JSON: ") + e.what();
        return false;
    }
    if (j.value("format", "") != "brack-session") {
        error = "not a Brack session file";
        return false;
    }

    SessionData s;
    try {
        auto je = j.value("engine", json::object());
        s.config.processSampleRate = je.value("processSampleRate", 0u);
        s.config.blockSize = je.value("blockSize", 256u);
        if (!parseResamplerQuality(je.value("resamplerQuality", "high"), s.config.resamplerQuality))
            s.config.resamplerQuality = ResamplerQuality::High;
        s.config.pluginsInProcess = je.value("pluginsInProcess", false);
        s.config.loadPluginsSerially = je.value("loadPluginsSerially", false);
        auto ja = j.value("audio", json::object());
        s.config.audio.deviceName = ja.value("device", "");
        s.config.audio.sampleRate = ja.value("sampleRate", 0u);
        s.config.audio.channels = ja.value("channels", 2u);
        s.config.audio.bufferFrames = ja.value("bufferFrames", 256u);
        s.config.audio.exclusive = ja.value("exclusive", false);
        s.masterGain = j.value("masterGain", 1.0f);
    } catch (const std::exception& e) {
        error = std::string("invalid session settings: ") + e.what();
        return false;
    }

    try {
        for (auto& js : j.value("midiSources", json::array())) {
            MidiSourceConfig sc;
            sc.id = js.value("id", "");
            sc.name = js.value("name", "");
            if (!parseMidiSourceKind(js.value("kind", "api"), sc.kind)) {
                s.problems.push_back("unknown MIDI source kind for " + sc.id);
                continue;
            }
            s.sources.push_back(std::move(sc));
        }
        auto plugin = [](const json& jp) {
            PluginConfig pc;
            pc.id = jp.value("id", "");
            pc.path = jp.value("path", "");
            pc.pluginId = jp.value("pluginId", "");
            pc.name = jp.value("name", "");
            if (jp.contains("state")) {
                std::vector<uint8_t> st;
                if (base64Decode(jp["state"].get<std::string>(), st)) pc.state = std::move(st);
            }
            return pc;
        };
        auto effects = [&](const json& list) {
            std::vector<Engine::EffectSlot> out;
            for (auto& je : list) {
                Engine::EffectSlot e;
                e.cfg = plugin(je);
                e.bypass = je.value("bypass", false);
                out.push_back(std::move(e));
            }
            return out;
        };
        for (auto& jp : j.value("plugins", json::array()))
            s.plugins.push_back({plugin(jp), effects(jp.value("effects", json::array()))});
        s.masterEffects = effects(j.value("masterEffects", json::array()));
        for (auto& jr : j.value("midiRoutes", json::array()))
            s.midiRoutes.push_back({jr.value("source", ""), jr.value("plugin", ""), (uint16_t)jr.value("notePort", 0)});
        for (auto& jr : j.value("audioRoutes", json::array())) {
            AudioRoute r;
            r.plugin = jr.value("plugin", "");
            r.port = jr.value("port", 0u);
            r.channel = jr.value("channel", 0u);
            r.output = jr.value("output", 0u);
            r.gain = jr.value("gain", 1.0f);
            s.audioRoutes.push_back(std::move(r));
        }
    } catch (const std::exception& e) {
        error = std::string("invalid session content: ") + e.what();
        return false;
    }
    out = std::move(s);
    return true;
}

}  // namespace

bool Engine::loadSessionJson(const std::string& text, std::string& error) {
    SessionData s;
    if (!parseSession(text, s, error)) return false;

    clear();
    if (!setMasterGain(s.masterGain)) setMasterGain(1.0f);
    EngineMode prevMode;
    uint32_t prevRate, prevCh, prevFrames;
    host_->invoke([&] {
        prevMode = mode_;
        prevRate = outRate_;
        prevCh = outChannels_;
        prevFrames = periodFrames_;
        stopRunning();
        config_ = s.config;
    });

    int problems = 0;
    auto warn = [&](const std::string& m) {
        logWarn("session: " + m);
        ++problems;
    };
    for (auto& m : s.problems) warn(m);
    for (auto& sc : s.sources) {
        std::string err;
        if (addMidiSource(sc, err).empty()) warn(err);  // a missing port still adds the source
    }
    // Chains made all at once, each in its own plugin host (those in this process one after
    // another on the host thread), then put in the rack in the session's order: a plugin that
    // takes long to load, or to take its state, does not hold up the others. loadPluginsSerially
    // makes them one after another instead.
    struct Made {
        std::unique_ptr<PluginSlot> slot = std::make_unique<PluginSlot>();
        bool stateRejected = false;
        std::string error;
    };
    std::vector<Made> made(s.plugins.size());
    auto make = [&](size_t i) {
        made[i].slot->cfg = s.plugins[i].cfg;
        made[i].slot->effects = s.plugins[i].effects;
        try {
            made[i].slot->chain = makeChain(*made[i].slot, made[i].stateRejected, made[i].error);
        } catch (const std::exception& e) {
            made[i].error = s.plugins[i].cfg.path + ": " + e.what();
        }
    };
    // On the host thread, other threads would wait for it forever (makeChain() asks it things).
    if (host_->isCurrent() || s.config.loadPluginsSerially) {
        for (size_t i = 0; i < made.size(); ++i) make(i);
    } else {
        parallelFor(made.size(), make);
    }
    for (size_t i = 0; i < s.plugins.size(); ++i) {
        const PluginConfig& pc = s.plugins[i].cfg;
        std::unique_ptr<PluginSlot>& slot = made[i].slot;
        std::string err = made[i].error;
        std::string id;
        for (auto& e : slot->effects)
            if (!e.inst && slot->chain) warn(e.cfg.id + ": " + e.status);
        if (slot->chain) host_->invoke([&] { id = installPlugin(std::move(slot), made[i].stateRejected, false, err); });
        if (id.empty()) {
            warn(pc.id + ": " + err);
            // Keep the slot so the plugin, its effects (and their states) survive a save.
            host_->invoke([&] {
                if (pc.id.empty() || findAny(pc.id)) return;
                auto kept = std::make_unique<PluginSlot>();
                kept->cfg = pc;
                kept->effects = s.plugins[i].effects;
                for (auto& e : kept->effects) e.status = "its instrument is not loaded";
                assignIds(*kept);
                kept->status = err;
                plugins_.push_back(std::move(kept));
                rebuildInjectionMaps();
            });
        }
    }
    host_->invoke([&] {
        master_->effects = s.masterEffects;
        if (master_->effects.empty()) return;
        assignIds(*master_);
        if (std::string err; !reloadSlot(*master_, err)) warn("master: " + err);
        for (auto& e : master_->effects) {
            if (e.inst) continue;
            if (!master_->chain) e.status = "the master's plugin host did not start";
            warn(e.cfg.id + ": " + e.status);
        }
        settleSlot(*master_);
    });
    for (auto& r : s.midiRoutes) {
        std::string err;
        bool known = false;
        host_->invoke([&] { known = findPlugin(r.plugin) && findPlugin(r.plugin)->instrument(); });
        if (!known) {
            host_->invoke([&] { midiRoutes_.push_back(r); });  // keep routes of unloaded plugins
            continue;
        }
        if (!addMidiRoute(r, err)) warn(err);
    }
    std::map<std::string, std::vector<AudioRoute>> byPlugin;
    for (auto& r : s.audioRoutes) byPlugin[r.plugin].push_back(r);
    for (auto& [plugin, routes] : byPlugin) {
        std::string err;
        if (!setAudioRoutes(plugin, routes, err)) warn(err);
    }
    if (problems) logWarn("session loaded with " + std::to_string(problems) + " problem(s)");

    // The rack has been replaced by now, so this is a successful load even if the engine
    // cannot run again (the session may name a device that is not there): a caller told
    // otherwise would take the old rack for still loaded.
    std::string err;
    bool restarted = true;
    if (prevMode == EngineMode::Device) restarted = startDevice(err);
    else if (prevMode == EngineMode::Manual) restarted = startManual(prevRate, prevCh, prevFrames, err);
    if (!restarted) logError("session loaded, but the engine could not start again: " + err);
    markChanged();
    return true;
}

bool Engine::saveSessionFile(const std::string& pathUtf8, std::string& error) {
    return writeFileAtomic(pathFromUtf8(pathUtf8), saveSessionJson(), error);
}

namespace {

bool readSessionFile(const std::string& pathUtf8, std::string& text, std::string& error) {
    std::ifstream f(pathFromUtf8(pathUtf8), std::ios::binary);
    if (!f) {
        error = "cannot read " + pathUtf8;
        return false;
    }
    std::stringstream ss;
    ss << f.rdbuf();
    text = ss.str();
    return true;
}

}  // namespace

bool Engine::loadSessionFile(const std::string& pathUtf8, std::string& error) {
    std::string text;
    return readSessionFile(pathUtf8, text, error) && loadSessionJson(text, error);
}

bool Engine::describeSessionFile(const std::string& pathUtf8, std::string& out, std::string& error) {
    std::string text;
    SessionData s;
    if (!readSessionFile(pathUtf8, text, error) || !parseSession(text, s, error)) return false;
    auto plugin = [](const PluginConfig& cfg) {
        PluginFormat format = PluginFormat::Clap;  // an unknown one, as brack_get_status has it
        pluginFormatFromPath(pathFromUtf8(cfg.path), format);
        return json{{"id", cfg.id},
                    {"name", cfg.name},
                    {"format", pluginFormatName(format)},
                    {"path", cfg.path},
                    {"pluginId", cfg.pluginId}};
    };
    auto effects = [&](const std::vector<EffectSlot>& list) {
        json a = json::array();
        for (auto& e : list) {
            json je = plugin(e.cfg);
            je["bypass"] = e.bypass;
            je["effects"] = json::array();
            a.push_back(je);
        }
        return a;
    };
    json plugins = json::array();
    for (auto& p : s.plugins) {
        json jp = plugin(p.cfg);
        jp["bypass"] = false;
        jp["effects"] = effects(p.effects);
        plugins.push_back(jp);
    }
    out = json{{"plugins", plugins}, {"masterEffects", effects(s.masterEffects)}}.dump(2);
    return true;
}

}  // namespace brack
