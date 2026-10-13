#pragma once
// A chain: plugins processed one after another in one process. An instrument and the effects on
// its main output, or, for the master, effects on the mix of every instrument. The engine
// processes chains side by side, a chain's plugins one after another.
//
// The bus is what flows down the chain: the instrument's main output, or the master's input, of a
// fixed number of channels. An effect takes the bus's first channels into its main input, as many
// as that has (silence past the bus), and replaces as many of them as its main output has; the
// rest pass on unchanged. A bypassed effect is not processed. Effects get no MIDI.

#include <memory>
#include <string>
#include <vector>

#include "plugin/plugin.h"

namespace brack {

class HostedChain {
public:
    virtual ~HostedChain() = default;

    // ---- host thread ----
    // The plugins, the instrument first if there is one.
    virtual size_t size() const = 0;
    virtual HostedPlugin& plugin(size_t i) = 0;
    virtual bool hasInstrument() const = 0;
    // Runs in a plugin host process rather than in this one.
    virtual bool separateProcess() const = 0;

    // An effect, in two steps so that the chain is not held up while the effect loads.
    // prepareEffect() makes it, outside the chain, activated if the chain is; the chain may be
    // processed meanwhile. Null, with `error`, if it does not load or is not an effect (no audio
    // input or output). One not yet inserted is replaced by the next. In a plugin host
    // (separateProcess()), this and the prepared effect's calls may come from another thread
    // while the host thread uses the chain.
    virtual HostedPlugin* prepareEffect(const std::string& pathUtf8, const std::string& pluginId,
                                        std::string& error) = 0;
    // Puts the prepared effect at `index` (counting the instrument; clamped to after it). Changing
    // the plugins (this, remove, move) is for while the chain is not processed. Null if there is
    // none, it crashed, or the chain's process is gone.
    virtual HostedPlugin* insertEffect(size_t index) = 0;
    virtual void remove(size_t index) = 0;  // an effect
    virtual void move(size_t from, size_t to) = 0;  // effects, among the effects
    // Any time; takes effect from the next block.
    virtual void setBypass(size_t index, bool bypass) = 0;

    // `busChannels`: the master's bus; a chain with an instrument has its main output's.
    virtual bool activate(double sampleRate, uint32_t maxFrames, uint32_t busChannels, std::string& error) = 0;
    virtual void deactivate() = 0;
    virtual bool isActive() const = 0;
    virtual void idle() = 0;  // about every 10 ms
    // Stop/start handshake with the audio thread, for every plugin of the chain: the chain stays
    // stopped until resumed (while it is not processed).
    virtual void requestStopProcessing() = 0;
    virtual bool stopProcessingAcknowledged() const = 0;
    virtual void resumeProcessing() = 0;
    // A plugin wants to be activated again (its ports changed).
    virtual bool restartPending() const = 0;
    // A plugin crashed: the chain is silent from then on.
    virtual bool crashed() const = 0;
    // Which plugin crashed; -1 if not known (the plugin host process just ended, say).
    virtual int crashedPlugin() const = 0;
    virtual std::string crashReport() const = 0;

    // ---- audio thread ----
    // MIDI for the instrument (see PluginInstance::appendMidi).
    virtual void appendMidi(InputEventList& list, uint16_t port, const uint8_t* data, uint32_t size,
                            uint32_t time) const = 0;
    // Processes `frames` frames, in two steps so that chains in other processes run side by side:
    // beginProcess() for every chain of the block, then endProcess() for each, which returns false
    // when the chain produced no audio. `in` and `input` (the master's bus channels) stay
    // untouched until then.
    virtual void beginProcess(uint32_t frames, uint64_t steadyTime, const InputEventList& in,
                              const float* const* input) = 0;
    virtual bool endProcess() = 0;
    // The instrument's output ports, the bus in place of its main one; the master's port 0 is the bus.
    virtual const float* outputChannel(uint32_t port, uint32_t channel) const = 0;
};

// A chain in this process: the engine's for plugins in process, and a plugin host's.
class LocalChain final : public HostedChain {
public:
    // With an initialised instrument, or without one (the master's).
    LocalChain(HostThread& host, PluginHostListener& listener, std::unique_ptr<PluginInstance> instrument);
    ~LocalChain() override;

    size_t size() const override { return plugins_.size(); }
    HostedPlugin& plugin(size_t i) override { return *plugins_[i].instance; }
    PluginInstance& instance(size_t i) { return *plugins_[i].instance; }
    bool hasInstrument() const override { return hasInstrument_; }
    bool separateProcess() const override { return false; }

    HostedPlugin* prepareEffect(const std::string& pathUtf8, const std::string& pluginId, std::string& error) override;
    PluginInstance* preparedInstance() { return prepared_.get(); }
    // The effect being prepared crashed: it is kept, never to go in the chain.
    bool preparedCrashed() const { return prepared_ && prepared_->crashed(); }
    PluginInstance* insertEffect(size_t index) override;
    void remove(size_t index) override;
    void move(size_t from, size_t to) override;
    void setBypass(size_t index, bool bypass) override;

    bool activate(double sampleRate, uint32_t maxFrames, uint32_t busChannels, std::string& error) override;
    void deactivate() override;
    bool isActive() const override { return active_; }
    void idle() override;
    void requestStopProcessing() override;
    bool stopProcessingAcknowledged() const override;
    void resumeProcessing() override;
    bool restartPending() const override;
    bool crashed() const override { return crashedPlugin() >= 0; }
    int crashedPlugin() const override;
    std::string crashReport() const override;

    void appendMidi(InputEventList& list, uint16_t port, const uint8_t* data, uint32_t size,
                    uint32_t time) const override;
    void beginProcess(uint32_t frames, uint64_t steadyTime, const InputEventList& in,
                      const float* const* input) override;
    bool endProcess() override;
    const float* outputChannel(uint32_t port, uint32_t channel) const override;
    uint32_t outputPorts() const;  // how many outputChannel() has
    uint32_t outputChannels(uint32_t port) const;
    uint32_t busChannels() const { return (uint32_t)busPtrs_.size(); }

private:
    struct Plugin {
        std::unique_ptr<PluginInstance> instance;
        std::unique_ptr<std::atomic<bool>> bypass = std::make_unique<std::atomic<bool>>(false);
    };
    size_t firstEffect() const { return hasInstrument_ ? 1 : 0; }
    bool activateEffect(PluginInstance& p, std::string& error);

    HostThread& host_;
    PluginHostListener& listener_;
    const bool hasInstrument_;
    std::vector<Plugin> plugins_;
    std::unique_ptr<PluginInstance> prepared_;  // see prepareEffect()

    bool active_ = false;
    std::atomic<bool> stopping_{false};  // asked to stop, until resumed or activated again
    double sampleRate_ = 0;
    uint32_t maxFrames_ = 0;
    uint32_t instrumentMain_ = 0;  // the instrument's main output port
    std::vector<std::vector<float>> bus_;
    std::vector<float*> busPtrs_;

    // Audio thread: the block beginProcess() was given, for endProcess().
    uint32_t pendingFrames_ = 0;
    uint64_t pendingTime_ = 0;
    const InputEventList* pendingIn_ = nullptr;
    const float* const* pendingInput_ = nullptr;
    InputEventList noEvents_{1, 0};
};

}  // namespace brack
