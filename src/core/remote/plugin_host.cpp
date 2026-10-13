// The plugin host process: one chain (or a scan's descriptions) for the Brack process that started
// it. See remote/host_protocol.h; the link to Brack is remote/brack_link.h.
#include <atomic>
#include <cstring>
#include <optional>
#include <string>
#include <thread>

#include "host_thread.h"
#include "plugin/chain.h"
#include "remote/brack_link.h"
#include "remote/plugin_host.h"
#include "util/common.h"

namespace brack {

using remote::BlockCommand;
using remote::BlockResult;
using remote::ControlBlock;
using remote::Message;

namespace {

Message pluginMessage(const PluginInstance& p) {
    return {{"description", remote::toMessage(p.description())},
            {"hasGui", p.hasGui()},
            {"ports", remote::portsToMessage(p.notePorts(), p.audioInputs(), p.audioOutputs())},
            {"active", p.isActive()},
            {"latency", p.latency()}};
}

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
                endIfCrashed();
                endIfPreparedCrashed();
            });
            reply["reply"] = op;
            if (!link_.send(reply) || op == "destroy") break;
        }
        link_.end();  // Brack is gone, or done with this process
    }

private:
    // ---- PluginHostListener (host thread unless noted) ----
    void pluginGuiClosed(HostedPlugin& p) override {
        if (auto i = indexOf(p)) link_.send({{"note", "guiClosed"}, {"plugin", *i}});
    }
    void pluginEditorResized(HostedPlugin& p, uint32_t w, uint32_t h) override {
        if (auto i = indexOf(p)) link_.send({{"note", "resized"}, {"plugin", *i}, {"width", w}, {"height", h}});
    }
    void pluginStateChanged(HostedPlugin&) override {  // any thread
        std::atomic_ref(control_.stateChanges).fetch_add(1, std::memory_order_release);
    }

    // Where `p` is in the chain; nothing once it has left it (a plugin being destroyed).
    std::optional<size_t> indexOf(const HostedPlugin& p) const {
        for (size_t i = 0; chain_ && i < chain_->size(); ++i)
            if (&chain_->plugin(i) == &p) return i;
        return std::nullopt;
    }

    // Any thread: a plugin crashed, so this process ends, saying which.
    void endIfCrashed() {
        if (!chain_ || !chain_->crashed()) return;
        control_.crashedPlugin = (uint32_t)(chain_->crashedPlugin() + 1);
        link_.endAfterCrash(chain_->crashReport());
    }
    // Host thread: likewise for the effect being prepared, which has no place in the chain
    // (crashedPlugin stays 0: not known).
    void endIfPreparedCrashed() {
        if (chain_ && chain_->preparedCrashed()) link_.endAfterCrash(chain_->preparedInstance()->crashReport());
    }

    Message handle(const std::string& op, const Message& m) {
        std::string error;
        if (op == "create") {
            if (chain_) return {{"ok", false}, {"error", "this plugin host already has a chain"}};
            std::unique_ptr<PluginInstance> instrument;
            if (m.contains("path")) {
                instrument =
                    createPluginInstance(host_, *this, m.value("path", ""), m.value("pluginId", ""), true, error);
                if (!instrument || !instrument->init(error)) {
                    if (instrument && instrument->crashed()) link_.endAfterCrash(error);
                    return {{"ok", false}, {"error", error}};
                }
            }
            chain_ = std::make_unique<LocalChain>(host_, *this, std::move(instrument));
            std::thread([this] { audioLoop(); }).detach();  // not for a scan: no real-time thread to ask for
            if (!chain_->hasInstrument()) return {{"ok", true}};
            return {{"ok", true}, {"plugin", pluginMessage(chain_->instance(0))}};
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
        if (!chain_) return {{"ok", false}, {"error", "no chain"}};
        if (op == "prepareEffect") {  // while the chain is processed
            if (!chain_->prepareEffect(m.value("path", ""), m.value("pluginId", ""), error))
                return {{"ok", false}, {"error", error}};
            return {{"ok", true}, {"plugin", pluginMessage(*chain_->preparedInstance())}};
        }
        if (op == "insertEffect") {
            PluginInstance* p = chain_->insertEffect(m.value("index", (size_t)0));
            if (!p) return {{"ok", false}};
            return {{"ok", true}, {"plugin", pluginMessage(*p)}};
        }
        if (op == "remove") {
            chain_->remove(m.value("index", (size_t)0));
            return {{"ok", true}};
        }
        if (op == "move") {
            chain_->move(m.value("from", (size_t)0), m.value("to", (size_t)0));
            return {{"ok", true}};
        }
        if (op == "bypass") {
            chain_->setBypass(m.value("index", (size_t)0), m.value("bypass", false));
            return {{"ok", true}};
        }
        if (op == "activate") return activate(m.value("rate", 0.0), m.value("maxFrames", 0u), m.value("busChannels", 0u));
        if (op == "deactivate") {
            chain_->deactivate();
            return {{"ok", true}};
        }
        if (op == "destroy") {
            host_.removeTimer(idleTimer_);
            chain_.reset();
            return {{"ok", true}};
        }
        // The plugin the request is about: one of the chain's, or the prepared effect.
        const size_t i = m.value("plugin", (size_t)0);
        PluginInstance* target = m.value("prepared", false) ? chain_->preparedInstance()
                                 : i < chain_->size()       ? &chain_->instance(i)
                                                            : nullptr;
        if (!target) return {{"ok", false}, {"error", "no such plugin in the chain"}};
        PluginInstance& p = *target;
        if (op == "saveState") {
            std::vector<uint8_t> state;
            if (!p.saveState(state)) return {{"ok", false}};
            return {{"ok", true}, {"state", Message::binary(std::move(state))}};
        }
        if (op == "loadState") {
            const auto& bin = m.at("state").get_binary();
            return {{"ok", p.loadState(std::vector<uint8_t>(bin.begin(), bin.end()))}};
        }
        if (op == "openGui") {
            void* parent = (void*)(uintptr_t)m.value("parent", (uint64_t)0);
            const bool ok = p.openGui(error, parent);
            return {{"ok", ok}, {"error", error}};
        }
        if (op == "closeGui") {
            p.closeGui();
            return {{"ok", true}};
        }
        if (op == "setName") {
            p.setDisplayName(m.value("name", ""));
            return {{"ok", true}};
        }
        return {{"ok", false}, {"error", "unknown request: " + op}};
    }

    Message activate(double rate, uint32_t maxFrames, uint32_t busChannels) {
        std::string error;
        if (!chain_->activate(rate, maxFrames, busChannels, error)) return {{"ok", false}, {"error", error}};
        // The master's bus in, then the outputs, port after port, channel after channel, in shared
        // memory made anew at each activation: Brack maps the one it was given until the next.
        inputChannels_ = chain_->hasInstrument() ? 0 : chain_->busChannels();
        channels_.clear();
        Message outputPorts = Message::array();
        for (uint32_t p = 0; p < chain_->outputPorts(); ++p) {
            outputPorts.push_back(chain_->outputChannels(p));
            for (uint32_t c = 0; c < chain_->outputChannels(p); ++c) channels_.push_back({p, c});
        }
        maxFrames_ = maxFrames;
        rate_ = rate;
        const uint64_t bytes = (uint64_t)(inputChannels_ + channels_.size()) * maxFrames * sizeof(float);
        Message ref;
        channelsView_ = bytes ? link_.createOutputs(bytes, ref) : nullptr;
        if (bytes && !channelsView_) {
            chain_->deactivate();
            return {{"ok", false}, {"error", "cannot share the chain's channels"}};
        }
        inputPtrs_.clear();
        for (uint32_t c = 0; c < inputChannels_; ++c) inputPtrs_.push_back(channelsView_ + c * maxFrames);
        restartSent_ = false;
        Message plugins = Message::array();
        for (size_t i = 0; i < chain_->size(); ++i) plugins.push_back(pluginMessage(chain_->instance(i)));
        return {{"ok", true},
                {"plugins", plugins},
                {"inputChannels", inputChannels_},
                {"outputPorts", outputPorts},
                {"channels", ref}};
    }

    void idle() {
        if (!chain_) return;
        chain_->idle();
        endIfCrashed();
        endIfPreparedCrashed();
        if (chain_->restartPending() && !restartSent_) {
            restartSent_ = true;
            link_.send({{"note", "restart"}});
        }
        // Which plugins failed to start or to process (an effect's failure does not fail the
        // block), whenever that changes.
        std::vector<size_t> failed;
        for (size_t i = 0; i < chain_->size(); ++i)
            if (chain_->instance(i).processFailed()) failed.push_back(i);
        if (failed != failedSent_) {
            failedSent_ = failed;
            link_.send({{"note", "failed"}, {"plugins", failed}});
        }
    }

    // The chain's audio thread. Brack signals "process" only while the chain is active, and waits
    // for "done" before anything else happens to it.
    void audioLoop() {
        link_.protectThisThread();
        setCurrentThreadIsAudio(true);
        InputEventList events;
        std::pair<double, uint32_t> raisedFor;
        bool stopped = false;
        while (link_.waitForBlock()) {
            ControlBlock& c = control_;
            if (raisedFor != std::pair(rate_, maxFrames_)) {
                raisedFor = {rate_, maxFrames_};
                link_.raiseAudioThreadPriority(rate_, maxFrames_);
            }
            events.clear();
            if (c.command == (uint32_t)BlockCommand::Stop) {
                chain_->requestStopProcessing();
                chain_->beginProcess(0, c.steadyTime, events, nullptr);
                chain_->endProcess();
                stopped = true;
                c.result = (uint32_t)BlockResult::Stopped;
                c.processNs = 0;
            } else {
                if (stopped) chain_->resumeProcessing();
                stopped = false;
                const uint32_t n = std::min(c.eventCount, remote::kMaxEvents);
                for (uint32_t i = 0; i < n; ++i) {
                    const remote::MidiEvent& e = c.events[i];
                    const uint8_t* data = e.size <= sizeof e.bytes ? e.bytes : c.sysex + e.offset;
                    if (e.size > sizeof e.bytes && (uint64_t)e.offset + e.size > remote::kSysexBytes) continue;
                    chain_->appendMidi(events, e.port, data, e.size, e.time);
                }
                chain_->beginProcess(c.frames, c.steadyTime, events, inputPtrs_.data());
                const bool ok = chain_->endProcess();
                endIfCrashed();
                if (ok) {
                    const uint32_t frames = std::min(c.frames, maxFrames_);
                    float* outputs = channelsView_ + (size_t)inputChannels_ * maxFrames_;
                    for (size_t i = 0; i < channels_.size(); ++i)
                        if (const float* src = chain_->outputChannel(channels_[i].first, channels_[i].second))
                            std::memcpy(outputs + i * maxFrames_, src, frames * sizeof(float));
                }
                c.result = (uint32_t)(ok ? BlockResult::Audio : BlockResult::Failed);
                c.processNs = chain_->processNs();
            }
            link_.blockDone();
        }
    }

    BrackLink& link_;
    ControlBlock& control_;
    HostThread host_;
    uint32_t idleTimer_ = 0;
    std::unique_ptr<LocalChain> chain_;
    bool restartSent_ = false;  // host thread
    std::vector<size_t> failedSent_;  // host thread: the last "failed" note's

    // Set on the host thread while the audio thread waits (see audioLoop()).
    float* channelsView_ = nullptr;
    uint32_t maxFrames_ = 0, inputChannels_ = 0;
    double rate_ = 0;
    std::vector<const float*> inputPtrs_;
    std::vector<std::pair<uint32_t, uint32_t>> channels_;  // the outputs': port, channel
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
