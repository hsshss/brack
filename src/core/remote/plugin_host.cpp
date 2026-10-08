// The plugin host process: one plugin instance (or a scan's descriptions) for the Brack process
// that started it. See remote/host_protocol.h; the link to Brack is remote/brack_link.h.
#include <atomic>
#include <cstring>
#include <string>
#include <thread>

#include "host_thread.h"
#include "plugin/plugin.h"
#include "remote/brack_link.h"
#include "remote/plugin_host.h"
#include "util/common.h"

namespace brack {

using remote::BlockCommand;
using remote::BlockResult;
using remote::ControlBlock;
using remote::Message;

namespace {

class Host final : public PluginHostListener {
public:
    explicit Host(BrackLink& link) : link_(link), control_(link.control()) {}

    int run() {
        setLogSink([this](LogLevel level, const std::string& text) {
            link_.send({{"note", "log"}, {"level", (int)level}, {"text", text}});
        });
        host_.invoke([this] { link_.protectThisThread(); });
        idleTimer_ = host_.addTimer(10, [this] { idle(); });
        Message request;
        while (link_.receive(request)) {
            const std::string op = request.value("op", "");
            Message reply;
            host_.invoke([&] {
                reply = handle(op, request);
                if (inst_ && inst_->crashed()) link_.endAfterCrash(inst_->crashReport());
            });
            reply["reply"] = op;
            if (!link_.send(reply) || op == "destroy") break;
        }
        link_.end();  // Brack is gone, or done with this process
    }

private:
    // ---- PluginHostListener (host thread unless noted) ----
    void pluginGuiClosed(HostedPlugin&) override { link_.send({{"note", "guiClosed"}}); }
    void pluginEditorResized(HostedPlugin&, uint32_t w, uint32_t h) override {
        link_.send({{"note", "resized"}, {"width", w}, {"height", h}});
    }
    void pluginStateChanged(HostedPlugin&) override {  // any thread
        std::atomic_ref(control_.stateChanges).fetch_add(1, std::memory_order_release);
    }

    Message handle(const std::string& op, const Message& m) {
        std::string error;
        if (op == "create") {
            if (inst_) return {{"ok", false}, {"error", "this plugin host already has a plugin"}};
            inst_ = createPluginInstance(host_, *this, m.value("path", ""), m.value("pluginId", ""), error);
            if (!inst_ || !inst_->init(error)) {
                if (inst_ && inst_->crashed()) link_.endAfterCrash(error);
                inst_.reset();
                return {{"ok", false}, {"error", error}};
            }
            std::thread([this] { audioLoop(); }).detach();  // not for a scan: no real-time thread to ask for
            return {{"ok", true},
                    {"description", remote::toMessage(inst_->description())},
                    {"hasGui", inst_->hasGui()},
                    {"ports", remote::portsToMessage(inst_->notePorts(), inst_->audioInputs(), inst_->audioOutputs())}};
        }
        if (op == "describe") {
            auto descs = describePluginFile(pathFromUtf8(m.value("path", "")), error);
            Message list = Message::array();
            for (auto& d : descs) {
                d.architecture = buildArchitecture();
                list.push_back(remote::toMessage(d));
            }
            return {{"ok", true}, {"plugins", list}, {"error", error}};
        }
        if (!inst_) return {{"ok", false}, {"error", "no plugin"}};
        if (op == "activate") return activate(m.value("rate", 0.0), m.value("maxFrames", 0u));
        if (op == "deactivate") {
            inst_->deactivate();
            return {{"ok", true}};
        }
        if (op == "saveState") {
            std::vector<uint8_t> state;
            if (!inst_->saveState(state)) return {{"ok", false}};
            return {{"ok", true}, {"state", Message::binary(std::move(state))}};
        }
        if (op == "loadState") {
            const auto& bin = m.at("state").get_binary();
            return {{"ok", inst_->loadState(std::vector<uint8_t>(bin.begin(), bin.end()))}};
        }
        if (op == "openGui") {
            void* parent = (void*)(uintptr_t)m.value("parent", (uint64_t)0);
            const bool ok = inst_->openGui(error, parent);
            return {{"ok", ok}, {"error", error}};
        }
        if (op == "closeGui") {
            inst_->closeGui();
            return {{"ok", true}};
        }
        if (op == "setName") {
            inst_->setDisplayName(m.value("name", ""));
            return {{"ok", true}};
        }
        if (op == "destroy") {
            host_.removeTimer(idleTimer_);
            inst_.reset();
            return {{"ok", true}};
        }
        return {{"ok", false}, {"error", "unknown request: " + op}};
    }

