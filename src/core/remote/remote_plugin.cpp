// RemotePlugin, the HostedPlugin for a plugin running in a plugin host process, and
// RemoteDescriber, which describes plugin files in them. The same on every OS; the process itself
// is remote/host_process.h.
#include <atomic>
#include <chrono>
#include <cstring>
#include <map>
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

class RemotePlugin final : public HostedPlugin {
public:
    RemotePlugin(PluginHostListener& listener, std::string path, std::string pluginId, std::string architecture)
        : HostedPlugin(placeholder(path, architecture)), listener_(listener), pluginId_(std::move(pluginId)) {}

    ~RemotePlugin() override {
        if (!host_) return;
        destroying_ = true;
        if (!crashed()) {
            Message reply;
            host_->call({{"op", "destroy"}}, reply, 5s);
        }
    }

    bool init(std::string& error) override {
        host_ = HostProcess::start(desc_.architecture, error);
        if (!host_) return false;
        host_->onNote = [this](const Message& m) { onNote(m); };
        Message r;
        if (!call("create", {{"path", absolute(desc_.path)}, {"pluginId", pluginId_}}, r, kCreateTimeout, &error))
            return false;
        if (!r.value("ok", false)) {
            error = r.value("error", "the plugin did not load");
            return false;
        }
        const std::string path = desc_.path, architecture = desc_.architecture;
        desc_ = remote::descriptionFromMessage(r.at("description"));
        desc_.path = path;  // as given, like a plugin in this process
        desc_.architecture = architecture;
        hasGui_ = r.value("hasGui", false);
        remote::portsFromMessage(r.value("ports", Message::object()), notePorts_, audioIns_, audioOuts_);
        return true;
    }

    bool activate(double sampleRate, uint32_t maxFrames, std::string& error) override {
        if (active_) deactivate();
        Message r;
        if (!call("activate", {{"rate", sampleRate}, {"maxFrames", maxFrames}}, r, kActivateTimeout, &error)) return false;
        if (!r.value("ok", false)) {
            error = r.value("error", "");
            if (error.empty()) error = "activate failed (" + desc_.name + ")";
            return false;
        }
        remote::portsFromMessage(r.at("ports"), notePorts_, audioIns_, audioOuts_);
        latency_.store(r.value("latency", 0u), std::memory_order_relaxed);
        uint32_t channels = 0;
        for (auto& p : audioOuts_) channels += p.channels;
        float* base = host_->mapOutputs(r.value("outputs", Message()), (uint64_t)channels * maxFrames * sizeof(float));
        if (channels && !base) {
            Message ignored;
            call("deactivate", {}, ignored, kShortTimeout);
            error = "cannot map the plugin's outputs";
            return false;
        }
        outPtrs_.assign(audioOuts_.size(), {});
        for (size_t p = 0, ch = 0; p < audioOuts_.size(); ++p)
            for (uint32_t c = 0; c < audioOuts_[p].channels; ++c) outPtrs_[p].push_back(base + (ch++) * maxFrames);
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
        outPtrs_.clear();
        host_->unmapOutputs();
    }

    bool isActive() const override { return active_; }

    void idle() override {
        if (!crashed()) {
            host_->pollNotes();
            checkStateChanges();
            if (!host_->running()) end(Ending::Ended, "idle");
        }
        if (crashed() && guiOpen_) {
            guiOpen_ = false;
            listener_.pluginGuiClosed(*this);
        }
    }

