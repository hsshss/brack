// RemoteChain, the HostedChain for a chain running in a plugin host process, and
// RemoteDescriber, which describes plugin files in them. The same on every OS; the process itself
// is remote/host_process.h.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <map>
#include <mutex>
#include <optional>

#include "remote/host_process.h"
#include "remote/host_protocol.h"
#include "remote/plugin_host.h"
#include "util/common.h"

namespace brack {

using remote::BlockCommand;
using remote::BlockResult;
using remote::ControlBlock;
using remote::Message;
using namespace std::chrono_literals;

namespace {

// For the host, which may not share this process's current directory by the time it reads it.
std::string absolute(const std::string& pathUtf8) {
    std::error_code ec;
    const std::filesystem::path p = std::filesystem::absolute(pathFromUtf8(pathUtf8), ec);
    return ec ? pathUtf8 : pathToUtf8(p);
}

// How long a plugin may take to answer before it counts as hung. Generous where plugins do
// real work: loading samples on creation or with a state, building a large editor.
constexpr auto kCreateTimeout = 60s;
constexpr auto kStateTimeout = 60s;
constexpr auto kEditorTimeout = 30s;
constexpr auto kActivateTimeout = 30s;
constexpr auto kShortTimeout = 10s;
constexpr uint32_t kProcessTimeoutMs = 2000;  // one audio block

class RemoteChain;

// A plugin of a RemoteChain: what the host said of it, and requests about it.
class RemotePlugin final : public HostedPlugin {
public:
    RemotePlugin(RemoteChain& chain, const Message& info, const std::string& path, const std::string& architecture);

    bool isActive() const override;
    bool saveState(std::vector<uint8_t>& out) override;
    bool loadState(const std::vector<uint8_t>& in) override;
    bool isGuiOpen() const override;
    bool openGui(std::string& error, void* parent) override;
    void closeGui() override;
    void setDisplayName(std::string name) override;
    bool processFailed() const override;
    bool crashed() const override;
    std::string crashReport() const override;

    // From an activation's reply, or from inserting an effect.
    void activated(const Message& m) {
        remote::portsFromMessage(m.at("ports"), notePorts_, audioIns_, audioOuts_);
        latency_.store(m.value("latency", 0u), std::memory_order_relaxed);
        active_ = m.value("active", false);
    }
    void deactivated() { active_ = false; }
    void guiClosed();  // by the host, or with it

    bool failed = false;    // as the host last said (its "failed" note)
    bool prepared = false;  // made, not yet in the chain (see RemoteChain::prepareEffect())

private:
    RemoteChain& chain_;
    bool active_ = false;
    bool guiOpen_ = false;
    void* guiParent_ = nullptr;
    std::optional<std::vector<uint8_t>> lastState_;  // last saved or loaded
};

class RemoteChain final : public HostedChain {
public:
    RemoteChain(PluginHostListener& listener, std::string architecture)
        : listener_(listener), architecture_(std::move(architecture)) {}

    ~RemoteChain() override {
        if (!host_) return;
        destroying_ = true;
        if (!crashed()) {
            Message reply;
            std::lock_guard lock(io_);
            host_->call({{"op", "destroy"}}, reply, 5s);
        }
    }

    // Starts the host and makes the chain: with the instrument at `path`, or without one.
    bool start(const std::string& path, const std::string& pluginId, std::string& error) {
        host_ = HostProcess::start(architecture_, error);
        if (!host_) return false;
        host_->onNote = [this](const Message& m) { queueNote(m); };
        Message request, r;
        if (!path.empty()) request = {{"path", absolute(path)}, {"pluginId", pluginId}};
        if (!call("create", request, r, kCreateTimeout, &error)) return false;
        if (!r.value("ok", false)) {
            error = r.value("error", "the plugin did not load");
            return false;
        }
        if (!path.empty()) {
            plugins_.push_back(std::make_unique<RemotePlugin>(*this, r.at("plugin"), path, architecture_));
            hasInstrument_ = true;
        }
        return true;
    }

