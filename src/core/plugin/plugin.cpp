#include "plugin/plugin.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <utility>

#include "host_thread.h"
#include "util/common.h"

namespace brack {

const char* pluginFormatName(PluginFormat f) {
    switch (f) {
        case PluginFormat::Clap: return "clap";
        case PluginFormat::Vst2: return "vst2";
        case PluginFormat::Vst3: return "vst3";
    }
    return "clap";
}

const char* pluginFormatLabel(PluginFormat f) {
    switch (f) {
        case PluginFormat::Clap: return "CLAP";
        case PluginFormat::Vst2: return "VST2";
        case PluginFormat::Vst3: return "VST3";
    }
    return "CLAP";
}

std::string pluginLabel(const PluginDescription& d) {
    const bool foreign = d.architecture != buildArchitecture();
    return d.name + " [" + pluginFormatLabel(d.format) + (foreign ? ", " + d.architecture : "") + "]";
}

// ---------------------------------------------------------------------------
// InputEventList

InputEventList::InputEventList(size_t maxEvents, size_t sysexArenaBytes)
    : events_(maxEvents), arena_(sysexArenaBytes) {
    clap_.ctx = this;
    clap_.size = &InputEventList::sizeCb;
    clap_.get = &InputEventList::getCb;
}

uint32_t InputEventList::sizeCb(const clap_input_events_t* l) {
    return (uint32_t) static_cast<const InputEventList*>(l->ctx)->count_;
}

const clap_event_header_t* InputEventList::getCb(const clap_input_events_t* l, uint32_t i) {
    auto* self = static_cast<const InputEventList*>(l->ctx);
    return i < self->count_ ? &self->events_[i].header : nullptr;
}

bool InputEventList::pushMidi(uint16_t port, const uint8_t* data, uint32_t size, uint32_t time) {
    if (count_ >= events_.size() || size == 0) return false;
    Event& e = events_[count_];
    if (data[0] == 0xF0) {
        if (arenaUsed_ + size > arena_.size()) return false;
        std::memcpy(arena_.data() + arenaUsed_, data, size);
        e.sysex.header = {sizeof(clap_event_midi_sysex_t), time, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_MIDI_SYSEX, 0};
        e.sysex.port_index = port;
        e.sysex.buffer = arena_.data() + arenaUsed_;
        e.sysex.size = size;
        arenaUsed_ += size;
    } else {
        if (size > 3) return false;
        e.midi.header = {sizeof(clap_event_midi_t), time, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_MIDI, 0};
        e.midi.port_index = port;
        e.midi.data[0] = data[0];
        e.midi.data[1] = size > 1 ? data[1] : 0;
        e.midi.data[2] = size > 2 ? data[2] : 0;
    }
    ++count_;
    return true;
}

bool InputEventList::pushNote(uint16_t type, uint16_t port, uint8_t channel, uint8_t key, double velocity,
                              uint32_t time) {
    if (count_ >= events_.size()) return false;
    Event& e = events_[count_++];
    e.note.header = {sizeof(clap_event_note_t), time, CLAP_CORE_EVENT_SPACE_ID, type, 0};
    e.note.note_id = -1;
    e.note.port_index = (int16_t)port;
    e.note.channel = channel;
    e.note.key = key;
    e.note.velocity = velocity;
    return true;
}

void InputEventList::sortByTime() {
    // Insertion sort: stable, allocation free, and the lists are short and nearly sorted.
    for (size_t i = 1; i < count_; ++i) {
        if (events_[i - 1].header.time <= events_[i].header.time) continue;
        Event e = events_[i];
        size_t j = i;
        while (j > 0 && events_[j - 1].header.time > e.header.time) {
            events_[j] = events_[j - 1];
            --j;
        }
        events_[j] = e;
    }
}

// ---------------------------------------------------------------------------
// PluginInstance

PluginInstance::PluginInstance(HostThread& host, PluginHostListener& listener, PluginDescription desc,
                               std::string displayName)
    : HostedPlugin(std::move(desc)), host_(host), listener_(listener), displayName_(std::move(displayName)) {
    desc_.architecture = buildArchitecture();
}

PluginInstance::~PluginInstance() = default;

void PluginInstance::shutdown() {
    closeGui();
    deactivate();
}

void PluginInstance::keepForever(std::shared_ptr<void> p) {
    static auto* kept = new std::vector<std::shared_ptr<void>>;  // never destroyed, on purpose
    static std::mutex mutex;
    std::lock_guard lock(mutex);
    kept->push_back(std::move(p));
}

void PluginInstance::recordCrash(const char* what, const GuardFault& fault) {
    bool expected = false;
    if (!crashClaimed_.compare_exchange_strong(expected, true)) return;  // another thread's crash came first
    crashWhat_ = what;
    crashFault_ = fault;
    processFailed_.store(true, std::memory_order_relaxed);
    crashed_.store(true, std::memory_order_release);
}

std::string PluginInstance::crashReport() const {
    if (!crashed()) return {};
    return faultReport(crashWhat_, crashFault_);
}

bool PluginInstance::init(std::string& error) {
    bool ok = false;
    callPlugin("init", [&] { ok = initPlugin(error); });
    if (crashed()) {
        error = crashReport();
        return false;
    }
    if (!ok) return false;
    callPlugin("init", [&] { hasGui_ = pluginHasGui(); });
    return !crashed();
}

void PluginInstance::idle() {
    callPlugin("idle", [&] { idlePlugin(); });
    if (crashed() && !crashHandled_) {
        crashHandled_ = true;
        closeGui();  // its window would only offer more ways into the broken plugin
    }
}

bool PluginInstance::saveState(std::vector<uint8_t>& out) {
    bool ok = false;
    callPlugin("save state", [&] { ok = saveStatePlugin(out); });
    if (ok && !crashed()) {
        lastState_ = out;
        return true;
    }
    if (crashed() && lastState_) {
        out = *lastState_;
        return true;
    }
    return false;
}

bool PluginInstance::loadState(const std::vector<uint8_t>& in) {
    bool ok = false;
    callPlugin("load state", [&] { ok = loadStatePlugin(in); });
    if (ok && !crashed()) lastState_ = in;
    return ok && !crashed();
}

bool PluginInstance::isMainThread() const { return host_.isCurrent(); }

std::string PluginInstance::displayName() const {
    std::lock_guard lock(nameMutex_);
    return displayName_;
}

void PluginInstance::setDisplayName(std::string name) {
    {
        std::lock_guard lock(nameMutex_);
        displayName_ = name;
    }
    if (editor_) callPlugin("editor", [&] { editor_->setTitle(name); });
}

void PluginInstance::allocateBuffers(uint32_t maxFrames) {
    maxFrames_ = maxFrames;
    auto build = [&](const std::vector<AudioPortInfo>& ports, std::vector<std::vector<float>>& storage,
                     std::vector<std::vector<float*>>& ptrs) {
        storage.clear();
        ptrs.clear();
        size_t totalCh = 0;
        for (auto& p : ports) totalCh += p.channels;
        storage.assign(totalCh, std::vector<float>(maxFrames, 0.0f));
        ptrs.resize(ports.size());
        size_t ch = 0;
        for (size_t i = 0; i < ports.size(); ++i)
            for (uint32_t c = 0; c < ports[i].channels; ++c) ptrs[i].push_back(storage[ch++].data());
    };
    build(audioIns_, inStorage_, inPtrs_);
    build(audioOuts_, outStorage_, outPtrs_);
}

bool PluginInstance::activate(double sampleRate, uint32_t maxFrames, std::string& error) {
    if (active_) deactivate();
    sampleRate_ = sampleRate;
    if (crashed()) {
        error = crashReport();
        return false;
    }
    bool ok = false;
    callPlugin("activate", [&] {
        queryPorts();
        allocateBuffers(maxFrames);
        ok = activatePlugin(sampleRate, maxFrames, error);
        if (ok) latency_.store(pluginLatency(), std::memory_order_relaxed);
    });
    if (crashed()) {
        error = crashReport();
        return false;
    }
    if (!ok) {
        if (error.empty()) error = "activate failed (" + desc_.name + ")";
        return false;
    }
    processing_ = false;
    processFailed_ = false;
    stopRequested_ = false;
    stopAck_ = false;
    restartRequested_ = false;
    active_ = true;
    return true;
}

void PluginInstance::deactivate() {
    if (!active_) return;
    if (!stopAck_) forceStopProcessing();
    active_ = false;
    callPlugin("deactivate", [&] { deactivatePlugin(); });
}

void PluginInstance::requestStopProcessing() {
    if (!active_) return;
    stopAck_.store(false, std::memory_order_release);
    stopRequested_.store(true, std::memory_order_release);
}

void PluginInstance::forceStopProcessing() {
    // The audio thread is gone (device stopped or lost); act as the audio thread.
    setCurrentThreadIsAudio(true);
    if (processing_) callPlugin("stop processing", [&] { stopProcessing(); });
    setCurrentThreadIsAudio(false);
    processing_ = false;
    stopRequested_ = true;
    stopAck_ = true;
}

void PluginInstance::portsChanged() {
    if (!host_.isCurrent()) return;
    if (!active_) {
        callPlugin("query ports", [&] { queryPorts(); });
    } else {
        restartRequested_ = true;  // re-read after a deactivate/activate cycle
    }
}

void PluginInstance::appendMidi(InputEventList& list, uint16_t port, const uint8_t* data, uint32_t size,
                                uint32_t time) const {
    if (port < notePorts_.size()) list.pushMidi(port, data, size, time);
}

bool PluginInstance::process(uint32_t frames, uint64_t steadyTime, const InputEventList& in) {
    if (!active_.load(std::memory_order_acquire)) return false;
    if (stopRequested_.load(std::memory_order_acquire)) {
        if (processing_) {
            callPlugin("stop processing", [&] { stopProcessing(); });
            processing_ = false;
        }
        stopAck_.store(true, std::memory_order_release);
        return false;
    }
    if (processFailed_.load(std::memory_order_relaxed)) return false;  // also once crashed
    if (!processing_) {
        bool started = false;
        callPlugin("start processing", [&] { started = startProcessing(); });
        if (!started) {
            processFailed_ = true;
            return false;
        }
        processing_ = true;
    }
    bool ok = false;
    callPlugin("process", [&] { ok = processPlugin(std::min(frames, maxFrames_), steadyTime, in); });
    if (!ok) {
        processFailed_ = true;
        return false;
    }
    return true;
}

void PluginInstance::beginProcess(uint32_t frames, uint64_t steadyTime, const InputEventList& in) {
    pendingFrames_ = frames;
    pendingTime_ = steadyTime;
    pendingIn_ = &in;
}

bool PluginInstance::endProcess() {
    const InputEventList* in = std::exchange(pendingIn_, nullptr);
    return in && process(pendingFrames_, pendingTime_, *in);
}

const float* PluginInstance::outputChannel(uint32_t port, uint32_t channel) const {
    if (port >= outPtrs_.size() || channel >= outPtrs_[port].size()) return nullptr;
    return outPtrs_[port][channel];
}

// ---- GUI ----

bool PluginInstance::openGui(std::string& error, void* parent) {
    if (crashed()) {
        error = crashReport();
        return false;
    }
    if (editor_ && editorParent_ != parent) closeGui();
    if (editor_) {
        callPlugin("editor", [&] { editor_->show(); });
        return !crashed();
    }
    if (!hasGui()) {
        error = "plugin has no GUI";
        return false;
    }
    editorParent_ = parent;
    {
        ParentDpiScope dpi(parent);
        callPlugin("open editor", [&] { editor_ = createEditor(error); });
    }
    if (crashed()) {
        (void)editor_.release();  // half-built and the plugin's: leave it alone
        error = crashReport();
        return false;
    }
    if (editor_) ++editorGeneration_;
    return editor_ != nullptr;
}

void PluginInstance::closeGui() {
    if (!editor_) return;
    if (crashed()) {
        editor_->abandon();
        (void)editor_.release();
    } else if (!callPlugin("close editor", [&] { editor_.reset(); })) {
        (void)editor_.release();  // crashed tearing its view down, partly destroyed
    }
    listener_.pluginGuiClosed(*this);
}

HostWindow::Placement PluginInstance::editorPlacement() {
    HostWindow::Placement p;
    p.parent = editorParent_;
    p.sized = [this](uint32_t w, uint32_t h) { listener_.pluginEditorResized(*this, w, h); };
    return p;
}

void PluginInstance::editorClosedByUser(bool hideNow) {
    if (editor_ && hideNow) callPlugin("editor", [&] { editor_->hide(); });
    host_.post([this, alive = std::weak_ptr<int>(lifeToken_), gen = editorGeneration_] {
        // Not if the instance is gone, or the editor was closed and opened again meanwhile.
        if (alive.lock() && gen == editorGeneration_) closeGui();
    });
}

}  // namespace brack
