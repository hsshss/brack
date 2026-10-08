// VST 2.4 hosting. The only translation unit that includes vst.h (it defines two
// global tables).
#include "vst2/vst2_plugin.h"

#include <vst.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>

#include "host_thread.h"
#include "plugin/host_window.h"
#include "plugin/library.h"
#include "plugin/plugin_files.h"
#include "util/common.h"
#include "vst2/vst2_abi.h"

namespace brack {

std::string vst2IdToString(int32_t id) {
    const uint32_t u = (uint32_t)id;
    char c[5] = {char(u >> 24), char(u >> 16), char(u >> 8), char(u), 0};
    bool printable = true;
    for (int i = 0; i < 4; ++i) printable = printable && c[i] >= 0x20 && c[i] < 0x7F;
    if (printable) return c;
    char buf[16];
    std::snprintf(buf, sizeof buf, "0x%08X", u);
    return buf;
}

bool vst2IdFromString(const std::string& s, int32_t& id) {
    if (s.size() == 4) {
        id = (int32_t)VST_FOURCC((uint8_t)s[0], (uint8_t)s[1], (uint8_t)s[2], (uint8_t)s[3]);
        return true;
    }
    if (s.size() > 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        char* end = nullptr;
        unsigned long v = std::strtoul(s.c_str() + 2, &end, 16);
        if (*end) return false;
        id = (int32_t)(uint32_t)v;
        return true;
    }
    return false;
}

namespace {

using MainFn = Vst2MainFn;

constexpr size_t kMaxMidiEvents = 4096;
constexpr size_t kMaxSysexEvents = 256;
constexpr size_t kStringBuffer = 256;  // more than any VST2 string; plugins overrun the official sizes

const char kStateMagic[4] = {'B', 'R', 'V', '2'};

struct Vst2Module {
    std::string path;
    void* library = nullptr;
    MainFn main = nullptr;
    ~Vst2Module() {
        if (library) closeLibrary(library);
    }
};

std::mutex g_moduleMutex;
std::map<std::string, std::weak_ptr<Vst2Module>> g_modules;

std::shared_ptr<Vst2Module> loadModule(const std::string& pathUtf8, std::string& error) {
    std::lock_guard lock(g_moduleMutex);
    std::string key = pathToUtf8(std::filesystem::absolute(pathFromUtf8(pathUtf8)).lexically_normal());
    if (auto it = g_modules.find(key); it != g_modules.end())
        if (auto m = it->second.lock()) return m;
    auto m = std::make_shared<Vst2Module>();
    m->path = key;
    m->library = openLibrary(pathToUtf8(pluginBinaryPath(PluginFormat::Vst2, pathFromUtf8(key))), error);
    if (!m->library) return nullptr;
    m->main = reinterpret_cast<MainFn>(librarySymbol(m->library, "VSTPluginMain"));
    if (!m->main) m->main = reinterpret_cast<MainFn>(librarySymbol(m->library, "main"));
    if (!m->main) {
        error = "not a VST2 plugin (no VSTPluginMain)";
        return nullptr;
    }
    g_modules[key] = m;
    return m;
}

class Vst2Instance;

// What the host callback answers before an effect knows its instance: during
// VSTPluginMain (a shell asks which plugin to create) and while scanning.
struct Loading {
    Vst2Instance* instance = nullptr;
    int32_t shellId = 0;
};
thread_local Loading t_loading;

intptr_t VST_FUNCTION_INTERFACE hostCallback(vst_effect_t* effect, int32_t opcode, int32_t index, intptr_t value,
                                             void* ptr, float opt);

std::string effectString(vst_effect_t* e, int32_t opcode) {
    char buf[kStringBuffer] = {};
    e->control(e, opcode, 0, 0, buf, 0);
    buf[kStringBuffer - 1] = 0;
    return buf;
}

bool isShell(vst_effect_t* e) {
    return e->control(e, VST_EFFECT_OPCODE_CATEGORY, 0, 0, nullptr, 0) == VST_EFFECT_CATEGORY_CONTAINER;
}

// The plugins a shell contains, by id and name, in the shell's order.
std::vector<std::pair<int32_t, std::string>> shellPlugins(vst_effect_t* e) {
    std::vector<std::pair<int32_t, std::string>> out;
    for (int guard = 0; guard < 10000; ++guard) {
        char buf[kStringBuffer] = {};
        auto id = (int32_t)e->control(e, VST_EFFECT_OPCODE_CONTAINER_NEXT_EFFECT_ID, 0, 0, buf, 0);
        if (id == 0) break;
        buf[kStringBuffer - 1] = 0;
        out.emplace_back(id, buf);
    }
    return out;
}

vst_effect_t* instantiate(MainFn main, Vst2Instance* instance, int32_t shellId) {
    t_loading = {instance, shellId};
    vst_effect_t* e = main(&hostCallback);
    t_loading = {};
    if (e && e->magic_number != VST_MAGICNUMBER) return nullptr;
    return e;
}

// Description fields readable from an open effect.
void fillDescription(vst_effect_t* e, PluginDescription& d) {
    d.format = PluginFormat::Vst2;
    d.id = vst2IdToString(e->unique_id);
    std::string name = effectString(e, VST_EFFECT_OPCODE_EFFECT_NAME);
    if (name.empty()) name = effectString(e, VST_EFFECT_OPCODE_PRODUCT_NAME);
    if (!name.empty()) d.name = name;
    d.vendor = effectString(e, VST_EFFECT_OPCODE_VENDOR_NAME);
    intptr_t v = e->control(e, VST_EFFECT_OPCODE_VENDOR_VERSION, 0, 0, nullptr, 0);
    if (v <= 0) v = e->version;
    d.version = v > 0 ? std::to_string(v) : "";
    d.instrument = (e->flags & VST_EFFECT_FLAG_INSTRUMENT) ||
                   e->control(e, VST_EFFECT_OPCODE_CATEGORY, 0, 0, nullptr, 0) == VST_EFFECT_CATEGORY_INSTRUMENT;
    d.features.clear();
    if (d.instrument) d.features.emplace_back(CLAP_PLUGIN_FEATURE_INSTRUMENT);
}

class Vst2Editor final : public PluginEditor {
public:
    explicit Vst2Editor(Vst2Instance& owner) : owner_(owner) {}
    ~Vst2Editor() override;
    bool open(const std::string& title, std::string& error);
    bool resize(int w, int h) {
        if (!window_ || w <= 0 || h <= 0) return false;
        window_->setClientSize((uint32_t)w, (uint32_t)h);
        return true;
    }
    void show() override { window_->show(); }
    void hide() override { window_->hide(); }
    void setTitle(const std::string& title) override { window_->setTitle(title); }
    void abandon() override {
        if (window_) window_->abandon();
        (void)window_.release();
        opened_ = false;
    }

private:
    void fitToPlugin();
    Vst2Instance& owner_;
    std::unique_ptr<HostWindow> window_;
    bool opened_ = false;
};

class Vst2Instance final : public PluginInstance {
public:
    Vst2Instance(HostThread& host, PluginHostListener& listener, std::shared_ptr<Vst2Module> module,
                 PluginDescription desc, std::string displayName)
        : PluginInstance(host, listener, std::move(desc), std::move(displayName)), module_(std::move(module)) {
        midiEvents_.resize(kMaxMidiEvents);
        sysexEvents_.resize(kMaxSysexEvents);
    }