    // A request and its reply; false once the host is gone (error: why). `preparing`: of the
    // prepare step, which leaves notes queued.
    bool call(const char* op, Message request, Message& reply, std::chrono::milliseconds timeout,
              std::string* error = nullptr, bool preparing = false) {
        if (!crashed()) {
            request["op"] = op;
            HostProcess::Outcome o;
            std::vector<Message> notes;
            {
                std::lock_guard lock(io_);
                o = host_->call(request, reply, timeout);
                if (!preparing) notes.swap(notes_);
            }
            for (const Message& m : notes) onNote(m);
            switch (o) {
                case HostProcess::Outcome::Ok:
                    if (!preparing) checkStateChanges();
                    return true;
                case HostProcess::Outcome::Ended: end(Ending::Ended, op); break;
                case HostProcess::Outcome::TimedOut: end(Ending::Hung, op); break;
            }
        }
        if (error) *error = crashReport();
        return false;
    }
    // The same about one of the plugins.
    bool call(const RemotePlugin& p, const char* op, Message request, Message& reply,
              std::chrono::milliseconds timeout, std::string* error = nullptr) {
        if (p.prepared) request["prepared"] = true;
        else request["plugin"] = indexOf(p);
        return call(op, std::move(request), reply, timeout, error, p.prepared);
    }

    void allowForeground() { host_->allowForeground(); }

    // ---- HostedChain ----
    size_t size() const override { return plugins_.size(); }
    HostedPlugin& plugin(size_t i) override { return *plugins_[i]; }
    bool hasInstrument() const override { return hasInstrument_; }
    bool separateProcess() const override { return true; }

    // Off the host thread, this and the prepared effect's requests take turns with the host
    // thread's (io_), and touch nothing else it changes.
    HostedPlugin* prepareEffect(const std::string& pathUtf8, const std::string& pluginId,
                                std::string& error) override {
        prepared_.reset();
        Message r;
        if (!call("prepareEffect", {{"path", absolute(pathUtf8)}, {"pluginId", pluginId}}, r, kCreateTimeout, &error,
                  true))
            return nullptr;
        if (!r.value("ok", false)) {
            error = r.value("error", "the plugin did not load");
            return nullptr;
        }
        prepared_ = std::make_unique<RemotePlugin>(*this, r.at("plugin"), pathUtf8, architecture_);
        prepared_->prepared = true;
        return prepared_.get();
    }

    HostedPlugin* insertEffect(size_t index) override {
        if (!prepared_) return nullptr;
        index = std::clamp(index, firstEffect(), plugins_.size());
        Message r;
        if (!call("insertEffect", {{"index", index}}, r, kShortTimeout) || !r.value("ok", false)) return nullptr;
        prepared_->prepared = false;
        prepared_->activated(r.at("plugin"));  // the chain's activation may have changed since
        plugins_.insert(plugins_.begin() + (ptrdiff_t)index, std::move(prepared_));
        return plugins_[index].get();
    }

    void remove(size_t index) override {
        if (index < firstEffect() || index >= plugins_.size()) return;
        Message ignored;
        call("remove", {{"index", index}}, ignored, kShortTimeout);  // its "guiClosed" comes meanwhile
        plugins_[index]->guiClosed();  // it did not come: the host is gone
        plugins_.erase(plugins_.begin() + (ptrdiff_t)index);
    }

    void move(size_t from, size_t to) override {
        if (from < firstEffect() || from >= plugins_.size()) return;
        to = std::clamp(to, firstEffect(), plugins_.size() - 1);
        Message ignored;
        call("move", {{"from", from}, {"to", to}}, ignored, kShortTimeout);
        auto p = std::move(plugins_[from]);
        plugins_.erase(plugins_.begin() + (ptrdiff_t)from);
        plugins_.insert(plugins_.begin() + (ptrdiff_t)to, std::move(p));
    }

    void setBypass(size_t index, bool bypass) override {
        if (index < firstEffect() || index >= plugins_.size()) return;
        Message ignored;
        call("bypass", {{"index", index}, {"bypass", bypass}}, ignored, kShortTimeout);
    }