    bool saveState(std::vector<uint8_t>& out) override {
        Message r;
        if (call("saveState", {}, r, kStateTimeout) && r.value("ok", false)) {
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

    bool loadState(const std::vector<uint8_t>& in) override {
        Message r;
        if (!call("loadState", {{"state", Message::binary(in)}}, r, kStateTimeout) || !r.value("ok", false)) return false;
        lastState_ = in;
        return true;
    }

    bool isGuiOpen() const override { return guiOpen_ && !crashed(); }  // its window went with the process

    bool openGui(std::string& error, void* parent) override {
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
        host_->allowForeground();
        Message r;
        if (!call("openGui", {{"parent", (uint64_t)(uintptr_t)parent}}, r, kEditorTimeout, &error)) return false;
        if (!r.value("ok", false)) {
            error = r.value("error", "the editor did not open");
            return false;
        }
        guiOpen_ = true;
        guiParent_ = parent;
        return true;
    }

    void closeGui() override {
        if (!guiOpen_) return;
        Message ignored;
        call("closeGui", {}, ignored, kShortTimeout);  // the host's "guiClosed" note follows
        if (guiOpen_) {  // it did not come: the host is gone
            guiOpen_ = false;
            listener_.pluginGuiClosed(*this);
        }
    }

    void requestStopProcessing() override {
        if (!active_) return;
        stopAck_.store(false, std::memory_order_release);
        stopRequested_.store(true, std::memory_order_release);
    }
    bool stopProcessingAcknowledged() const override { return stopAck_.load(std::memory_order_acquire) || crashed(); }

    void setDisplayName(std::string name) override {
        Message ignored;
        call("setName", {{"name", name}}, ignored, kShortTimeout);
    }

    bool restartPending() const override { return restartPending_; }
    bool processFailed() const override { return processFailed_ || crashed(); }
    bool crashed() const override { return ending_.load(std::memory_order_acquire) != Ending::None; }
    std::string crashReport() const override {
        if (!crashed()) return {};
        if (report_.empty()) {
            // A host that recorded a crash and then hung (in the allocator's lock, say) crashed.
            if (ending_ == Ending::Hung && !host_->control().crashed)
                report_ = std::string("stopped responding in ") + endedDuring_;
            else
                report_ = host_->endReport();
        }
        return report_;
    }
    bool separateProcess() const override { return true; }

    // ---- audio thread ----
    void appendMidi(InputEventList& list, uint16_t port, const uint8_t* data, uint32_t size,
                    uint32_t time) const override {
        if (port < notePorts_.size()) list.pushMidi(port, data, size, time);  // the host converts it
    }

    void beginProcess(uint32_t frames, uint64_t steadyTime, const InputEventList& in) override {
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
        }
        host_->signalBlock();
        kicked_ = true;
    }

    bool endProcess() override {
        if (!kicked_) return false;
        kicked_ = false;
        const HostProcess::Outcome o = host_->waitBlock(kProcessTimeoutMs);
        if (o == HostProcess::Outcome::Ended) {
            end(Ending::Ended, "process");
        } else if (o == HostProcess::Outcome::TimedOut) {
            end(Ending::Hung, "process");
        } else {
            checkStateChanges();
            switch ((BlockResult)host_->control().result) {
                case BlockResult::Audio: return true;
                case BlockResult::Stopped: stopAck_.store(true, std::memory_order_release); break;
                case BlockResult::Failed: processFailed_ = true; break;
            }
        }
        return false;
    }

    const float* outputChannel(uint32_t port, uint32_t channel) const override {
        if (port >= outPtrs_.size() || channel >= outPtrs_[port].size()) return nullptr;
        return outPtrs_[port][channel];
    }

private:
    enum class Ending : uint8_t { None, Ended, Hung };

    static PluginDescription placeholder(const std::string& path, const std::string& architecture) {
        PluginDescription d;
        d.path = path;
        d.architecture = architecture;
        return d;
    }

    // Any thread, no allocation: the host is gone, or hung (and is ended now). The first call
    // decides; what it says is in place before crashed() is true.
    void end(Ending how, const char* during) {
        if (endClaimed_.exchange(true, std::memory_order_acq_rel)) return;
        endedDuring_ = during;
        ending_.store(how, std::memory_order_release);
        if (how == Ending::Hung) host_->terminate();
    }

    // A request and its reply; false once the host is gone (error: why).
    bool call(const char* op, Message request, Message& reply, std::chrono::milliseconds timeout,
              std::string* error = nullptr) {
        if (!crashed()) {
            request["op"] = op;
            switch (host_->call(request, reply, timeout)) {
                case HostProcess::Outcome::Ok: checkStateChanges(); return true;
                case HostProcess::Outcome::Ended: end(Ending::Ended, op); break;
                case HostProcess::Outcome::TimedOut: end(Ending::Hung, op); break;
            }
        }
        if (error) *error = crashReport();
        return false;
    }

    void onNote(const Message& m) {
        const std::string note = m.value("note", "");
        if (note == "log") {
            log((LogLevel)m.value("level", (int)LogLevel::Info), m.value("text", ""));
            return;
        }
        if (destroying_) return;
        if (note == "guiClosed") {
            if (guiOpen_) {
                guiOpen_ = false;
                listener_.pluginGuiClosed(*this);
            }
        } else if (note == "resized") {
            listener_.pluginEditorResized(*this, m.value("width", 0u), m.value("height", 0u));
        } else if (note == "restart") {
            restartPending_ = true;
        }
    }

    // Audio or host thread: tells the listener once for any number of changes the host counted.
    void checkStateChanges() {
        const uint32_t now = std::atomic_ref(host_->control().stateChanges).load(std::memory_order_acquire);
        uint32_t seen = stateSeen_.load(std::memory_order_relaxed);
        if (now != seen && stateSeen_.compare_exchange_strong(seen, now) && !destroying_)
            listener_.pluginStateChanged(*this);
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
    std::string pluginId_;
    std::unique_ptr<HostProcess> host_;
    bool destroying_ = false;

    std::atomic<bool> active_{false};
    std::atomic<bool> stopRequested_{false}, stopAck_{true}, processFailed_{false};
    bool restartPending_ = false;
    bool guiOpen_ = false;
    void* guiParent_ = nullptr;
    std::optional<std::vector<uint8_t>> lastState_;  // last saved or loaded

    std::atomic<uint32_t> stateSeen_{0};
    std::atomic<bool> endClaimed_{false};
    std::atomic<Ending> ending_{Ending::None};
    const char* endedDuring_ = "";  // written before ending_
    mutable std::string report_;    // host thread

    std::vector<std::vector<float*>> outPtrs_;  // into the shared outputs
    bool kicked_ = false;                       // audio thread
};

}  // namespace

std::unique_ptr<HostedPlugin> createRemotePlugin(PluginHostListener& listener, const std::string& pathUtf8,
                                                 const std::string& pluginId, const std::string& architecture) {
    return std::make_unique<RemotePlugin>(listener, pathUtf8, pluginId, architecture);
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