    ~Vst2Instance() override {
        shutdown();
        if (effect_) callPlugin("destroy", [&] { dispatch(VST_EFFECT_OPCODE_DESTROY); });
        if (crashed()) keepForever(module_);  // not unloaded under a broken plugin
    }

    intptr_t dispatch(int32_t op, int32_t index = 0, intptr_t value = 0, void* ptr = nullptr, float opt = 0) {
        return effect_->control(effect_, op, index, value, ptr, opt);
    }

    bool initPlugin(std::string& error) override {
        int32_t wanted = 0;
        if (!desc_.id.empty() && !vst2IdFromString(desc_.id, wanted)) {
            error = "invalid VST2 plugin id: " + desc_.id;
            return false;
        }
        effect_ = instantiate(module_->main, this, wanted);
        if (!effect_) {
            error = "VST2 plugin failed to instantiate";
            return false;
        }
        effect_->host_internal = this;
        dispatch(VST_EFFECT_OPCODE_CREATE);
        std::string shellName;
        if (isShell(effect_)) {
            // A shell (several plugins behind one entry point) that ignored or was not given
            // an id: pick the wanted, or else the first, and instantiate that.
            auto subs = shellPlugins(effect_);
            auto it = std::find_if(subs.begin(), subs.end(), [&](auto& s) { return wanted == 0 || s.first == wanted; });
            dispatch(VST_EFFECT_OPCODE_DESTROY);
            effect_ = nullptr;
            if (it == subs.end()) {
                error = subs.empty() ? "VST2 shell contains no plugins" : "plugin " + desc_.id + " not found in the shell";
                return false;
            }
            wanted = it->first;
            shellName = it->second;
            effect_ = instantiate(module_->main, this, wanted);
            if (effect_) {
                effect_->host_internal = this;
                dispatch(VST_EFFECT_OPCODE_CREATE);
            }
            if (!effect_ || isShell(effect_)) {
                if (effect_) dispatch(VST_EFFECT_OPCODE_DESTROY);
                effect_ = nullptr;
                error = "VST2 shell failed to create " + vst2IdToString(wanted);
                return false;
            }
        } else if (wanted != 0 && effect_->unique_id != wanted) {
            logWarn(module_->path + ": plugin id is now " + vst2IdToString(effect_->unique_id) + ", was " + desc_.id);
        }
        desc_.path = module_->path;
        if (desc_.name.empty()) desc_.name = pathToUtf8(pathFromUtf8(module_->path).stem());
        fillDescription(effect_, desc_);
        if (!shellName.empty()) desc_.name = shellName;
        directory_ = pathToUtf8(pathFromUtf8(module_->path).parent_path());
        queryPorts();
        return true;
    }