    bool activate(double sampleRate, uint32_t maxFrames, uint32_t busChannels, std::string& error) override {
        if (active_) deactivate();
        Message r;
        if (!call("activate", {{"rate", sampleRate}, {"maxFrames", maxFrames}, {"busChannels", busChannels}}, r,
                  kActivateTimeout, &error))
            return false;
        if (!r.value("ok", false)) {
            error = r.value("error", "");
            if (error.empty()) error = "activate failed";
            return false;
        }
        const Message& infos = r.at("plugins");
        for (size_t i = 0; i < plugins_.size() && i < infos.size(); ++i) plugins_[i]->activated(infos[i]);
        inputChannels_ = r.value("inputChannels", 0u);
        const auto outputs = r.value("outputPorts", std::vector<uint32_t>{});
        uint32_t channels = inputChannels_;
        for (uint32_t n : outputs) channels += n;
        float* base = host_->mapOutputs(r.value("channels", Message()), (uint64_t)channels * maxFrames * sizeof(float));
        if (channels && !base) {
            Message ignored;
            call("deactivate", {}, ignored, kShortTimeout);
            error = "cannot map the chain's channels";
            return false;
        }
        maxFrames_ = maxFrames;
        inPtrs_.clear();
        for (uint32_t c = 0; c < inputChannels_; ++c) inPtrs_.push_back(base + c * maxFrames);
        outPtrs_.assign(outputs.size(), {});
        for (size_t p = 0, ch = inputChannels_; p < outputs.size(); ++p)
            for (uint32_t c = 0; c < outputs[p]; ++c) outPtrs_[p].push_back(base + (ch++) * maxFrames);
        stopRequested_ = false;
        stopAck_ = false;
        processFailed_ = false;
        restartPending_ = false;
        active_ = true;
        return true;
    }

    void deactivate() override {
        if (!active_) return;
        active_ = false;
        Message ignored;
        call("deactivate", {}, ignored, kShortTimeout);
        for (auto& p : plugins_) p->deactivated();
        inPtrs_.clear();
        outPtrs_.clear();
        host_->unmapOutputs();
    }

    bool isActive() const override { return active_; }

    void idle() override {
        if (!crashed()) {
            std::vector<Message> notes;
            if (std::unique_lock lock(io_, std::try_to_lock); lock) {  // not while a prepare step's request waits
                host_->pollNotes();
                notes.swap(notes_);
            }
            for (const Message& m : notes) onNote(m);
            checkStateChanges();
            if (!host_->running()) end(Ending::Ended, "idle");
        }
        if (crashed())  // the editors went with the process
            for (auto& p : plugins_) p->guiClosed();
    }

    void requestStopProcessing() override {
        if (!active_) return;
        stopAck_.store(false, std::memory_order_release);
        stopRequested_.store(true, std::memory_order_release);
    }
    bool stopProcessingAcknowledged() const override { return stopAck_.load(std::memory_order_acquire) || crashed(); }
    void resumeProcessing() override {
        if (!active_) return;
        stopRequested_.store(false, std::memory_order_release);
        stopAck_.store(false, std::memory_order_release);
    }

    bool restartPending() const override { return restartPending_; }
    bool processFailed() const { return processFailed_ || crashed(); }
    bool crashed() const override { return ending_.load(std::memory_order_acquire) != Ending::None; }
    int crashedPlugin() const override { return crashed() ? (int)host_->control().crashedPlugin - 1 : -1; }
    std::string crashReport() const override {
        if (!crashed()) return {};
        std::lock_guard lock(reportMutex_);
        if (report_.empty()) {
            // A host that recorded a crash and then hung (in the allocator's lock, say) crashed.
            if (ending_ == Ending::Hung && !host_->control().crashed)
                report_ = std::string("stopped responding in ") + endedDuring_;
            else
                report_ = host_->endReport();
        }
        return report_;
    }

    // ---- audio thread ----
    bool appendMidi(InputEventList& list, uint16_t port, const uint8_t* data, uint32_t size,
                    uint32_t time) const override {
        // The host converts it, and may drop it there (a port that takes only CLAP notes).
        return hasInstrument_ && port < plugins_[0]->notePorts().size() && list.pushMidi(port, data, size, time);
    }

