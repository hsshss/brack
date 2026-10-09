// C API wrapper around brack::Engine (brack.dll).
#include "brack/brack.h"

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <mutex>
#include <string>

#include "engine.h"
#include "midi/midi_input.h"
#include "report.h"
#include "util/common.h"

struct brack_engine {
    brack::Engine engine;
};

namespace {

thread_local std::string t_lastError;
// Set instead of t_lastError by the functions a real-time thread may call, which must not allocate.
thread_local const char* t_lastErrorLiteral = nullptr;

int fail(int code, std::string message) {
    t_lastErrorLiteral = nullptr;
    t_lastError = std::move(message);
    return code;
}
int failLiteral(int code, const char* message) {
    t_lastErrorLiteral = message;
    return code;
}
int failed(const std::string& message) { return fail(BRACK_ERR_FAILED, message); }
int ok() {
    t_lastErrorLiteral = nullptr;
    t_lastError.clear();
    return BRACK_OK;
}
std::string str(const char* s) { return s ? s : ""; }

int writeText(const std::string& text, char* buf, size_t size, size_t* needed) {
    if (needed) *needed = text.size() + 1;
    if (!buf || size == 0) return ok();
    if (size < text.size() + 1) return fail(BRACK_ERR_BUFFER_TOO_SMALL, "buffer too small");
    std::memcpy(buf, text.data(), text.size() + 1);
    return ok();
}

// Copies UTF-8 text, cut short (at a character boundary) if it does not fit.
void copyText(const std::string& text, char* out, size_t size) {
    if (!out || size == 0) return;
    size_t n = std::min(text.size(), size - 1);
    if (n < text.size())
        while (n > 0 && (static_cast<unsigned char>(text[n]) & 0xC0) == 0x80) --n;
    std::memcpy(out, text.data(), n);
    out[n] = 0;
}

static_assert(brack::Engine::kMaxGeneratedId + 1 == BRACK_ID_MAX);

// Checked before adding anything, so that an id cut short never reaches the caller.
bool idOutFits(const char* id, const char* idOut, size_t idOutSize) {
    if (!idOut) return true;
    return idOutSize >= (id && *id ? std::strlen(id) + 1 : size_t{BRACK_ID_MAX});
}

int writeBytes(const std::vector<uint8_t>& bytes, uint8_t* buf, size_t size, size_t* needed) {
    if (needed) *needed = bytes.size();
    if (!buf || size == 0) return ok();
    if (size < bytes.size()) return fail(BRACK_ERR_BUFFER_TOO_SMALL, "buffer too small");
    if (!bytes.empty()) std::memcpy(buf, bytes.data(), bytes.size());
    return ok();
}

std::vector<std::string> splitDirs(const char* list) {
    std::vector<std::string> dirs;
    std::string s = str(list);
    size_t start = 0;
    while (start < s.size()) {
        size_t end = s.find(';', start);
        if (end == std::string::npos) end = s.size();
        if (end > start) dirs.push_back(s.substr(start, end - start));
        start = end + 1;
    }
    return dirs;
}

int sendResult(brack::MidiSend result, const char* unknownId) {
    switch (result) {
        case brack::MidiSend::Sent: return BRACK_OK;
        case brack::MidiSend::Malformed: return failLiteral(BRACK_ERR_INVALID_ARGUMENT, "not one complete MIDI message");
        case brack::MidiSend::UnknownId: return failLiteral(BRACK_ERR_NOT_FOUND, unknownId);
        case brack::MidiSend::QueueFull: return failLiteral(BRACK_ERR_QUEUE_FULL, "MIDI queue full, message dropped");
    }
    return failLiteral(BRACK_ERR_FAILED, "message not sent");
}

thread_local std::string t_configDevice;  // brack_get_config's audio_device

// The engine names an unknown id in its error message only; these give the code.
bool hasPlugin(brack_engine* e, const char* id) {
    for (auto& p : e->engine.snapshot().plugins)
        if (p.id == str(id)) return true;
    return false;
}
bool hasSource(brack_engine* e, const char* id) {
    for (auto& s : e->engine.snapshot().sources)
        if (s.id == str(id)) return true;
    return false;
}
int pluginFailed(brack_engine* e, const char* pluginId, const std::string& error) {
    return fail(hasPlugin(e, pluginId) ? BRACK_ERR_FAILED : BRACK_ERR_NOT_FOUND, error);
}

template <typename Fn>
int guarded(brack_engine* e, Fn&& fn) {
    if (!e) return fail(BRACK_ERR_INVALID_ARGUMENT, "engine is NULL");
    try {
        return fn();
    } catch (const std::exception& ex) {
        return failed(ex.what());
    } catch (...) {
        return failed("unknown error");
    }
}

std::mutex g_logMutex;
brack_log_fn g_logFn = nullptr;
void* g_logUser = nullptr;

// The least a caller's struct_size may be: the structs' first layouts. Fields added later go after
// these, and leave them as they are.
constexpr size_t kFirstConfigSize = offsetof(brack_config, plugins_in_process) + sizeof(int32_t);
constexpr size_t kFirstEventSize = offsetof(brack_event, height) + sizeof(uint32_t);

// How much of the caller's struct both it and this library know.
template <typename T>
size_t sharedPart(const T& s) {
    return std::min<size_t>(s.struct_size, sizeof(T));
}

}  // namespace