    void idlePlugin() override {
        if (editor()) dispatch(VST_EFFECT_OPCODE_EDITOR_KEEP_ALIVE);
    }

    bool saveStatePlugin(std::vector<uint8_t>& out) override {
        out.assign(kStateMagic, kStateMagic + 4);
        if (effect_->flags & VST_EFFECT_FLAG_CHUNKS) {
            void* data = nullptr;
            intptr_t n = dispatch(VST_EFFECT_OPCODE_GET_CHUNK_DATA, 0 /* bank */, 0, &data);
            if (n > 0 && data) {
                out.push_back('C');
                out.insert(out.end(), static_cast<uint8_t*>(data), static_cast<uint8_t*>(data) + n);
                return true;
            }
        }
        // No chunk: the current program and every parameter.
        out.push_back('P');
        auto put32 = [&](uint32_t v) {
            uint8_t b[4];
            std::memcpy(b, &v, 4);
            out.insert(out.end(), b, b + 4);
        };
        put32((uint32_t)dispatch(VST_EFFECT_OPCODE_PROGRAM_GET));
        put32((uint32_t)std::max(effect_->num_params, 0));
        for (int32_t i = 0; i < effect_->num_params; ++i) {
            float v = effect_->get_parameter(effect_, (uint32_t)i);
            uint32_t bits;
            std::memcpy(&bits, &v, 4);
            put32(bits);
        }
        return true;
    }