    void beginProcess(uint32_t frames, uint64_t steadyTime, const InputEventList& in,
                      const float* const* input) override {
        kicked_ = false;
        if (!active_.load(std::memory_order_acquire) || crashed()) return;
        ControlBlock& c = host_->control();
        if (stopRequested_.load(std::memory_order_acquire)) {
            if (stopAck_.load(std::memory_order_relaxed)) return;
            c.command = (uint32_t)BlockCommand::Stop;
        } else {
            if (processFailed_.load(std::memory_order_relaxed)) return;
            c.command = (uint32_t)BlockCommand::Process;
            c.frames = frames;
            c.steadyTime = steadyTime;
            c.eventCount = copyEvents(c, in);
            for (uint32_t ch = 0; ch < inputChannels_ && input; ++ch)
                std::memcpy(inPtrs_[ch], input[ch], std::min(frames, maxFrames_) * sizeof(float));
        }
        host_->signalBlock();
        kicked_ = true;
    }

    bool endProcess() override {
        processNs_ = 0;
        if (!kicked_) return false;
        kicked_ = false;
        const HostProcess::Outcome o = host_->waitBlock(kProcessTimeoutMs);
        if (o == HostProcess::Outcome::Ended) {
            end(Ending::Ended, "process");
        } else if (o == HostProcess::Outcome::TimedOut) {
            end(Ending::Hung, "process");
        } else {
            checkStateChanges();
            processNs_ = host_->control().processNs;
            switch ((BlockResult)host_->control().result) {
                case BlockResult::Audio: return true;
                case BlockResult::Stopped: stopAck_.store(true, std::memory_order_release); break;
                case BlockResult::Failed: processFailed_ = true; break;
            }
        }
        return false;
    }

    uint32_t processNs() const override { return processNs_; }

    const float* outputChannel(uint32_t port, uint32_t channel) const override {
        if (port >= outPtrs_.size() || channel >= outPtrs_[port].size()) return nullptr;
        return outPtrs_[port][channel];
    }

    size_t indexOf(const RemotePlugin& p) const {
        for (size_t i = 0; i < plugins_.size(); ++i)
            if (plugins_[i].get() == &p) return i;
        return plugins_.size();
    }
    PluginHostListener& listener() { return listener_; }

private:
    enum class Ending : uint8_t { None, Ended, Hung };

    size_t firstEffect() const { return hasInstrument_ ? 1 : 0; }

    // Any thread, no allocation: the host is gone, or hung (and is ended now). The first call
    // decides; what it says is in place before crashed() is true.
    void end(Ending how, const char* during) {
        if (endClaimed_.exchange(true, std::memory_order_acq_rel)) return;
        endedDuring_ = during;
        ending_.store(how, std::memory_order_release);
        if (how == Ending::Hung) host_->terminate();
    }

    // With io_ held. A log line goes out at once.
    void queueNote(const Message& m) {
        if (m.value("note", "") == "log") log((LogLevel)m.value("level", (int)LogLevel::Info), m.value("text", ""));
        else notes_.push_back(m);
    }

    void onNote(const Message& m) {
        const std::string note = m.value("note", "");
        if (destroying_) return;
        const size_t i = m.value("plugin", (size_t)0);
        if (note == "guiClosed") {
            if (i < plugins_.size()) plugins_[i]->guiClosed();
        } else if (note == "resized") {
            if (i < plugins_.size())
                listener_.pluginEditorResized(*plugins_[i], m.value("width", 0u), m.value("height", 0u));
        } else if (note == "restart") {
            restartPending_ = true;
        } else if (note == "failed") {
            for (auto& p : plugins_) p->failed = false;
            for (const Message& at : m.value("plugins", Message::array()))
                if (const size_t k = at.get<size_t>(); k < plugins_.size()) plugins_[k]->failed = true;
        }
    }

    // Audio or host thread: tells the listener once for any number of changes the host counted.
    void checkStateChanges() {
        const uint32_t now = std::atomic_ref(host_->control().stateChanges).load(std::memory_order_acquire);
        uint32_t seen = stateSeen_.load(std::memory_order_relaxed);
        if (now != seen && stateSeen_.compare_exchange_strong(seen, now) && !destroying_ && !plugins_.empty())
            listener_.pluginStateChanged(*plugins_[0]);
    }

