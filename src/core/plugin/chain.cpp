#include "plugin/chain.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <utility>

#include "util/common.h"

namespace brack {

LocalChain::LocalChain(HostThread& host, PluginHostListener& listener, std::unique_ptr<PluginInstance> instrument)
    : host_(host), listener_(listener), hasInstrument_(instrument != nullptr) {
    if (instrument) plugins_.push_back({std::move(instrument)});
}

LocalChain::~LocalChain() {
    prepared_.reset();
    while (!plugins_.empty()) plugins_.pop_back();  // the effects before the instrument
}

HostedPlugin* LocalChain::prepareEffect(const std::string& pathUtf8, const std::string& pluginId,
                                        std::string& error) {
    prepared_.reset();
    auto effect = createPluginInstance(host_, listener_, pathUtf8, pluginId, error);
    if (!effect) return nullptr;
    bool ok = effect->init(error);
    if (ok && (effect->audioInputs().empty() || effect->audioOutputs().empty())) {
        error = effect->description().name + " is not an effect: it has no audio " +
                (effect->audioInputs().empty() ? "input" : "output");
        ok = false;
    }
    if (ok && active_) ok = activateEffect(*effect, error);
    if (ok || effect->crashed()) prepared_ = std::move(effect);  // a crashed one for preparedCrashed()
    return ok ? prepared_.get() : nullptr;
}

PluginInstance* LocalChain::insertEffect(size_t index) {
    if (!prepared_ || prepared_->crashed()) return nullptr;
    // Activated as the chain is now, should that have changed since the effect was made.
    if (std::string err; active_ && !prepared_->isActive() && !activateEffect(*prepared_, err))
        logError(prepared_->displayName() + ": " + err);
    if (!active_) prepared_->deactivate();
    index = std::clamp(index, firstEffect(), plugins_.size());
    plugins_.insert(plugins_.begin() + (ptrdiff_t)index, {std::move(prepared_)});
    return plugins_[index].instance.get();
}

void LocalChain::remove(size_t index) {
    if (index < firstEffect() || index >= plugins_.size()) return;
    instance(index).closeGui();  // while the listener can still find it in the chain
    plugins_.erase(plugins_.begin() + (ptrdiff_t)index);
}

void LocalChain::move(size_t from, size_t to) {
    if (from < firstEffect() || from >= plugins_.size()) return;
    to = std::clamp(to, firstEffect(), plugins_.size() - 1);
    Plugin p = std::move(plugins_[from]);
    plugins_.erase(plugins_.begin() + (ptrdiff_t)from);
    plugins_.insert(plugins_.begin() + (ptrdiff_t)to, std::move(p));
}

void LocalChain::setBypass(size_t index, bool bypass) {
    if (index >= firstEffect() && index < plugins_.size()) plugins_[index].bypass->store(bypass);
}

bool LocalChain::activateEffect(PluginInstance& p, std::string& error) {
    return p.activate(sampleRate_, maxFrames_, error);
}

bool LocalChain::activate(double sampleRate, uint32_t maxFrames, uint32_t busChannels, std::string& error) {
    if (active_) deactivate();
    sampleRate_ = sampleRate;
    maxFrames_ = maxFrames;
    stopping_ = false;
    if (hasInstrument_) {
        PluginInstance& instrument = instance(0);
        if (!instrument.activate(sampleRate, maxFrames, error)) return false;
        const auto& outs = instrument.audioOutputs();
        instrumentMain_ = outs.empty() ? 0 : mainPort(outs);
        busChannels = outs.empty() ? 0 : outs[instrumentMain_].channels;
    }
    bus_.assign(busChannels, std::vector<float>(maxFrames, 0.0f));
    busPtrs_.clear();
    for (auto& b : bus_) busPtrs_.push_back(b.data());
    // An effect that does not activate is left out, and the chain goes on without it.
    for (size_t i = firstEffect(); i < plugins_.size(); ++i)
        if (std::string err; !activateEffect(instance(i), err)) logError(instance(i).displayName() + ": " + err);
    active_ = true;
    return true;
}

void LocalChain::deactivate() {
    active_ = false;
    for (auto& p : plugins_) p.instance->deactivate();
}

void LocalChain::idle() {
    for (auto& p : plugins_) p.instance->idle();
}

void LocalChain::requestStopProcessing() {
    for (auto& p : plugins_) p.instance->requestStopProcessing();
    stopping_.store(true, std::memory_order_release);
}

bool LocalChain::stopProcessingAcknowledged() const {
    for (auto& p : plugins_)
        if (p.instance->isActive() && !p.instance->stopProcessingAcknowledged()) return false;
    return true;
}

void LocalChain::resumeProcessing() {
    for (auto& p : plugins_) p.instance->resumeProcessing();
    stopping_.store(false, std::memory_order_release);
}

bool LocalChain::restartPending() const {
    for (auto& p : plugins_)
        if (p.instance->restartPending()) return true;
    return false;
}

int LocalChain::crashedPlugin() const {
    for (size_t i = 0; i < plugins_.size(); ++i)
        if (plugins_[i].instance->crashed()) return (int)i;
    return -1;
}

std::string LocalChain::crashReport() const {
    const int i = crashedPlugin();
    return i < 0 ? std::string() : plugins_[(size_t)i].instance->crashReport();
}

bool LocalChain::appendMidi(InputEventList& list, uint16_t port, const uint8_t* data, uint32_t size,
                            uint32_t time) const {
    return hasInstrument_ && plugins_[0].instance->appendMidi(list, port, data, size, time);
}

void LocalChain::beginProcess(uint32_t frames, uint64_t steadyTime, const InputEventList& in,
                              const float* const* input) {
    pendingFrames_ = frames;
    pendingTime_ = steadyTime;
    pendingIn_ = &in;
    pendingInput_ = input;
}

bool LocalChain::endProcess() {
    const auto start = std::chrono::steady_clock::now();
    const bool ok = process();
    const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count();
    processNs_ = (uint32_t)std::min<int64_t>(ns, UINT32_MAX);
    return ok;
}

bool LocalChain::process() {
    const InputEventList* in = std::exchange(pendingIn_, nullptr);
    if (!in || !active_) return false;
    const uint32_t frames = std::min(pendingFrames_, maxFrames_);
    const uint64_t time = pendingTime_;
    if (stopping_.load(std::memory_order_acquire)) {
        for (auto& p : plugins_) p.instance->process(0, time, noEvents_);  // stops it, if asked to
        return false;
    }
    if (crashed()) return false;

    const uint32_t busChannels = (uint32_t)bus_.size();
    if (hasInstrument_) {
        PluginInstance& instrument = instance(0);
        if (!instrument.process(frames, time, *in)) return false;
        for (uint32_t c = 0; c < busChannels; ++c)
            if (const float* src = instrument.outputChannel(instrumentMain_, c))
                std::memcpy(bus_[c].data(), src, frames * sizeof(float));
    } else {
        for (uint32_t c = 0; c < busChannels; ++c)
            std::memcpy(bus_[c].data(), pendingInput_[c], frames * sizeof(float));
    }
    for (size_t i = firstEffect(); i < plugins_.size(); ++i) {
        if (plugins_[i].bypass->load(std::memory_order_relaxed)) continue;
        PluginInstance& effect = instance(i);
        if (!effect.isActive()) continue;
        effect.setMainInput(busPtrs_.data(), busChannels);
        if (!effect.process(frames, time, noEvents_)) {
            if (effect.crashed()) return false;
            continue;  // failed: as if bypassed
        }
        const auto& outs = effect.audioOutputs();
        if (outs.empty()) continue;  // none since a restart: as if bypassed
        const uint32_t port = mainPort(outs);
        for (uint32_t c = 0; c < std::min(outs[port].channels, busChannels); ++c)
            std::memcpy(bus_[c].data(), effect.outputChannel(port, c), frames * sizeof(float));
    }
    return true;
}

const float* LocalChain::outputChannel(uint32_t port, uint32_t channel) const {
    if (port == (hasInstrument_ ? instrumentMain_ : 0) && channel < bus_.size()) return bus_[channel].data();
    return hasInstrument_ ? plugins_[0].instance->outputChannel(port, channel) : nullptr;
}

uint32_t LocalChain::outputPorts() const {
    return hasInstrument_ ? (uint32_t)plugins_[0].instance->audioOutputs().size() : 1;
}

uint32_t LocalChain::outputChannels(uint32_t port) const {
    if (!hasInstrument_) return port == 0 ? (uint32_t)bus_.size() : 0;
    const auto& outs = plugins_[0].instance->audioOutputs();
    return port < outs.size() ? outs[port].channels : 0;
}

}  // namespace brack