    bool loadStatePlugin(const std::vector<uint8_t>& in) override {
        if (in.size() < 5 || std::memcmp(in.data(), kStateMagic, 4) != 0) return false;
        const uint8_t* p = in.data() + 5;
        const size_t n = in.size() - 5;
        if (in[4] == 'C') {
            std::vector<uint8_t> chunk(p, p + n);  // writable, and alive for the whole call
            dispatch(VST_EFFECT_OPCODE_SET_CHUNK_DATA, 0 /* bank */, (intptr_t)chunk.size(), chunk.data());
            return true;
        }
        if (in[4] != 'P' || n < 8) return false;
        auto get32 = [&](size_t at) {
            uint32_t v;
            std::memcpy(&v, p + at, 4);
            return v;
        };
        const uint32_t program = get32(0), count = get32(4);
        if (n < 8 + (size_t)count * 4) return false;
        dispatch(VST_EFFECT_OPCODE_PROGRAM_SET, 0, (intptr_t)program);
        for (uint32_t i = 0; i < count && (int32_t)i < effect_->num_params; ++i) {
            uint32_t bits = get32(8 + i * 4);
            float v;
            std::memcpy(&v, &bits, 4);
            effect_->set_parameter(effect_, i, v);
        }
        return true;
    }

    bool pluginHasGui() const override { return effect_ && (effect_->flags & VST_EFFECT_FLAG_EDITOR); }
    uint32_t pluginLatency() const override { return (uint32_t)std::max(effect_->delay, 0); }

    // ---- host callback ----
    const std::string& directory() const { return directory_; }
    double sampleRate() const { return sampleRate_ > 0 ? sampleRate_ : 44100.0; }
    uint32_t blockSize() const { return maxFrames_ ? maxFrames_ : 512; }
    void onIoChanged() {
        if (host_.isCurrent()) portsChanged();
        else
            host_.post([this, alive = std::weak_ptr<int>(lifeToken_)] {
                if (alive.lock()) portsChanged();
            });
    }
    void onStateDirty() { stateChanged(); }
    bool onEditorResize(int w, int h) {
        auto* e = static_cast<Vst2Editor*>(editor());
        return host_.isCurrent() && e && e->resize(w, h);
    }

private:
    bool canDo(const char* what) { return dispatch(VST_EFFECT_OPCODE_SUPPORTS, 0, 0, const_cast<char*>(what)) > 0; }

    void queryPorts() override {
        notePorts_.clear();
        audioIns_.clear();
        audioOuts_.clear();
        if (desc_.instrument || canDo("receiveVstMidiEvent") || canDo("receiveVstEvents"))
            notePorts_.push_back({0, "MIDI In", CLAP_NOTE_DIALECT_MIDI});
        // VST2 has plain channel lists; present them as stereo pairs.
        auto group = [](int32_t channels, const char* prefix, std::vector<AudioPortInfo>& out) {
            for (int32_t c = 0; c < std::min(channels, 256); c += 2) {
                const uint32_t n = (uint32_t)std::min(2, channels - c);
                std::string name = std::string(prefix) + " " + std::to_string(c + 1) +
                                   (n == 2 ? "/" + std::to_string(c + 2) : "");
                out.push_back({(clap_id)out.size(), name, n, out.empty()});
            }
        };
        group(effect_->num_inputs, "In", audioIns_);
        group(effect_->num_outputs, "Out", audioOuts_);
    }

    bool activatePlugin(double sampleRate, uint32_t maxFrames, std::string&) override {
        inFlat_.clear();
        outFlat_.clear();
        for (auto& p : inPtrs_) inFlat_.insert(inFlat_.end(), p.begin(), p.end());
        for (auto& p : outPtrs_) outFlat_.insert(outFlat_.end(), p.begin(), p.end());
        dispatch(VST_EFFECT_OPCODE_SET_SAMPLE_RATE, 0, 0, nullptr, (float)sampleRate);
        dispatch(VST_EFFECT_OPCODE_SET_BLOCK_SIZE, 0, (intptr_t)maxFrames);
        dispatch(VST_EFFECT_OPCODE_SUSPEND_RESUME, 0, 1);
        return true;
    }

    void deactivatePlugin() override { dispatch(VST_EFFECT_OPCODE_SUSPEND_RESUME, 0, 0); }