    // The block's MIDI, as the host's appendMidi() will want it.
    static uint32_t copyEvents(ControlBlock& c, const InputEventList& in) {
        uint32_t n = 0, sysexUsed = 0;
        for (size_t i = 0; i < in.size() && n < remote::kMaxEvents; ++i) {
            const clap_event_header_t* h = in.at(i);
            remote::MidiEvent& e = c.events[n];
            if (h->type == CLAP_EVENT_MIDI) {
                auto* m = reinterpret_cast<const clap_event_midi_t*>(h);
                e.size = (uint16_t)midiShortMessageLength(m->data[0]);
                if (!e.size) continue;
                std::memcpy(e.bytes, m->data, 3);
                e.port = m->port_index;
            } else if (h->type == CLAP_EVENT_MIDI_SYSEX) {
                auto* s = reinterpret_cast<const clap_event_midi_sysex_t*>(h);
                if (s->size == 0 || s->size > 0xFFFF) continue;
                if (s->size <= sizeof e.bytes) {
                    std::memcpy(e.bytes, s->buffer, s->size);
                } else {
                    if (s->size > remote::kSysexBytes - sysexUsed) continue;
                    std::memcpy(c.sysex + sysexUsed, s->buffer, s->size);
                    e.offset = sysexUsed;
                    sysexUsed += s->size;
                }
                e.size = (uint16_t)s->size;
                e.port = s->port_index;
            } else {
                continue;
            }
            e.time = h->time;
            ++n;
        }
        return n;
    }

    PluginHostListener& listener_;
    const std::string architecture_;
    std::unique_ptr<HostProcess> host_;
    std::vector<std::unique_ptr<RemotePlugin>> plugins_;
    std::unique_ptr<RemotePlugin> prepared_;  // see prepareEffect()
    bool hasInstrument_ = false;
    bool destroying_ = false;

    std::atomic<bool> active_{false};
    std::atomic<bool> stopRequested_{false}, stopAck_{true}, processFailed_{false};
    bool restartPending_ = false;

    std::atomic<uint32_t> stateSeen_{0};
    std::atomic<bool> endClaimed_{false};
    std::atomic<Ending> ending_{Ending::None};
    const char* endedDuring_ = "";  // written before ending_
    mutable std::mutex reportMutex_;
    mutable std::string report_;

    std::mutex io_;               // the host's pipe: one request or poll at a time
    std::vector<Message> notes_;  // with io_: notes not yet handled