    Message activate(double rate, uint32_t maxFrames) {
        std::string error;
        if (!inst_->activate(rate, maxFrames, error)) return {{"ok", false}, {"error", error}};
        // The outputs, port after port, channel after channel, in shared memory made anew at each
        // activation: Brack maps the one it was given until the next.
        channels_.clear();
        for (uint32_t p = 0; p < inst_->audioOutputs().size(); ++p)
            for (uint32_t c = 0; c < inst_->audioOutputs()[p].channels; ++c) channels_.push_back({p, c});
        maxFrames_ = maxFrames;
        rate_ = rate;
        const uint64_t bytes = (uint64_t)channels_.size() * maxFrames * sizeof(float);
        Message outputs;
        outputsView_ = bytes ? link_.createOutputs(bytes, outputs) : nullptr;
        if (bytes && !outputsView_) {
            inst_->deactivate();
            return {{"ok", false}, {"error", "cannot share the plugin's outputs"}};
        }
        restartSent_ = false;
        return {{"ok", true},
                {"latency", inst_->latency()},
                {"ports", remote::portsToMessage(inst_->notePorts(), inst_->audioInputs(), inst_->audioOutputs())},
                {"outputs", outputs}};
    }

    void idle() {
        if (!inst_) return;
        inst_->idle();
        if (inst_->crashed()) link_.endAfterCrash(inst_->crashReport());
        if (inst_->restartPending() && !restartSent_) {
            restartSent_ = true;
            link_.send({{"note", "restart"}});
        }
    }

    // The plugin's audio thread. Brack signals "process" only while the plugin is active, and
    // waits for "done" before anything else happens to it.
    void audioLoop() {
        link_.protectThisThread();
        setCurrentThreadIsAudio(true);
        InputEventList events;
        std::pair<double, uint32_t> raisedFor;
        while (link_.waitForBlock()) {
            ControlBlock& c = control_;
            if (raisedFor != std::pair(rate_, maxFrames_)) {
                raisedFor = {rate_, maxFrames_};
                link_.raiseAudioThreadPriority(rate_, maxFrames_);
            }
            events.clear();
            if (c.command == (uint32_t)BlockCommand::Stop) {
                inst_->requestStopProcessing();
                inst_->process(0, c.steadyTime, events);
                c.result = (uint32_t)BlockResult::Stopped;
            } else {
                const uint32_t n = std::min(c.eventCount, remote::kMaxEvents);
                for (uint32_t i = 0; i < n; ++i) {
                    const remote::MidiEvent& e = c.events[i];
                    const uint8_t* data = e.size <= sizeof e.bytes ? e.bytes : c.sysex + e.offset;
                    if (e.size > sizeof e.bytes && (uint64_t)e.offset + e.size > remote::kSysexBytes) continue;
                    inst_->appendMidi(events, e.port, data, e.size, e.time);
                }
                const bool ok = inst_->process(c.frames, c.steadyTime, events);
                if (inst_->crashed()) link_.endAfterCrash(inst_->crashReport());
                if (ok) {
                    const uint32_t frames = std::min(c.frames, maxFrames_);
                    for (size_t i = 0; i < channels_.size(); ++i)
                        if (const float* src = inst_->outputChannel(channels_[i].first, channels_[i].second))
                            std::memcpy(outputsView_ + i * maxFrames_, src, frames * sizeof(float));
                }
                c.result = (uint32_t)(ok ? BlockResult::Audio : BlockResult::Failed);
            }
            link_.blockDone();
        }
    }

    BrackLink& link_;
    ControlBlock& control_;
    HostThread host_;
    uint32_t idleTimer_ = 0;
    std::unique_ptr<PluginInstance> inst_;
    bool restartSent_ = false;  // host thread

    // Set on the host thread while the audio thread waits (see audioLoop()).
    float* outputsView_ = nullptr;
    uint32_t maxFrames_ = 0;
    double rate_ = 0;
    std::vector<std::pair<uint32_t, uint32_t>> channels_;  // port, channel
};

}  // namespace

int runPluginHost(const std::vector<std::string>& args) {
    int exitCode = 0;
    auto link = BrackLink::open(args, exitCode);
    if (!link) return exitCode;
    link->recordUncaughtCrashes();
    Host host(*link);
    return host.run();
}

}  // namespace brack