    bool startProcessing() override {
        dispatch(VST_EFFECT_OPCODE_PROCESS_BEGIN);
        return true;
    }

    void stopProcessing() override { dispatch(VST_EFFECT_OPCODE_PROCESS_END); }

    bool processPlugin(uint32_t frames, uint64_t, const InputEventList& in) override {
        size_t count = 0, midi = 0, sysex = 0;
        for (size_t i = 0; i < in.size(); ++i) {
            const clap_event_header_t* h = in.at(i);
            if (h->space_id != CLAP_CORE_EVENT_SPACE_ID) continue;
            if (h->type == CLAP_EVENT_MIDI && midi < midiEvents_.size()) {
                auto* m = reinterpret_cast<const clap_event_midi_t*>(h);
                vst_event_midi_t& e = midiEvents_[midi++];
                e = {};
                e.event.type = VST_EVENT_TYPE_MIDI;
                e.event.size = (int32_t)(sizeof(e) - 2 * sizeof(int32_t));
                e.event.offset = (int32_t)h->time;
                e.midi.is_real_time = 1;
                for (int b = 0; b < 3; ++b) e.midi.data[b] = (char)m->data[b];
                eventList_->events[count++] = &e.event;
            } else if (h->type == CLAP_EVENT_MIDI_SYSEX && sysex < sysexEvents_.size()) {
                auto* s = reinterpret_cast<const clap_event_midi_sysex_t*>(h);
                vst_event_midi_sysex_t& e = sysexEvents_[sysex++];
                e = {};
                e.event.type = VST_EVENT_TYPE_MIDI_SYSEX;
                e.event.size = (int32_t)(sizeof(e) - 2 * sizeof(int32_t));
                e.event.offset = (int32_t)h->time;
                e.sysex.size = (int32_t)s->size;
                e.sysex.data = reinterpret_cast<char*>(const_cast<uint8_t*>(s->buffer));
                eventList_->events[count++] = &e.event;
            }
        }
        if (count) {
            eventList_->count = (int32_t)count;
            dispatch(VST_EFFECT_OPCODE_EVENT, 0, 0, eventList_.get());
        }
        // Plugins may process in place or leave a silent output untouched: start from silence.
        for (auto& port : inPtrs_)
            for (float* c : port) std::memset(c, 0, frames * sizeof(float));
        for (float* c : outFlat_) std::memset(c, 0, frames * sizeof(float));
        if (effect_->flags & VST_EFFECT_FLAG_SUPPORTS_FLOAT)
            effect_->process_float(effect_, inFlat_.data(), outFlat_.data(), (int32_t)frames);
        else if (effect_->process)
            effect_->process(effect_, inFlat_.data(), outFlat_.data(), (int32_t)frames);  // accumulating (VST 1)
        return true;
    }

    std::unique_ptr<PluginEditor> createEditor(std::string& error) override {
        auto e = std::make_unique<Vst2Editor>(*this);
        if (!e->open(displayName(), error)) return nullptr;
        return e;
    }