extern "C" {

BRACK_API uint32_t BRACK_CALL brack_api_version(void) { return BRACK_API_VERSION; }
BRACK_API const char* BRACK_CALL brack_version_string(void) { return "Brack " BRACK_VERSION; }
BRACK_API const char* BRACK_CALL brack_last_error(void) {
    return t_lastErrorLiteral ? t_lastErrorLiteral : t_lastError.c_str();
}

BRACK_API void BRACK_CALL brack_set_log_callback(brack_log_fn fn, void* user) {
    {
        std::lock_guard lock(g_logMutex);
        g_logFn = fn;
        g_logUser = user;
    }
    if (!fn) {
        brack::setLogSink(nullptr);
        return;
    }
    brack::setLogSink([](brack::LogLevel level, const std::string& message) {
        std::lock_guard lock(g_logMutex);
        if (g_logFn) g_logFn(g_logUser, (int)level, message.c_str());
    });
}

BRACK_API brack_engine* BRACK_CALL brack_engine_create(void) {
    try {
        return new brack_engine();
    } catch (const std::exception& ex) {
        failed(ex.what());
        return nullptr;
    }
}

BRACK_API void BRACK_CALL brack_engine_destroy(brack_engine* e) { delete e; }

BRACK_API int BRACK_CALL brack_set_config(brack_engine* e, const brack_config* given) {
    return guarded(e, [&] {
        if (!given || given->struct_size < kFirstConfigSize) return fail(BRACK_ERR_INVALID_ARGUMENT, "invalid config");
        brack_config whole;
        brack_config_init(&whole);
        std::memcpy(&whole, given, sharedPart(*given));
        const brack_config* cfg = &whole;
        brack::EngineConfig c;
        c.audio.deviceName = str(cfg->audio_device);
        c.audio.sampleRate = cfg->device_sample_rate;
        c.audio.channels = cfg->channels ? cfg->channels : 2;
        c.audio.bufferFrames = cfg->buffer_frames ? cfg->buffer_frames : 256;
        c.audio.exclusive = cfg->exclusive != 0;
        c.processSampleRate = cfg->process_sample_rate;
        c.blockSize = cfg->block_size ? cfg->block_size : 256;
        switch (cfg->resampler_quality) {
            case BRACK_SRC_STANDARD: c.resamplerQuality = brack::ResamplerQuality::Standard; break;
            case BRACK_SRC_ULTRA: c.resamplerQuality = brack::ResamplerQuality::Ultra; break;
            default: c.resamplerQuality = brack::ResamplerQuality::High; break;
        }
        c.pluginsInProcess = cfg->plugins_in_process != 0;
        c.loadPluginsSerially = cfg->load_plugins_serially != 0;
        std::string err;
        return e->engine.setConfig(c, err) ? ok() : failed(err);
    });
}

BRACK_API int BRACK_CALL brack_get_config(brack_engine* e, brack_config* out) {
    return guarded(e, [&] {
        if (!out || out->struct_size < kFirstConfigSize) return fail(BRACK_ERR_INVALID_ARGUMENT, "invalid config");
        brack_config whole{};
        whole.struct_size = out->struct_size;
        brack_config* cfg = &whole;
        const brack::EngineConfig c = e->engine.config();
        t_configDevice = c.audio.deviceName;
        cfg->audio_device = t_configDevice.c_str();
        cfg->device_sample_rate = c.audio.sampleRate;
        cfg->channels = c.audio.channels;
        cfg->buffer_frames = c.audio.bufferFrames;
        cfg->exclusive = c.audio.exclusive ? 1 : 0;
        cfg->process_sample_rate = c.processSampleRate;
        cfg->block_size = c.blockSize;
        switch (c.resamplerQuality) {
            case brack::ResamplerQuality::Standard: cfg->resampler_quality = BRACK_SRC_STANDARD; break;
            case brack::ResamplerQuality::Ultra: cfg->resampler_quality = BRACK_SRC_ULTRA; break;
            default: cfg->resampler_quality = BRACK_SRC_HIGH; break;
        }
        cfg->plugins_in_process = c.pluginsInProcess ? 1 : 0;
        cfg->load_plugins_serially = c.loadPluginsSerially ? 1 : 0;
        std::memcpy(out, &whole, sharedPart(whole));
        return ok();
    });
}

BRACK_API int BRACK_CALL brack_start(brack_engine* e) {
    return guarded(e, [&] {
        std::string err;
        return e->engine.startDevice(err) ? ok() : failed(err);
    });
}

BRACK_API int BRACK_CALL brack_start_manual(brack_engine* e, uint32_t sampleRate, uint32_t channels, uint32_t maxFrames) {
    return guarded(e, [&] {
        std::string err;
        return e->engine.startManual(sampleRate, channels, maxFrames, err) ? ok() : failed(err);
    });
}

BRACK_API void BRACK_CALL brack_stop(brack_engine* e) {
    if (e) e->engine.stop();
}

BRACK_API void BRACK_CALL brack_render(brack_engine* e, float* const* outputs, uint32_t channels, uint32_t frames) {
    if (e && outputs) e->engine.render(outputs, channels, frames);
}

BRACK_API uint64_t BRACK_CALL brack_get_render_position(brack_engine* e) { return e ? e->engine.renderPosition() : 0; }

BRACK_API int BRACK_CALL brack_add_plugin_ex(brack_engine* e, const char* id, const char* path, const char* pluginId,
                                             uint32_t flags, char* idOut, size_t idOutSize) {
    return guarded(e, [&] {
        if (!path || !*path) return fail(BRACK_ERR_INVALID_ARGUMENT, "plugin_path is required");
        if (!idOutFits(id, idOut, idOutSize)) return fail(BRACK_ERR_BUFFER_TOO_SMALL, "id_out is too small");
        brack::PluginConfig pc;
        pc.id = str(id);
        pc.path = path;
        pc.pluginId = str(pluginId);
        std::string err;
        std::string newId = e->engine.addPlugin(pc, (flags & BRACK_ADD_PLUGIN_NO_AUDIO_ROUTES) == 0, err);
        if (newId.empty()) return failed(err);
        copyText(newId, idOut, idOutSize);
        return ok();
    });
}

BRACK_API int BRACK_CALL brack_add_plugin(brack_engine* e, const char* id, const char* path, const char* pluginId,
                                          char* idOut, size_t idOutSize) {
    return brack_add_plugin_ex(e, id, path, pluginId, 0, idOut, idOutSize);
}

BRACK_API int BRACK_CALL brack_move_plugin(brack_engine* e, const char* pluginId, uint32_t index) {
    return guarded(e, [&] {
        std::string err;
        return e->engine.movePlugin(str(pluginId), index, err) ? ok() : fail(BRACK_ERR_NOT_FOUND, err);
    });
}

BRACK_API int BRACK_CALL brack_remove_plugin(brack_engine* e, const char* pluginId) {
    return guarded(e, [&] {
        std::string err;
        return e->engine.removePlugin(str(pluginId), err) ? ok() : fail(BRACK_ERR_NOT_FOUND, err);
    });
}

BRACK_API int BRACK_CALL brack_reload_plugin(brack_engine* e, const char* pluginId) {
    return guarded(e, [&] {
        std::string err;
        return e->engine.reloadPlugin(str(pluginId), err) ? ok() : pluginFailed(e, pluginId, err);
    });
}

BRACK_API int BRACK_CALL brack_show_plugin_gui(brack_engine* e, const char* pluginId, int visible) {
    return guarded(e, [&] {
        std::string err;
        return e->engine.setPluginGuiVisible(str(pluginId), visible != 0, err) ? ok()
                                                                                 : pluginFailed(e, pluginId, err);
    });
}

BRACK_API int BRACK_CALL brack_show_plugin_gui_in(brack_engine* e, const char* pluginId, void* parentWindow) {
    return guarded(e, [&] {
        if (!parentWindow) return fail(BRACK_ERR_INVALID_ARGUMENT, "parent_window is required");
        std::string err;
        return e->engine.showPluginGuiIn(str(pluginId), parentWindow, err) ? ok() : pluginFailed(e, pluginId, err);
    });
}

BRACK_API int BRACK_CALL brack_get_plugin_state(brack_engine* e, const char* pluginId, uint8_t* buf, size_t size,
                                                size_t* needed) {
    return guarded(e, [&] {
        std::vector<uint8_t> state;
        std::string err;
        if (!e->engine.getPluginState(str(pluginId), state, err)) return pluginFailed(e, pluginId, err);
        return writeBytes(state, buf, size, needed);
    });
}

BRACK_API int BRACK_CALL brack_set_plugin_state(brack_engine* e, const char* pluginId, const uint8_t* data,
                                                size_t size) {
    return guarded(e, [&] {
        if (!data && size) return fail(BRACK_ERR_INVALID_ARGUMENT, "data is NULL");
        std::vector<uint8_t> state(data, data + size);
        std::string err;
        return e->engine.setPluginState(str(pluginId), state, err) ? ok() : pluginFailed(e, pluginId, err);
    });
}

BRACK_API int BRACK_CALL brack_set_plugin_name(brack_engine* e, const char* pluginId, const char* name) {
    return guarded(e, [&] {
        std::string err;
        return e->engine.setPluginName(str(pluginId), str(name), err) ? ok() : fail(BRACK_ERR_NOT_FOUND, err);
    });
}

BRACK_API int BRACK_CALL brack_add_midi_source(brack_engine* e, const char* id, int kind, const char* name, char* idOut,
                                               size_t idOutSize) {
    return guarded(e, [&] {
        if (!idOutFits(id, idOut, idOutSize)) return fail(BRACK_ERR_BUFFER_TOO_SMALL, "id_out is too small");
        brack::MidiSourceConfig sc;
        sc.id = str(id);
        sc.name = str(name);
        switch (kind) {
            case BRACK_SOURCE_HARDWARE: sc.kind = brack::MidiSourceKind::Hardware; break;
            case BRACK_SOURCE_VIRTUAL: sc.kind = brack::MidiSourceKind::Virtual; break;
            case BRACK_SOURCE_API: sc.kind = brack::MidiSourceKind::Api; break;
            default: return fail(BRACK_ERR_INVALID_ARGUMENT, "invalid source kind");
        }
        std::string err;
        std::string newId = e->engine.addMidiSource(sc, err);
        if (newId.empty()) return failed(err);
        // The engine keeps sources whose port failed to open (so sessions round-trip);
        // for API callers a failed open is simply an error.
        for (auto& s : e->engine.snapshot().sources)
            if (s.id == newId && !s.ok) {
                std::string status = s.status, ignored;
                e->engine.removeMidiSource(newId, ignored);
                return failed(status);
            }
        copyText(newId, idOut, idOutSize);
        return ok();
    });
}

BRACK_API int BRACK_CALL brack_remove_midi_source(brack_engine* e, const char* sourceId) {
    return guarded(e, [&] {
        std::string err;
        return e->engine.removeMidiSource(str(sourceId), err) ? ok() : fail(BRACK_ERR_NOT_FOUND, err);
    });
}

BRACK_API int BRACK_CALL brack_move_midi_source(brack_engine* e, const char* sourceId, uint32_t index) {
    return guarded(e, [&] {
        std::string err;
        return e->engine.moveMidiSource(str(sourceId), index, err) ? ok() : fail(BRACK_ERR_NOT_FOUND, err);
    });
}

BRACK_API int BRACK_CALL brack_reopen_midi_source(brack_engine* e, const char* sourceId) {
    return guarded(e, [&] {
        std::string err;
        if (e->engine.reopenMidiSource(str(sourceId), err)) return ok();
        return fail(hasSource(e, sourceId) ? BRACK_ERR_FAILED : BRACK_ERR_NOT_FOUND, err);
    });
}

BRACK_API int BRACK_CALL brack_connect_midi(brack_engine* e, const char* sourceId, const char* pluginId, uint32_t notePort) {
    return guarded(e, [&] {
        if (notePort > UINT16_MAX) return fail(BRACK_ERR_INVALID_ARGUMENT, "note_port out of range");
        std::string err;
        if (e->engine.addMidiRoute({str(sourceId), str(pluginId), (uint16_t)notePort}, err)) return ok();
        return fail(hasSource(e, sourceId) && hasPlugin(e, pluginId) ? BRACK_ERR_FAILED : BRACK_ERR_NOT_FOUND, err);
    });
}

BRACK_API int BRACK_CALL brack_disconnect_midi(brack_engine* e, const char* sourceId, const char* pluginId,
                                               uint32_t notePort) {
    return guarded(e, [&] {
        if (notePort > UINT16_MAX) return fail(BRACK_ERR_INVALID_ARGUMENT, "note_port out of range");
        return e->engine.removeMidiRoute({str(sourceId), str(pluginId), (uint16_t)notePort})
                   ? ok()
                   : fail(BRACK_ERR_NOT_FOUND, "no such route");
    });
}

BRACK_API int BRACK_CALL brack_connect_audio(brack_engine* e, const char* pluginId, uint32_t port, uint32_t channel,
                                             uint32_t output, float gain) {
    return guarded(e, [&] {
        std::string err;
        if (!e->engine.addAudioRoute({str(pluginId), port, channel, output, gain}, err))
            return fail(BRACK_ERR_NOT_FOUND, err);
        return ok();
    });
}

BRACK_API int BRACK_CALL brack_disconnect_audio(brack_engine* e, const char* pluginId, uint32_t port, uint32_t channel,
                                                uint32_t output) {
    return guarded(e, [&] {
        if (!e->engine.removeAudioRoute({str(pluginId), port, channel, output, 1.0f}))
            return fail(BRACK_ERR_NOT_FOUND, "no such route");
        return ok();
    });
}

BRACK_API int BRACK_CALL brack_clear_audio_routes(brack_engine* e, const char* pluginId) {
    return guarded(e, [&] {
        std::string err;
        return e->engine.setAudioRoutes(str(pluginId), {}, err) ? ok() : fail(BRACK_ERR_NOT_FOUND, err);
    });
}

BRACK_API int BRACK_CALL brack_set_master_gain(brack_engine* e, float gain) {
    if (!e) return failLiteral(BRACK_ERR_INVALID_ARGUMENT, "engine is NULL");
    return e->engine.setMasterGain(gain) ? BRACK_OK : failLiteral(BRACK_ERR_INVALID_ARGUMENT, "gain must be >= 0");
}

BRACK_API float BRACK_CALL brack_get_master_gain(brack_engine* e) { return e ? e->engine.masterGain() : 0.0f; }

BRACK_API int BRACK_CALL brack_send_midi(brack_engine* e, const char* pluginId, uint32_t notePort, const uint8_t* data,
                                         uint32_t size) {
    if (!e || !pluginId || !data || notePort > UINT16_MAX)
        return failLiteral(BRACK_ERR_INVALID_ARGUMENT, "invalid argument");
    return sendResult(e->engine.sendMidiToPlugin(pluginId, (uint16_t)notePort, data, size), "no such plugin");
}

BRACK_API int BRACK_CALL brack_send_midi_at(brack_engine* e, const char* pluginId, uint32_t notePort, uint64_t time,
                                            const uint8_t* data, uint32_t size) {
    if (!e || !pluginId || !data || notePort > UINT16_MAX)
        return failLiteral(BRACK_ERR_INVALID_ARGUMENT, "invalid argument");
    return sendResult(e->engine.sendMidiToPlugin(pluginId, (uint16_t)notePort, data, size, time), "no such plugin");
}

BRACK_API int BRACK_CALL brack_send_midi_to_source(brack_engine* e, const char* sourceId, const uint8_t* data,
                                                   uint32_t size) {
    if (!e || !sourceId || !data) return failLiteral(BRACK_ERR_INVALID_ARGUMENT, "invalid argument");
    return sendResult(e->engine.sendMidiToSource(sourceId, data, size), "no such MIDI source");
}

BRACK_API int BRACK_CALL brack_send_midi_to_source_at(brack_engine* e, const char* sourceId, uint64_t time,
                                                      const uint8_t* data, uint32_t size) {
    if (!e || !sourceId || !data) return failLiteral(BRACK_ERR_INVALID_ARGUMENT, "invalid argument");
    return sendResult(e->engine.sendMidiToSource(sourceId, data, size, time), "no such MIDI source");
}

BRACK_API int64_t BRACK_CALL brack_now_ns(void) { return brack::Engine::nowNs(); }

BRACK_API int BRACK_CALL brack_send_midi_at_time(brack_engine* e, const char* pluginId, uint32_t notePort,
                                                 int64_t time, const uint8_t* data, uint32_t size) {
    if (!e || !pluginId || !data || notePort > UINT16_MAX || time <= 0)
        return failLiteral(BRACK_ERR_INVALID_ARGUMENT, "invalid argument");
    return sendResult(e->engine.sendMidiToPluginAtTime(pluginId, (uint16_t)notePort, data, size, time), "no such plugin");
}

BRACK_API int BRACK_CALL brack_send_midi_to_source_at_time(brack_engine* e, const char* sourceId, int64_t time,
                                                           const uint8_t* data, uint32_t size) {
    if (!e || !sourceId || !data || time <= 0) return failLiteral(BRACK_ERR_INVALID_ARGUMENT, "invalid argument");
    return sendResult(e->engine.sendMidiToSourceAtTime(sourceId, data, size, time), "no such MIDI source");
}

BRACK_API int BRACK_CALL brack_load_session(brack_engine* e, const char* path) {
    return guarded(e, [&] {
        std::string err;
        return e->engine.loadSessionFile(str(path), err) ? ok() : failed(err);
    });
}

BRACK_API int BRACK_CALL brack_save_session(brack_engine* e, const char* path) {
    return guarded(e, [&] {
        std::string err;
        return e->engine.saveSessionFile(str(path), err) ? ok() : failed(err);
    });
}

BRACK_API int BRACK_CALL brack_load_session_json(brack_engine* e, const char* json) {
    return guarded(e, [&] {
        std::string err;
        return e->engine.loadSessionJson(str(json), err) ? ok() : failed(err);
    });
}

BRACK_API int BRACK_CALL brack_save_session_json(brack_engine* e, char* buf, size_t size, size_t* needed) {
    return guarded(e, [&] { return writeText(e->engine.saveSessionJson(), buf, size, needed); });
}

BRACK_API int BRACK_CALL brack_clear(brack_engine* e) {
    return guarded(e, [&] {
        e->engine.clear();
        return ok();
    });
}

BRACK_API uint64_t BRACK_CALL brack_change_count(brack_engine* e) { return e ? e->engine.changeCount() : 0; }

BRACK_API int BRACK_CALL brack_poll_event(brack_engine* e, brack_event* given) {
    return guarded(e, [&] {
        if (!given || given->struct_size < kFirstEventSize) return fail(BRACK_ERR_INVALID_ARGUMENT, "invalid event");
        brack::EngineEvent ev;
        if (!e->engine.pollEvent(ev)) {
            ok();
            return 0;
        }
        brack_event whole{};
        whole.struct_size = given->struct_size;
        brack_event* out = &whole;
        using T = brack::EngineEvent::Type;
        switch (ev.type) {
            case T::Changed: out->type = BRACK_EVENT_CHANGED; break;
            case T::PluginCrashed: out->type = BRACK_EVENT_PLUGIN_CRASHED; break;
            case T::EditorClosed: out->type = BRACK_EVENT_EDITOR_CLOSED; break;
            case T::EditorResized: out->type = BRACK_EVENT_EDITOR_RESIZED; break;
            case T::DeviceStalled: out->type = BRACK_EVENT_DEVICE_STALLED; break;
            case T::DeviceResumed: out->type = BRACK_EVENT_DEVICE_RESUMED; break;
            case T::MidiSourceLost: out->type = BRACK_EVENT_MIDI_SOURCE_LOST; break;
            case T::MidiSourceReconnected: out->type = BRACK_EVENT_MIDI_SOURCE_RECONNECTED; break;
        }
        copyText(ev.id, out->id, sizeof out->id);
        copyText(ev.message, out->message, sizeof out->message);
        out->width = ev.width;
        out->height = ev.height;
        std::memcpy(given, &whole, sharedPart(whole));
        ok();
        return 1;
    });
}

BRACK_API int BRACK_CALL brack_set_event_notify(brack_engine* e, brack_event_notify_fn fn, void* user) {
    return guarded(e, [&] {
        if (fn) e->engine.setEventNotify([fn, user] { fn(user); });
        else e->engine.setEventNotify(nullptr);
        return ok();
    });
}

BRACK_API int BRACK_CALL brack_get_status(brack_engine* e, char* buf, size_t size, size_t* needed) {
    return guarded(e, [&] { return writeText(brack::snapshotToJson(e->engine.snapshot()), buf, size, needed); });
}

BRACK_API int BRACK_CALL brack_get_status_cached(brack_engine* e, char* buf, size_t size, size_t* needed) {
    return guarded(e, [&] { return writeText(brack::snapshotToJson(e->engine.cachedSnapshot()), buf, size, needed); });
}

BRACK_API int BRACK_CALL brack_list_audio_devices(char* buf, size_t size, size_t* needed) {
    try {
        return writeText(brack::audioDevicesToJson(brack::listAudioOutputDevices()), buf, size, needed);
    } catch (const std::exception& ex) {
        return failed(ex.what());
    }
}

BRACK_API int BRACK_CALL brack_list_midi_inputs(char* buf, size_t size, size_t* needed) {
    try {
        return writeText(brack::midiInputsToJson(brack::listHardwareMidiInputs()), buf, size, needed);
    } catch (const std::exception& ex) {
        return failed(ex.what());
    }
}

BRACK_API int BRACK_CALL brack_scan_plugins(const char* extraDirs, const char* cachePath, uint32_t flags, char* buf,
                                            size_t size, size_t* needed) {
    try {
        const bool standardLocations = (flags & BRACK_SCAN_EXTRA_DIRS_ONLY) == 0;
        auto found = brack::Engine::scanPlugins(splitDirs(extraDirs), str(cachePath), standardLocations);
        return writeText(brack::pluginsToJson(found), buf, size, needed);
    } catch (const std::exception& ex) {
        return failed(ex.what());
    }
}

BRACK_API int BRACK_CALL brack_virtual_midi_available(void) {
    std::string reason;
    if (brack::virtualMidiAvailable(reason)) {
        ok();
        return 1;
    }
    failed(reason);
    return 0;
}

BRACK_API int BRACK_CALL brack_virtual_midi_removal_hangs(void) {
    std::string reason;
    ok();
    return brack::virtualMidiAvailable(reason) && brack::virtualMidiRemovalHangsService() ? 1 : 0;
}

}  // extern "C"
