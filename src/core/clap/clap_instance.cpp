#include "clap/clap_instance.h"

#include <algorithm>
#include <cstring>

#include "host_thread.h"
#include "util/common.h"

namespace brack {

namespace {
uint32_t emptyOutSize(const clap_input_events_t*) { return 0; }
const clap_event_header_t* emptyOutGet(const clap_input_events_t*, uint32_t) { return nullptr; }
const clap_input_events_t kEmptyIn{nullptr, &emptyOutSize, &emptyOutGet};

ClapInstance* self(const clap_host_t* h) { return static_cast<ClapInstance*>(h->host_data); }

// Output events from process() and params.flush(). brack has no use for them except that a
// parameter the user changed in the editor shows up here, and plugins (JUCE-based ones among
// them) may report it no other way. Called on the audio thread: atomics only.
bool outTryPush(const clap_output_events_t* list, const clap_event_header_t* e) {
    if (e && e->space_id == CLAP_CORE_EVENT_SPACE_ID &&
        (e->type == CLAP_EVENT_PARAM_VALUE || e->type == CLAP_EVENT_PARAM_GESTURE_END))
        static_cast<ClapInstance*>(list->ctx)->onStateDirty();
    return true;
}

// ---- host extensions (trampolines) ----
const clap_host_log_t kHostLog{
    [](const clap_host_t* h, clap_log_severity s, const char* m) { self(h)->onLog(s, m); }};

const clap_host_thread_check_t kHostThreadCheck{
    [](const clap_host_t* h) { return self(h)->isMainThread(); },
    [](const clap_host_t*) { return currentThreadIsAudio(); }};

const clap_host_params_t kHostParams{
    [](const clap_host_t* h, clap_param_rescan_flags flags) {
        if (flags & (CLAP_PARAM_RESCAN_VALUES | CLAP_PARAM_RESCAN_ALL)) self(h)->onStateDirty();
    },
    [](const clap_host_t*, clap_id, clap_param_clear_flags) {},
    [](const clap_host_t* h) { self(h)->onParamsRequestFlush(); }};

const clap_host_state_t kHostState{[](const clap_host_t* h) { self(h)->onStateDirty(); }};

// Latency is re-read after every activation, which is the only time it may change.
const clap_host_latency_t kHostLatency{[](const clap_host_t*) {}};

const clap_host_audio_ports_t kHostAudioPorts{
    [](const clap_host_t*, uint32_t) { return true; },
    [](const clap_host_t* h, uint32_t) { self(h)->onRescanPorts(); }};

const clap_host_note_ports_t kHostNotePorts{
    [](const clap_host_t*) -> uint32_t { return CLAP_NOTE_DIALECT_MIDI | CLAP_NOTE_DIALECT_CLAP; },
    [](const clap_host_t* h, uint32_t) { self(h)->onRescanPorts(); }};

const clap_host_gui_t kHostGui{
    [](const clap_host_t*) {},
    [](const clap_host_t* h, uint32_t w, uint32_t hh) { return self(h)->onGuiRequestResize(w, hh); },
    [](const clap_host_t* h) { return self(h)->onGuiRequestShow(); },
    [](const clap_host_t* h) { return self(h)->onGuiRequestHide(); },
    [](const clap_host_t* h, bool d) { self(h)->onGuiClosed(d); }};

const clap_host_timer_support_t kHostTimer{
    [](const clap_host_t* h, uint32_t ms, clap_id* id) { return self(h)->onRegisterTimer(ms, id); },
    [](const clap_host_t* h, clap_id id) { return self(h)->onUnregisterTimer(id); }};

#ifndef _WIN32
// The plugin's own file descriptors (its X11 connection, say), watched on the host thread. CLAP's
// flags are HostThread's FdEvents.
static_assert(uint32_t(CLAP_POSIX_FD_READ) == HostThread::FdRead && uint32_t(CLAP_POSIX_FD_WRITE) == HostThread::FdWrite &&
              uint32_t(CLAP_POSIX_FD_ERROR) == HostThread::FdError);
const clap_host_posix_fd_support_t kHostFd{
    [](const clap_host_t* h, int fd, clap_posix_fd_flags_t flags) { return self(h)->onRegisterFd(fd, flags); },
    [](const clap_host_t* h, int fd, clap_posix_fd_flags_t flags) { return self(h)->onModifyFd(fd, flags); },
    [](const clap_host_t* h, int fd) { return self(h)->onUnregisterFd(fd); }};
#endif

const void* hostGetExtension(const clap_host_t*, const char* id) {
    if (!std::strcmp(id, CLAP_EXT_LOG)) return &kHostLog;
    if (!std::strcmp(id, CLAP_EXT_THREAD_CHECK)) return &kHostThreadCheck;
    if (!std::strcmp(id, CLAP_EXT_PARAMS)) return &kHostParams;
    if (!std::strcmp(id, CLAP_EXT_STATE)) return &kHostState;
    if (!std::strcmp(id, CLAP_EXT_LATENCY)) return &kHostLatency;
    if (!std::strcmp(id, CLAP_EXT_AUDIO_PORTS)) return &kHostAudioPorts;
    if (!std::strcmp(id, CLAP_EXT_NOTE_PORTS)) return &kHostNotePorts;
    if (!std::strcmp(id, CLAP_EXT_GUI)) return &kHostGui;
    if (!std::strcmp(id, CLAP_EXT_TIMER_SUPPORT)) return &kHostTimer;
#ifndef _WIN32
    if (!std::strcmp(id, CLAP_EXT_POSIX_FD_SUPPORT)) return &kHostFd;
#endif
    return nullptr;
}
}  // namespace

// ---------------------------------------------------------------------------
// ClapInstance

ClapInstance::ClapInstance(HostThread& host, PluginHostListener& listener, std::shared_ptr<ClapModule> module,
                           PluginDescription desc, std::string displayName)
    : PluginInstance(host, listener, std::move(desc), std::move(displayName)), module_(std::move(module)) {
    clapHost_.clap_version = CLAP_VERSION;
    clapHost_.host_data = this;
    clapHost_.name = "Brack";
    clapHost_.vendor = "Brack";
    clapHost_.url = "";
    clapHost_.version = BRACK_VERSION;
    clapHost_.get_extension = &hostGetExtension;
    clapHost_.request_restart = [](const clap_host_t* h) { self(h)->onRequestRestart(); };
    clapHost_.request_process = [](const clap_host_t*) {};  // brack always processes while running
    clapHost_.request_callback = [](const clap_host_t* h) { self(h)->onRequestCallback(); };
    outEvents_.ctx = this;
    outEvents_.try_push = &outTryPush;
}

ClapInstance::~ClapInstance() {
    shutdown();
    for (auto& [cid, hid] : timers_) host_.removeTimer(hid);
    timers_.clear();
#ifndef _WIN32
    for (auto& [fd, id] : fds_) host_.unwatchFd(id);
    fds_.clear();
#endif
    if (plugin_) callPlugin("destroy", [&] { plugin_->destroy(plugin_); });
    if (crashed()) keepForever(module_);  // not deinitialised or unloaded under a broken plugin
}

bool ClapInstance::initPlugin(std::string& error) {
    plugin_ = module_->factory()->create_plugin(module_->factory(), &clapHost_, desc_.id.c_str());
    if (!plugin_) {
        error = "create_plugin failed for " + desc_.id;
        return false;
    }
    if (!plugin_->init(plugin_)) {
        error = "plugin init failed for " + desc_.id;
        plugin_->destroy(plugin_);
        plugin_ = nullptr;
        return false;
    }
    auto ext = [&](const char* id) { return plugin_->get_extension(plugin_, id); };
    extAudioPorts_ = static_cast<const clap_plugin_audio_ports_t*>(ext(CLAP_EXT_AUDIO_PORTS));
    extNotePorts_ = static_cast<const clap_plugin_note_ports_t*>(ext(CLAP_EXT_NOTE_PORTS));
    extGui_ = static_cast<const clap_plugin_gui_t*>(ext(CLAP_EXT_GUI));
    extState_ = static_cast<const clap_plugin_state_t*>(ext(CLAP_EXT_STATE));
    extParams_ = static_cast<const clap_plugin_params_t*>(ext(CLAP_EXT_PARAMS));
    extTimer_ = static_cast<const clap_plugin_timer_support_t*>(ext(CLAP_EXT_TIMER_SUPPORT));
    extLatency_ = static_cast<const clap_plugin_latency_t*>(ext(CLAP_EXT_LATENCY));
    extFd_ = static_cast<const clap_plugin_posix_fd_support_t*>(ext(CLAP_EXT_POSIX_FD_SUPPORT));
    queryPorts();
    return true;
}

void ClapInstance::queryPorts() {
    notePorts_.clear();
    audioIns_.clear();
    audioOuts_.clear();
    if (extNotePorts_) {
        uint32_t n = extNotePorts_->count(plugin_, true);
        for (uint32_t i = 0; i < n; ++i) {
            clap_note_port_info_t info{};
            if (!extNotePorts_->get(plugin_, i, true, &info)) continue;
            notePorts_.push_back({info.id, info.name, info.supported_dialects});
        }
    }
    portAcceptsMidi_.clear();
    for (auto& p : notePorts_) portAcceptsMidi_.push_back((p.dialects & CLAP_NOTE_DIALECT_MIDI) != 0);

    auto readAudio = [&](bool input, std::vector<AudioPortInfo>& out) {
        if (!extAudioPorts_) return;
        uint32_t n = extAudioPorts_->count(plugin_, input);
        for (uint32_t i = 0; i < n; ++i) {
            clap_audio_port_info_t info{};
            if (!extAudioPorts_->get(plugin_, i, input, &info)) continue;
            out.push_back({info.id, info.name, info.channel_count, (info.flags & CLAP_AUDIO_PORT_IS_MAIN) != 0});
        }
    };
    readAudio(true, audioIns_);
    readAudio(false, audioOuts_);
}

bool ClapInstance::activatePlugin(double sampleRate, uint32_t maxFrames, std::string& error) {
    auto build = [](const std::vector<AudioPortInfo>& ports, std::vector<std::vector<float*>>& ptrs,
                    std::vector<clap_audio_buffer_t>& bufs) {
        bufs.clear();
        for (size_t i = 0; i < ports.size(); ++i) {
            clap_audio_buffer_t b{};
            b.data32 = ptrs[i].empty() ? nullptr : ptrs[i].data();
            b.channel_count = ports[i].channels;
            bufs.push_back(b);
        }
    };
    build(audioIns_, inPtrs_, inBufs_);
    build(audioOuts_, outPtrs_, outBufs_);
    for (auto& b : inBufs_) b.constant_mask = ~0ull;  // silent inputs
    if (!plugin_->activate(plugin_, sampleRate, 1, maxFrames)) {
        error = "activate failed (" + desc_.name + ")";
        return false;
    }
    return true;
}

void ClapInstance::deactivatePlugin() { plugin_->deactivate(plugin_); }

bool ClapInstance::startProcessing() { return plugin_->start_processing(plugin_); }

void ClapInstance::stopProcessing() { plugin_->stop_processing(plugin_); }

uint32_t ClapInstance::pluginLatency() const { return extLatency_ ? extLatency_->get(plugin_) : 0; }

void ClapInstance::idlePlugin() {
    if (callbackRequested_.exchange(false)) plugin_->on_main_thread(plugin_);
    if (flushRequested_ && extParams_ && (!isActive() || stopProcessingAcknowledged())) {
        flushRequested_ = false;
        extParams_->flush(plugin_, &kEmptyIn, &outEvents_);
    }
}

void ClapInstance::appendMidi(InputEventList& list, uint16_t port, const uint8_t* data, uint32_t size,
                              uint32_t time) const {
    if (port >= portAcceptsMidi_.size()) return;
    if (portAcceptsMidi_[port]) {
        list.pushMidi(port, data, size, time);
        return;
    }
    // The port only takes CLAP note events: note on/off become those, other messages are dropped.
    if (size < 3) return;
    uint8_t type = data[0] & 0xF0, ch = data[0] & 0x0F;
    if (type == 0x90 && data[2] > 0) list.pushNote(CLAP_EVENT_NOTE_ON, port, ch, data[1], data[2] / 127.0, time);
    else if (type == 0x80 || type == 0x90)
        list.pushNote(CLAP_EVENT_NOTE_OFF, port, ch, data[1], data[2] / 127.0, time);
}

bool ClapInstance::processPlugin(uint32_t frames, uint64_t steadyTime, const InputEventList& in) {
    clap_process_t p{};
    p.steady_time = (int64_t)steadyTime;
    p.frames_count = frames;
    p.transport = nullptr;
    p.audio_inputs = inBufs_.empty() ? nullptr : inBufs_.data();
    p.audio_inputs_count = (uint32_t)inBufs_.size();
    p.audio_outputs = outBufs_.empty() ? nullptr : outBufs_.data();
    p.audio_outputs_count = (uint32_t)outBufs_.size();
    p.in_events = in.get();
    p.out_events = &outEvents_;
    for (auto& b : outBufs_) b.constant_mask = 0;
    if (plugin_->process(plugin_, &p) == CLAP_PROCESS_ERROR) return false;
    if (flushRequested_) flushRequested_ = false;  // in/out events were exchanged by process()
    return true;
}

void ClapInstance::onLog(clap_log_severity s, const char* msg) {
    LogLevel lvl = LogLevel::Info;
    switch (s) {
        case CLAP_LOG_DEBUG: lvl = LogLevel::Debug; break;
        case CLAP_LOG_INFO: lvl = LogLevel::Info; break;
        case CLAP_LOG_WARNING: lvl = LogLevel::Warning; break;
        default: lvl = LogLevel::Error; break;
    }
    log(lvl, "[" + displayName() + "] " + (msg ? msg : ""));
}

// ---- state ----

bool ClapInstance::saveStatePlugin(std::vector<uint8_t>& out) {
    out.clear();
    if (!extState_) return false;
    clap_ostream_t os{};
    os.ctx = &out;
    os.write = [](const clap_ostream_t* s, const void* buf, uint64_t size) -> int64_t {
        auto* v = static_cast<std::vector<uint8_t>*>(s->ctx);
        const auto* b = static_cast<const uint8_t*>(buf);
        v->insert(v->end(), b, b + size);
        return (int64_t)size;
    };
    return extState_->save(plugin_, &os);
}

bool ClapInstance::loadStatePlugin(const std::vector<uint8_t>& in) {
    if (!extState_) return false;
    struct Reader {
        const std::vector<uint8_t>* data;
        size_t pos;
    } r{&in, 0};
    clap_istream_t is{};
    is.ctx = &r;
    is.read = [](const clap_istream_t* s, void* buf, uint64_t size) -> int64_t {
        auto* rd = static_cast<Reader*>(s->ctx);
        size_t n = (size_t)std::min<uint64_t>(size, rd->data->size() - rd->pos);
        std::memcpy(buf, rd->data->data() + rd->pos, n);
        rd->pos += n;
        return (int64_t)n;
    };
    return extState_->load(plugin_, &is);
}

// ---- timers ----

bool ClapInstance::onRegisterTimer(uint32_t periodMs, clap_id* id) {
    if (!host_.isCurrent() || !extTimer_) return false;
    clap_id cid = nextTimerId_++;
    uint32_t hid = host_.addTimer(std::max<uint32_t>(periodMs, 10), [this, cid] {
        if (timers_.count(cid)) callPlugin("timer", [&] { extTimer_->on_timer(plugin_, cid); });
    });
    timers_[cid] = hid;
    *id = cid;
    return true;
}

bool ClapInstance::onUnregisterTimer(clap_id id) {
    auto it = timers_.find(id);
    if (it == timers_.end()) return false;
    host_.removeTimer(it->second);
    timers_.erase(it);
    return true;
}

#ifndef _WIN32
// ---- file descriptors ----

bool ClapInstance::onRegisterFd(int fd, clap_posix_fd_flags_t flags) {
    if (!host_.isCurrent() || !extFd_ || fds_.count(fd)) return false;
    fds_[fd] = host_.watchFd(fd, flags, [this, fd](uint32_t events) {
        if (fds_.count(fd)) callPlugin("fd", [&] { extFd_->on_fd(plugin_, fd, events); });
    });
    return true;
}

bool ClapInstance::onModifyFd(int fd, clap_posix_fd_flags_t flags) {
    auto it = fds_.find(fd);
    if (it == fds_.end()) return false;
    host_.changeFd(it->second, flags);
    return true;
}

bool ClapInstance::onUnregisterFd(int fd) {
    auto it = fds_.find(fd);
    if (it == fds_.end()) return false;
    host_.unwatchFd(it->second);
    fds_.erase(it);
    return true;
}
#endif

// ---- GUI ----

bool ClapInstance::pluginHasGui() const { return extGui_ && clapEditorSupported(plugin_, extGui_); }

std::unique_ptr<PluginEditor> ClapInstance::createEditor(std::string& error) {
    return createClapEditor(*this, plugin_, extGui_, displayName(), error);
}

bool ClapInstance::onGuiRequestResize(uint32_t w, uint32_t h) {
    auto* e = static_cast<ClapEditor*>(editor());
    return e && e->requestResize(w, h);
}
bool ClapInstance::onGuiRequestShow() {
    if (!editor()) return false;
    editor()->show();
    return true;
}
bool ClapInstance::onGuiRequestHide() {
    if (!editor()) return false;
    editor()->hide();
    return true;
}
// The plugin's floating window was closed by the user (or the plugin).
void ClapInstance::onGuiClosed(bool) { editorClosedByUser(false); }

}  // namespace brack