    std::shared_ptr<Vst2Module> module_;
    vst_effect_t* effect_ = nullptr;
    std::string directory_;  // returned to the plugin; stays put while it lives
    std::vector<const float*> inFlat_;
    std::vector<float*> outFlat_;
    std::vector<vst_event_midi_t> midiEvents_;
    std::vector<vst_event_midi_sysex_t> sysexEvents_;
    std::unique_ptr<Vst2EventList<kMaxMidiEvents + kMaxSysexEvents>> eventList_ =
        std::make_unique<Vst2EventList<kMaxMidiEvents + kMaxSysexEvents>>();
};

Vst2Editor::~Vst2Editor() {
    if (opened_) owner_.dispatch(VST_EFFECT_OPCODE_EDITOR_CLOSE);
    window_.reset();
}

bool Vst2Editor::open(const std::string& title, std::string& error) {
    HostWindow::Callbacks cb;
    cb.closeRequested = [this] { owner_.editorClosedByUser(); };
    window_ = HostWindow::create(title, false, std::move(cb), owner_.editorPlacement(), error);
    if (!window_) return false;
    fitToPlugin();
    // The pointer is HostWindow::nativeHandle(). The return value is unreliable: plenty of
    // editors open fine and return 0.
    owner_.dispatch(VST_EFFECT_OPCODE_EDITOR_OPEN, 0, 0, window_->nativeHandle());
    opened_ = true;
    fitToPlugin();  // some editors only know their size once open
    window_->show();
    return true;
}

void Vst2Editor::fitToPlugin() {
    vst_rect_t* r = nullptr;
    if (owner_.dispatch(VST_EFFECT_OPCODE_EDITOR_GET_RECT, 0, 0, &r) && r) resize(r->right - r->left, r->bottom - r->top);
}

bool hostSupports(const char* what) {
    if (!what) return false;
    static const char* const kYes[] = {"sendVstEvents",   "sendVstMidiEvent", "sendVstMidiEventFlagIsRealtime",
                                       "sizeWindow",      "startStopProcess", "acceptIOChanges",
                                       "shellCategory",   "supplyIdle"};
    for (const char* y : kYes)
        if (!std::strcmp(what, y)) return true;
    return false;
}

void copyString(void* dst, const char* src, size_t size) {
    if (!dst) return;
    char* d = static_cast<char*>(dst);
    std::strncpy(d, src, size - 1);
    d[size - 1] = 0;
}

intptr_t VST_FUNCTION_INTERFACE hostCallback(vst_effect_t* effect, int32_t opcode, int32_t index, intptr_t value,
                                             void* ptr, float) {
    Vst2Instance* self = effect && effect->host_internal ? static_cast<Vst2Instance*>(effect->host_internal)
                                                         : t_loading.instance;
    switch (opcode) {
        case VST_HOST_OPCODE_VST_VERSION: return 2400;
        case VST_HOST_OPCODE_CURRENT_EFFECT_ID:
            // While a shell is being created: the plugin it should create. Later: the effect's own.
            if (t_loading.shellId) return t_loading.shellId;
            return effect ? effect->unique_id : 0;
        case VST_HOST_OPCODE_KEEPALIVE_OR_IDLE: return 1;
        case VST_HOST_OPCODE_GET_SAMPLE_RATE: return self ? (intptr_t)self->sampleRate() : 44100;
        case VST_HOST_OPCODE_GET_BLOCK_SIZE: return self ? (intptr_t)self->blockSize() : 512;
        case VST_HOST_OPCODE_GET_ACTIVE_THREAD:
            return currentThreadIsAudio() ? VST_HOST_ACTIVE_THREAD_AUDIO : VST_HOST_ACTIVE_THREAD_INTERFACE;
        case VST_HOST_OPCODE_VENDOR_NAME: copyString(ptr, "Brack", VST_BUFFER_SIZE_VENDOR_NAME); return 1;
        case VST_HOST_OPCODE_PRODUCT_NAME: copyString(ptr, "Brack", VST_BUFFER_SIZE_PRODUCT_NAME); return 1;
        case VST_HOST_OPCODE_VENDOR_VERSION: return 100;
        case VST_HOST_OPCODE_SUPPORTS: return hostSupports(static_cast<const char*>(ptr)) ? 1 : 0;
        case VST_HOST_OPCODE_LANGUAGE: return 1;  // English
        case VST_HOST_OPCODE_GET_EFFECT_DIRECTORY: return self ? (intptr_t)self->directory().c_str() : 0;
        case VST_HOST_OPCODE_IO_MODIFIED:
            if (self && !t_loading.instance) self->onIoChanged();
            return 1;
        case VST_HOST_OPCODE_EDITOR_RESIZE: return self && self->onEditorResize(index, (int)value) ? 1 : 0;
        case VST_HOST_OPCODE_AUTOMATE:          // a parameter changed (the user turned a knob, say)
        case VST_HOST_OPCODE_PARAM_STOP_EDIT:   // the end of such an edit
        case VST_HOST_OPCODE_EDITOR_UPDATE:     // the plugin changed itself (a program, say)
            if (self && !t_loading.instance) self->onStateDirty();
            return 1;
        case VST_HOST_OPCODE_PARAM_START_EDIT: return 1;
        case kVst2HostProcessEvents: return 1;  // MIDI the plugin sends is not used
        default: return 0;  // including the time info: brack has no transport
    }
}

// ---- scanning ----

struct ScanCall {
    MainFn main = nullptr;
    int32_t shellId = 0;
    vst_effect_t* effect = nullptr;
    PluginDescription desc;
    bool shell = false;
    std::vector<std::pair<int32_t, std::string>> subs;
};

void scanInstantiate(void* p) {
    auto* c = static_cast<ScanCall*>(p);
    c->effect = instantiate(c->main, nullptr, c->shellId);
}

void scanDescribe(void* p) {
    auto* c = static_cast<ScanCall*>(p);
    vst_effect_t* e = c->effect;
    e->control(e, VST_EFFECT_OPCODE_CREATE, 0, 0, nullptr, 0);
    c->shell = isShell(e);
    if (c->shell) c->subs = shellPlugins(e);
    else fillDescription(e, c->desc);
}

void scanDestroy(void* p) {
    auto* c = static_cast<ScanCall*>(p);
    c->effect->control(c->effect, VST_EFFECT_OPCODE_DESTROY, 0, 0, nullptr, 0);
}

// Instantiates (a shell's plugin `shellId`, or the file's) and describes it, guarded.
bool scanOne(ScanCall& c, std::string& error) {
    if (!runGuarded(&scanInstantiate, &c)) {
        error = "crashed while instantiating";
        return false;
    }
    if (!c.effect) {
        error = "failed to instantiate";
        return false;
    }
    if (!runGuarded(&scanDescribe, &c)) {
        error = "crashed while describing itself";
        return false;  // left alone: destroying a crashed plugin is asking for more
    }
    runGuarded(&scanDestroy, &c);
    return true;
}

}  // namespace

std::unique_ptr<PluginInstance> createVst2Instance(HostThread& host, PluginHostListener& listener,
                                                   const std::string& pathUtf8, const std::string& pluginId,
                                                   std::string& error) {
    auto module = loadModule(pathUtf8, error);
    if (!module) return nullptr;
    PluginDescription desc;
    desc.format = PluginFormat::Vst2;
    desc.path = module->path;
    desc.id = pluginId;
    std::string name = pathToUtf8(pathFromUtf8(module->path).stem());
    return std::make_unique<Vst2Instance>(host, listener, std::move(module), std::move(desc), name);
}

std::vector<PluginDescription> describeVst2File(const std::filesystem::path& file, std::string& error) {
    std::vector<PluginDescription> result;
    auto module = loadModule(pathToUtf8(file), error);
    if (!module) return result;
    const std::string stem = pathToUtf8(file.stem());
    ScanCall top;
    top.main = module->main;
    if (!scanOne(top, error)) return result;
    if (!top.shell) {
        top.desc.path = module->path;
        if (top.desc.name.empty()) top.desc.name = stem;
        result.push_back(std::move(top.desc));
        return result;
    }
    for (auto& [id, name] : top.subs) {
        ScanCall sub;
        sub.main = module->main;
        sub.shellId = id;
        std::string err;
        if (!scanOne(sub, err) || sub.shell) {
            logWarn("scan: " + module->path + ": " + vst2IdToString(id) + " (" + name + "): " +
                    (err.empty() ? "not created by the shell" : err));
            continue;
        }
        sub.desc.path = module->path;
        sub.desc.id = vst2IdToString(id);
        if (!name.empty()) sub.desc.name = name;
        result.push_back(std::move(sub.desc));
    }
    if (result.empty() && error.empty()) error = "VST2 shell contains no plugins";
    return result;
}

}  // namespace brack