    uint32_t maxFrames_ = 0, inputChannels_ = 0;
    std::vector<float*> inPtrs_;                // into the shared channels
    std::vector<std::vector<float*>> outPtrs_;  // likewise
    bool kicked_ = false;                       // audio thread
    uint32_t processNs_ = 0;                    // audio thread
};

RemotePlugin::RemotePlugin(RemoteChain& chain, const Message& info, const std::string& path,
                           const std::string& architecture)
    : HostedPlugin(remote::descriptionFromMessage(info.at("description"))), chain_(chain) {
    desc_.path = path;  // as given, like a plugin in this process
    desc_.architecture = architecture;
    hasGui_ = info.value("hasGui", false);
    remote::portsFromMessage(info.value("ports", Message::object()), notePorts_, audioIns_, audioOuts_);
}

bool RemotePlugin::isActive() const { return active_ && chain_.isActive(); }

bool RemotePlugin::saveState(std::vector<uint8_t>& out) {
    Message r;
    if (chain_.call(*this, "saveState", {}, r, kStateTimeout) && r.value("ok", false)) {
        const auto& bin = r.at("state").get_binary();
        out.assign(bin.begin(), bin.end());
        lastState_ = out;
        return true;
    }
    if (crashed() && lastState_) {
        out = *lastState_;
        return true;
    }
    return false;
}

bool RemotePlugin::loadState(const std::vector<uint8_t>& in) {
    Message r;
    if (!chain_.call(*this, "loadState", {{"state", Message::binary(in)}}, r, kStateTimeout) || !r.value("ok", false))
        return false;
    lastState_ = in;
    return true;
}

bool RemotePlugin::isGuiOpen() const { return guiOpen_ && !crashed(); }

bool RemotePlugin::openGui(std::string& error, void* parent) {
    if (crashed()) {
        error = crashReport();
        return false;
    }
    if (guiOpen_ && parent != guiParent_) closeGui();
    if (!hasGui()) {
        error = "plugin has no GUI";
        return false;
    }
#ifdef __APPLE__
    if (parent) {  // an NSView is not shared between processes
        error = "on macOS an editor opens inside the application's window only with plugins in process";
        return false;
    }
#endif
    chain_.allowForeground();
    Message r;
    if (!chain_.call(*this, "openGui", {{"parent", (uint64_t)(uintptr_t)parent}}, r, kEditorTimeout, &error))
        return false;
    if (!r.value("ok", false)) {
        error = r.value("error", "the editor did not open");
        return false;
    }
    guiOpen_ = true;
    guiParent_ = parent;
    return true;
}

void RemotePlugin::closeGui() {
    if (!guiOpen_) return;
    Message ignored;
    chain_.call(*this, "closeGui", {}, ignored, kShortTimeout);  // the host's "guiClosed" note follows
    guiClosed();  // it did not come: the host is gone
}

void RemotePlugin::guiClosed() {
    if (!guiOpen_) return;
    guiOpen_ = false;
    chain_.listener().pluginGuiClosed(*this);
}

void RemotePlugin::setDisplayName(std::string name) {
    Message ignored;
    chain_.call(*this, "setName", {{"name", name}}, ignored, kShortTimeout);
}

bool RemotePlugin::processFailed() const {
    return failed || (chain_.indexOf(*this) == 0 && chain_.hasInstrument() && chain_.processFailed());
}
bool RemotePlugin::crashed() const { return chain_.crashed(); }
std::string RemotePlugin::crashReport() const { return chain_.crashReport(); }

}  // namespace

std::unique_ptr<HostedChain> createRemoteChain(PluginHostListener& listener, const std::string& pathUtf8,
                                               const std::string& pluginId, const std::string& architecture,
                                               std::string& error) {
    auto chain = std::make_unique<RemoteChain>(listener, architecture);
    if (!chain->start(pathUtf8, pluginId, error)) return nullptr;
    return chain;
}

std::unique_ptr<HostedChain> createRemoteMasterChain(PluginHostListener& listener, std::string& error) {
    return createRemoteChain(listener, {}, {}, buildArchitecture(), error);
}

// ---------------------------------------------------------------------------

struct RemoteDescriber::Impl {
    std::map<std::string, std::unique_ptr<HostProcess>> hosts;  // by architecture
    std::map<std::string, std::string> startErrors;            // architectures whose host would not start
};

RemoteDescriber::RemoteDescriber() : impl_(std::make_unique<Impl>()) {}
RemoteDescriber::~RemoteDescriber() = default;

bool RemoteDescriber::runs(const std::string& architecture) const {
    std::error_code ec;
    return architectureRunsHere(architecture) && std::filesystem::is_regular_file(pluginHostExecutable(architecture), ec);
}

std::optional<std::vector<PluginDescription>> RemoteDescriber::describe(const std::filesystem::path& file,
                                                                        const std::string& architecture,
                                                                        std::string& error) {
    auto& host = impl_->hosts[architecture];
    if (!host) {
        if (!runs(architecture)) return std::nullopt;
        if (auto it = impl_->startErrors.find(architecture); it != impl_->startErrors.end()) {
            error = it->second;
            return std::vector<PluginDescription>{};
        }
        host = HostProcess::start(architecture, error);
        if (!host) {
            impl_->startErrors[architecture] = error;
            return std::vector<PluginDescription>{};
        }
        host->onNote = [](const Message& m) {
            if (m.value("note", "") == "log") log((LogLevel)m.value("level", (int)LogLevel::Info), m.value("text", ""));
        };
    }
    Message reply;
    switch (host->call({{"op", "describe"}, {"path", absolute(pathToUtf8(file))}}, reply, kCreateTimeout)) {
        case HostProcess::Outcome::Ok: break;
        case HostProcess::Outcome::Ended:
            error = host->endReport();
            host.reset();
            return std::vector<PluginDescription>{};
        case HostProcess::Outcome::TimedOut:
            error = "stopped responding while being described";
            host.reset();
            return std::vector<PluginDescription>{};
    }
    error = reply.value("error", "");
    std::vector<PluginDescription> descs;
    for (auto& d : reply.value("plugins", Message::array())) descs.push_back(remote::descriptionFromMessage(d));
    return descs;
}

}  // namespace brack
