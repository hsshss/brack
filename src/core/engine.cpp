#include "engine.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstring>
#include <fstream>
#include <set>
#include <sstream>
#include <thread>

#include "engine_internal.h"
#include "host_thread.h"
#include "midi/midi_input.h"
#include "plugin/plugin_files.h"
#include "remote/plugin_host.h"
#include "util/common.h"
#include "util/midi_queue.h"

namespace brack {

namespace {
int64_t steadyNowNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}
}  // namespace

const char* midiSourceKindName(MidiSourceKind k) {
    switch (k) {
        case MidiSourceKind::Hardware: return "hardware";
        case MidiSourceKind::Virtual: return "virtual";
        case MidiSourceKind::Api: return "api";
    }
    return "api";
}

bool parseMidiSourceKind(const std::string& s, MidiSourceKind& out) {
    if (s == "hardware") out = MidiSourceKind::Hardware;
    else if (s == "virtual") out = MidiSourceKind::Virtual;
    else if (s == "api") out = MidiSourceKind::Api;
    else return false;
    return true;
}

namespace {
bool isValidMessage(const uint8_t* d, uint32_t n) {
    if (!d || n == 0 || d[0] < 0x80) return false;
    if (d[0] == 0xF0) return n >= 2 && d[n - 1] == 0xF7;
    return n <= 3;
}
}  // namespace

Engine::Engine() : host_(std::make_unique<HostThread>()) {
    scratch_.resize(1 << 16);
    idleTimer_ = host_->addTimer(10, [this] { onIdle(); });
}

Engine::~Engine() {
    host_->removeTimer(idleTimer_);
    host_->invoke([this] {
        eventNotify_ = nullptr;
        stopRunning();
        {
            std::unique_lock lock(injectMutex_);
            injectPlugins_.clear();
            injectSources_.clear();
        }
        // Out of the lists before they go: closing an editor looks its plugin up in plugins_.
        auto plugins = std::move(plugins_);
        auto sources = std::move(sources_);
        plugins.clear();
        sources.clear();
    });
}

// ---------------------------------------------------------------------------
// transport

EngineConfig Engine::config() const {
    EngineConfig c;
    host_->invoke([&] { c = config_; });
    return c;
}

bool Engine::setConfig(const EngineConfig& cfg, std::string& error) {
    bool ok = true;
    host_->invoke([&] {
        EngineMode prev = mode_;
        uint32_t prevRate = outRate_, prevCh = outChannels_, prevFrames = periodFrames_;
        stopRunning();
        const bool hostingChanged = cfg.pluginsInProcess != config_.pluginsInProcess;
        config_ = cfg;
        if (hostingChanged)
            for (auto& p : plugins_)
                if (std::string err; !reloadSlot(*p, err)) logError(p->cfg.id + ": " + err);
        if (prev == EngineMode::Device) ok = startDevice(error);
        else if (prev == EngineMode::Manual) ok = startManual(prevRate, prevCh, prevFrames, error);
    });
    markChanged();  // the configuration changed even if restarting failed
    return ok;
}

bool Engine::startDevice(std::string& error) {
    bool ok = false;
    host_->invoke([&] {
        stopRunning();
        if (!device_.open(config_.audio,
                          [this](float* const* out, uint32_t ch, uint32_t n) { deviceRender(out, ch, n); }, error))
            return;
        uint32_t period = std::max<uint32_t>(device_.periodFrames(), config_.audio.bufferFrames);
        if (!startRunning(EngineMode::Device, device_.sampleRate(), device_.channels(), period, error)) {
            device_.close();
            return;
        }
        if (!device_.start(error)) {
            stopRunning();
            return;
        }
        startClock();
        logInfo("audio: " + device_.deviceName() + " (" + device_.backendName() + ") @ " + std::to_string(outRate_) + " Hz, " +
                std::to_string(outChannels_) + " ch, plugins @ " + std::to_string(procRate_) + " Hz" +
                (resampler_ ? std::string(" (SRC ") + resamplerQualityName(config_.resamplerQuality) + ")" : ""));
        ok = true;
    });
    return ok;
}

bool Engine::startManual(uint32_t sampleRate, uint32_t channels, uint32_t maxFrames, std::string& error) {
    bool ok = false;
    host_->invoke([&] {
        stopRunning();
        if (sampleRate == 0 || channels == 0) {
            error = "invalid manual render format";
            return;
        }
        ok = startRunning(EngineMode::Manual, sampleRate, channels, std::max<uint32_t>(maxFrames, 1), error);
    });
    return ok;
}

void Engine::stop() {
    host_->invoke([this] { stopRunning(); });
}

bool Engine::startRunning(EngineMode mode, uint32_t outRate, uint32_t channels, uint32_t maxOutFrames,
                          std::string& error) {
    procRate_ = config_.processSampleRate ? config_.processSampleRate : outRate;
    blockSize_ = std::clamp<uint32_t>(config_.blockSize, 16, 8192);
    steadyTime_ = 0;
    renderPos_.store(0, std::memory_order_release);
    frameMap_ = {};
    arrival_.forgetOrder();
    if (!configureOutput(outRate, channels, maxOutFrames, error)) return false;
    for (auto& s : sources_) {
        s->queue->clear();
        s->pending->clear();
    }
    for (auto& p : plugins_) {
        p->direct->clear();
        p->pending->clear();
    }
    activateSlots();
    mode_ = mode;
    publishGraph();
    rtReady_.store(true);
    return true;
}

bool Engine::configureOutput(uint32_t outRate, uint32_t channels, uint32_t maxOutFrames, std::string& error) {
    outRate_ = outRate;
    frameMap_.setRates(renderPos_.load(std::memory_order_acquire), outRate_, procRate_);
    outChannels_ = std::min<uint32_t>(channels, (uint32_t)peaks_.size());
    periodFrames_ = maxOutFrames;
    mix_.assign(outChannels_, std::vector<float>(blockSize_, 0.0f));
    mixPtrs_.resize(outChannels_);
    for (uint32_t c = 0; c < outChannels_; ++c) mixPtrs_[c] = mix_[c].data();
    resampler_.reset();
    if (procRate_ != outRate_) {
        try {
            resampler_ = std::make_unique<MultiChannelResampler>(procRate_, outRate_, outChannels_, blockSize_,
                                                                 config_.resamplerQuality);
        } catch (const std::exception& e) {
            error = std::string("resampler: ") + e.what();
            return false;
        }
    }
    // A message arriving just as a period starts is taken at the next, a period on. Through
    // the SRC, plugin blocks are rendered ahead of the output, a block at most.
    const uint32_t latency =
        periodFrames_ + (resampler_ ? (uint32_t)std::ceil((double)blockSize_ * outRate_ / procRate_) : 0);
    cadence_.reset();
    callbackGapNs_.store(0, std::memory_order_relaxed);
    arrival_.reset(outRate_, latency, steadyNowNs());
    return true;
}

void Engine::checkOutputRoute() {
    if (mode_ != EngineMode::Device) return;
    const bool followDefault = config_.audio.deviceName.empty();
    const bool stalled = clockActive_.load();
    std::string target;
    if (followDefault) {
        target = defaultAudioOutputKey();
        const bool moved = device_.takeMoved();
        if (target.empty()) return;  // no output device at all: keep going on our own clock
        if (target == device_.deviceKey() && !stalled) {
            defaultSeen_.clear();
            return;
        }
        // A sound server that moves the stream with the default (PipeWire) has done so already:
        // reopening would only break the sound once more. Just after the default changed it may
        // not have yet, so a new default waits one more check before the device is reopened.
        if (!stalled && device_.movesWithDefault()) {
            if (moved) {
                const std::string previous = device_.deviceName();
                device_.movedTo(target);
                defaultSeen_.clear();
                logInfo("audio: now playing on " + device_.deviceName() + " (was " + previous +
                        "), moved there by the sound server");
                return;
            }
            if (defaultSeen_ != target) {
                defaultSeen_ = target;
                return;
            }
        }
    } else if (!stalled) {
        return;  // a named device is only reopened once it has gone away (and maybe come back)
    }
    defaultSeen_.clear();
    switchOutputDevice();
}

void Engine::switchOutputDevice() {
    // Plugins stay active and processing; only the output side is rebuilt. The
    // fallback clock is stopped while the buffers change hands, then keeps time
    // again until the new device delivers (or for good, if it would not open).
    stopClock(true);
    device_.close();
    rtReady_.store(false);
    uint64_t s = audioSeq_.load();
    if (s & 1) waitFor([&] { return audioSeq_.load() != s; }, std::chrono::milliseconds(2000));

    std::string error;
    const std::string previous = device_.deviceName();
    bool ok = device_.open(config_.audio,
                           [this](float* const* out, uint32_t ch, uint32_t n) { deviceRender(out, ch, n); }, error);
    if (ok) {
        uint32_t period = std::max<uint32_t>(device_.periodFrames(), config_.audio.bufferFrames);
        ok = configureOutput(device_.sampleRate(), device_.channels(), period, error);
        if (ok) publishGraph();  // audio routes are filtered by the new channel count
    }
    rtReady_.store(true);
    if (ok) ok = device_.start(error);
    if (!ok) {
        device_.close();
        if (!routeFailureLogged_) logWarn("audio: could not open the output device yet (" + error + "); still retrying");
        routeFailureLogged_ = true;
        startClock(false);  // stay on our own clock without announcing a new stall
        return;
    }
    routeFailureLogged_ = false;
    startClock(true);
    logInfo("audio: now playing on " + device_.deviceName() + " @ " + std::to_string(outRate_) + " Hz, " +
            std::to_string(outChannels_) + " ch" + (previous == device_.deviceName() ? "" : " (was " + previous + ")") +
            (resampler_ ? std::string(", plugins stay @ ") + std::to_string(procRate_) + " Hz through the SRC" : ""));
}

void Engine::stopRunning() {
    if (mode_ == EngineMode::Stopped) return;
    if (mode_ == EngineMode::Device) {
        // clap: stop_processing belongs on the audio thread; the device callback (or,
        // while the device is away, the fallback clock) is rendering, so ask it and
        // wait for the acknowledgement.
        for (auto& p : plugins_)
            if (p->inst && p->inst->isActive()) p->inst->requestStopProcessing();
        waitFor(
            [&] {
                for (auto& p : plugins_)
                    if (p->inst && p->inst->isActive() && !p->inst->stopProcessingAcknowledged()) return false;
                return true;
            },
            std::chrono::milliseconds(2000));
        stopClock();
        device_.close();
    }
    // In manual mode the client may never call render() again. Once no render()
    // is in flight, deactivate() stops processing on our side (see forceStopProcessing).
    rtReady_.store(false);
    // Wait until a render() still in flight has returned.
    uint64_t s = audioSeq_.load();
    if (s & 1) waitFor([&] { return audioSeq_.load() != s; }, std::chrono::milliseconds(2000));
    delete graph_.exchange(nullptr);
    for (auto& p : plugins_)
        if (p->inst) p->inst->deactivate();
    resampler_.reset();
    mode_ = EngineMode::Stopped;
}

void Engine::activateSlots() {
    std::vector<PluginSlot*> here, apart;
    for (auto& p : plugins_)
        if (p->inst) (p->inst->separateProcess() ? apart : here).push_back(p.get());
    auto activate = [this](PluginSlot& s) {
        std::string err;
        try {
            if (!activateSlot(s, err)) logError(s.cfg.id + ": " + err);
        } catch (const std::exception& e) {
            s.status = e.what();
            logError(s.cfg.id + ": " + e.what());
        }
    };
    parallelFor(apart.size(), [&](size_t i) { activate(*apart[i]); }, [&] {
        for (PluginSlot* s : here) activate(*s);
    });
}

bool Engine::activateSlot(PluginSlot& s, std::string& error) {
    if (!s.inst->activate(procRate_, blockSize_, error)) {
        s.status = error;
        return false;
    }
    s.status = "ok";
    return true;
}

void Engine::publishGraph(const PluginSlot* exclude) {
    auto* g = new RtGraph();
    std::map<std::string, uint32_t> nodeIndex;
    for (auto& p : plugins_) {
        if (p.get() == exclude || !p->inst || !p->inst->isActive()) continue;
        RtGraph::Node n{p->inst.get(), p->direct.get(), p->pending.get(), p->events.get(), {}};
        for (auto& r : audioRoutes_)
            if (r.plugin == p->cfg.id && r.output < outChannels_) n.outs.push_back({r.port, r.channel, r.output, r.gain});
        nodeIndex[p->cfg.id] = (uint32_t)g->nodes.size();
        g->nodes.push_back(std::move(n));
    }
    for (auto& s : sources_) {
        RtGraph::Source src{s->queue.get(), s->pending.get(), {}};
        for (auto& r : midiRoutes_) {
            if (r.source != s->cfg.id) continue;
            auto it = nodeIndex.find(r.plugin);
            if (it != nodeIndex.end()) src.targets.push_back({it->second, r.notePort});
        }
        g->sources.push_back(std::move(src));
    }
    RtGraph* old = graph_.exchange(g);
    if (!old) return;
    uint64_t s = audioSeq_.load();
    if ((s & 1) && !waitFor([&] { return audioSeq_.load() != s; }, std::chrono::milliseconds(2000))) {
        logWarn("audio thread unresponsive; leaking old routing graph");
        return;
    }
    delete old;
}

void Engine::quiesceSlot(PluginSlot& s) {
    // Manual mode: render() may not be called, so detach first; the plugin is
    // then stopped from the host thread when it is deactivated.
    if (s.inst && s.inst->isActive() && mode_ == EngineMode::Device) {
        s.inst->requestStopProcessing();
        waitFor([&] { return s.inst->stopProcessingAcknowledged(); }, std::chrono::milliseconds(2000));
    }
    publishGraph(&s);
}

// ---------------------------------------------------------------------------
// fallback clock

void Engine::deviceRender(float* const* out, uint32_t channels, uint32_t frames) {
    const int64_t now = steadyNowNs();
    lastDeviceCallbackNs_.store(now, std::memory_order_relaxed);
    cadence_.callback(now);
    callbackGapNs_.store(cadence_.longestGapNs(), std::memory_order_relaxed);
    bool expected = false;
    if (!renderBusy_.compare_exchange_strong(expected, true, std::memory_order_acquire)) {
        // The fallback clock is mid-render: the device just came back. One buffer of
        // silence; the clock sees this callback and stands down.
        for (uint32_t c = 0; c < channels; ++c) std::memset(out[c], 0, frames * sizeof(float));
        return;
    }
    arrival_.rendering(renderPos_.load(std::memory_order_relaxed), now, stallNs());
    render(out, channels, frames);
    renderBusy_.store(false, std::memory_order_release);
}

void Engine::startClock(bool freshDevice) {
    stopClock(true);
    if (freshDevice) lastDeviceCallbackNs_.store(steadyNowNs());
    clockQuit_ = false;
    clock_ = std::thread([this] { runClock(); });
}

int64_t Engine::stallNs() const {
    const auto periodNs = int64_t(periodFrames_) * 1'000'000'000 / std::max<uint32_t>(outRate_, 1);
    return CallbackCadence::stallNs(periodNs, callbackGapNs_.load(std::memory_order_relaxed));
}

void Engine::stopClock(bool keepState) {
    if (clock_.joinable()) {
        clockQuit_ = true;
        clock_.join();
    }
    if (!keepState) clockActive_ = false;
}

void Engine::runClock() {
    constexpr uint32_t kChunk = 2048;
    const uint32_t channels = outChannels_, rate = outRate_;
    std::vector<std::vector<float>> buf(channels, std::vector<float>(kChunk));
    std::vector<float*> ptrs;
    for (auto& b : buf) ptrs.push_back(b.data());

    // clockSinceNs_ / clockRendered_ survive a restart of this thread with the
    // stall still in progress (a device switch that could not open anything yet),
    // so time stays continuous across retries.
    int64_t& since = clockSinceNs_;   // when the device went quiet
    uint64_t& rendered = clockRendered_;  // frames rendered by this clock since then
    while (!clockQuit_.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        const int64_t now = steadyNowNs();
        const int64_t last = lastDeviceCallbackNs_.load(std::memory_order_relaxed);
        if (now - last < stallNs()) {
            if (clockActive_.exchange(false)) logInfo("audio device is back; sound resumes from now");
            continue;
        }
        if (!clockActive_.exchange(true)) {
            since = last;  // keep time continuous from the device's last callback
            rendered = 0;
            logWarn("audio device stopped; processing continues on Brack's own clock until it returns");
        }
        uint64_t due = uint64_t(double(now - since) * rate / 1e9);
        if (due > rendered + rate) rendered = due - rate;  // never replay more than a second (e.g. after sleep)
        while (rendered < due && !clockQuit_.load()) {
            const uint32_t n = uint32_t(std::min<uint64_t>(due - rendered, kChunk));
            bool expected = false;
            if (!renderBusy_.compare_exchange_strong(expected, true, std::memory_order_acquire)) break;
            arrival_.rendering(renderPos_.load(std::memory_order_relaxed), steadyNowNs(), stallNs());
            render(ptrs.data(), channels, n);
            renderBusy_.store(false, std::memory_order_release);
            rendered += n;
        }
    }
}

// ---------------------------------------------------------------------------
// audio thread

void Engine::render(float* const* out, uint32_t channels, uint32_t frames) {
    audioSeq_.fetch_add(1);
    setCurrentThreadIsAudio(true);
    auto t0 = std::chrono::steady_clock::now();
    RtGraph* g = graph_.load();
    const uint32_t ch = std::min(channels, outChannels_);

    if (!rtReady_.load() || !g) {
        for (uint32_t c = 0; c < channels; ++c) std::memset(out[c], 0, frames * sizeof(float));
        audioSeq_.fetch_add(1);
        return;
    }
    for (uint32_t c = ch; c < channels; ++c) std::memset(out[c], 0, frames * sizeof(float));

    if (!resampler_) {
        uint32_t pos = 0;
        while (pos < frames) {
            uint32_t n = std::min(frames - pos, blockSize_);
            processBlock(*g, n);
            for (uint32_t c = 0; c < ch; ++c) std::memcpy(out[c] + pos, mix_[c].data(), n * sizeof(float));
            pos += n;
        }
    } else {
        uint32_t done = 0;
        while (done < frames) {
            if (resampler_->available() == 0) {
                processBlock(*g, blockSize_);
                resampler_->push(mixPtrs_.data(), blockSize_);
                continue;
            }
            uint32_t n = std::min(resampler_->available(), frames - done);
            if (ch == outChannels_) {
                resampler_->pop(out, done, n);
            } else {
                // Caller has fewer channels than the engine: pop into the mix scratch first.
                uint32_t m = std::min(n, blockSize_);
                resampler_->pop(mixPtrs_.data(), 0, m);
                for (uint32_t c = 0; c < ch; ++c) std::memcpy(out[c] + done, mix_[c].data(), m * sizeof(float));
                n = m;
            }
            done += n;
        }
    }

    // Master gain, ramped across the buffer when it changed so a move does not click.
    const float g1 = masterGain_.load(std::memory_order_relaxed), g0 = masterApplied_;
    if (g0 != g1) {
        const float step = (g1 - g0) / (float)frames;
        for (uint32_t c = 0; c < ch; ++c)
            for (uint32_t i = 0; i < frames; ++i) out[c][i] *= g0 + step * (float)(i + 1);
        masterApplied_ = g1;
    } else if (g1 != 1.0f) {
        for (uint32_t c = 0; c < ch; ++c)
            for (uint32_t i = 0; i < frames; ++i) out[c][i] *= g1;
    }

    for (uint32_t c = 0; c < ch; ++c) {  // metered after the master gain: what the device gets
        float pk = 0;
        for (uint32_t i = 0; i < frames; ++i) pk = std::max(pk, std::fabs(out[c][i]));
        peaks_[c].store(std::max(pk, peaks_[c].load(std::memory_order_relaxed) * 0.9f), std::memory_order_relaxed);
    }
    renderPos_.fetch_add(frames, std::memory_order_release);
    double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    double budget = outRate_ ? (double)frames / outRate_ : 1.0;
    float load = (float)(elapsed / budget);
    cpuLoad_.store(cpuLoad_.load(std::memory_order_relaxed) * 0.9f + load * 0.1f, std::memory_order_relaxed);
    audioSeq_.fetch_add(1);
}

static_assert(ArrivalClock::kUnknown == Engine::kMidiNow, "an arrival not yet placed is due at once");

void Engine::processBlock(RtGraph& g, uint32_t frames) {
    // MIDI times are render positions (output frames); this block covers process frames
    // [start, end).
    const uint64_t start = steadyTime_, end = start + frames;
    auto processFrame = [&](uint64_t time) { return frameMap_.processFrame(time); };
    auto due = [&](uint64_t time) { return time == kMidiNow || processFrame(time) < end; };
    auto offset = [&](uint64_t time) -> uint32_t {
        if (time == kMidiNow) return 0;
        const uint64_t f = processFrame(time);
        return f > start ? (uint32_t)(f - start) : 0;
    };

    for (auto& n : g.nodes) n.events->clear();
    for (auto& src : g.sources) {
        auto deliver = [&](const uint8_t* d, uint32_t size, uint16_t, uint64_t time) {
            for (auto& t : src.targets) {
                auto& node = g.nodes[t.node];
                node.plugin->appendMidi(*node.events, t.port, d, size, offset(time));
            }
        };
        src.pending->take(due, deliver);
        src.queue->drain(scratch_, [&](const uint8_t* d, uint32_t size, uint16_t port, uint64_t time, int64_t when) {
            if (time == kMidiNow) time = arrival_.due(when);
            if (due(time)) deliver(d, size, port, time);
            else src.pending->add(d, size, port, time);
        });
    }
    for (auto& n : g.nodes) {
        auto deliver = [&](const uint8_t* d, uint32_t size, uint16_t port, uint64_t time) {
            n.plugin->appendMidi(*n.events, port, d, size, offset(time));
        };
        n.pending->take(due, deliver);
        n.direct->drain(scratch_, [&](const uint8_t* d, uint32_t size, uint16_t port, uint64_t time, int64_t when) {
            if (time == kMidiNow) time = arrival_.due(when);
            if (due(time)) deliver(d, size, port, time);
            else n.pending->add(d, size, port, time);
        });
        n.events->sortByTime();
    }

    for (uint32_t c = 0; c < outChannels_; ++c) std::memset(mix_[c].data(), 0, frames * sizeof(float));
    for (auto& n : g.nodes) n.plugin->beginProcess(frames, steadyTime_, *n.events);
    for (auto& n : g.nodes) {
        if (!n.plugin->endProcess()) continue;
        for (auto& o : n.outs) {
            const float* src = n.plugin->outputChannel(o.port, o.channel);
            if (!src) continue;
            float* dst = mix_[o.output].data();
            for (uint32_t i = 0; i < frames; ++i) dst[i] += src[i] * o.gain;
        }
    }
    steadyTime_ += frames;
}

// ---------------------------------------------------------------------------
// plugins

std::string Engine::uniqueId(const std::string& base, bool plugin) const {
    std::string b;
    for (char c : base) {
        unsigned char u = (unsigned char)c;
        if (std::isalnum(u)) b += (char)std::tolower(u);
        else if (!b.empty() && b.back() != '-') b += '-';
    }
    auto cut = [](std::string id, size_t size) {
        if (id.size() > size) id.resize(size);
        while (!id.empty() && id.back() == '-') id.pop_back();
        return id;
    };
    b = cut(b, kMaxGeneratedId);
    if (b.empty()) b = plugin ? "plugin" : "midi";
    auto taken = [&](const std::string& id) { return plugin ? findPlugin(id) != nullptr : findSource(id) != nullptr; };
    if (!taken(b)) return b;
    for (int i = 2;; ++i) {
        const std::string suffix = "-" + std::to_string(i);
        std::string id = cut(b, kMaxGeneratedId - suffix.size()) + suffix;
        if (!taken(id)) return id;
    }
}

Engine::PluginSlot* Engine::findPlugin(const std::string& id) const {
    for (auto& p : plugins_)
        if (p->cfg.id == id) return p.get();
    return nullptr;
}

Engine::SourceSlot* Engine::findSource(const std::string& id) const {
    for (auto& s : sources_)
        if (s->cfg.id == id) return s.get();
    return nullptr;
}

void Engine::rebuildInjectionMaps() {
    std::unique_lock lock(injectMutex_);
    injectPlugins_.clear();
    injectSources_.clear();
    for (auto& p : plugins_) injectPlugins_[p->cfg.id] = p->direct.get();
    for (auto& s : sources_) injectSources_[s->cfg.id] = s->queue.get();
}

// A plugin in a plugin host loads without holding up the host thread (other plugins' editors,
// idle work).
std::string Engine::addPlugin(const PluginConfig& cfg, bool autoRouteAudio, std::string& error) {
    bool taken = false;
    host_->invoke([&] { taken = !cfg.id.empty() && findPlugin(cfg.id); });
    if (taken) {
        error = "plugin id already in use: " + cfg.id;
        return {};
    }
    bool stateRejected = false;
    auto inst = makePlugin(cfg, stateRejected, error);
    if (!inst) return {};
    std::string id;
    host_->invoke([&] { id = installPlugin(cfg, std::move(inst), stateRejected, autoRouteAudio, error); });
    if (!id.empty()) markChanged();
    return id;
}

std::unique_ptr<HostedPlugin> Engine::makePlugin(const PluginConfig& cfg, bool& stateRejected, std::string& error) {
    std::string architecture = pluginArchitecture(pathFromUtf8(cfg.path));
    if (architecture.empty()) architecture = buildArchitecture();  // the host process reports what is wrong
    bool inProcess = false;
    host_->invoke([&] { inProcess = config_.pluginsInProcess && architecture == buildArchitecture(); });
    std::unique_ptr<HostedPlugin> inst;
    auto make = [&] {
        if (inProcess) inst = createPluginInstance(*host_, *this, cfg.path, cfg.pluginId, error);
        else inst = createRemotePlugin(*this, cfg.path, cfg.pluginId, architecture);
        if (!inst || !inst->init(error)) {
            inst.reset();
            error = cfg.path + ": " + error;
            return;
        }
        stateRejected = cfg.state && !cfg.state->empty() && !inst->loadState(*cfg.state);
    };
    if (inProcess) host_->invoke(make);
    else make();
    return inst;
}

std::string Engine::installPlugin(PluginConfig cfg, std::unique_ptr<HostedPlugin> inst, bool stateRejected,
                                  bool autoRouteAudio, std::string& error) {
    if (!cfg.id.empty() && findPlugin(cfg.id)) {
        error = "plugin id already in use: " + cfg.id;
        return {};
    }
    const PluginDescription& desc = inst->description();
    cfg.pluginId = desc.id;
    cfg.path = desc.path;
    if (cfg.name.empty()) cfg.name = desc.name;
    if (cfg.id.empty()) cfg.id = uniqueId(cfg.name, true);
    inst->setDisplayName(cfg.name);
    if (stateRejected) logWarn(cfg.id + ": plugin rejected the saved state");

    auto slot = std::make_unique<PluginSlot>();
    slot->cfg = cfg;
    slot->inst = std::move(inst);
    slot->status = "loaded";
    if (mode_ != EngineMode::Stopped) {
        std::string err;
        if (!activateSlot(*slot, err)) logError(cfg.id + ": " + err);
    }
    if (autoRouteAudio && !slot->inst->audioOutputs().empty()) {
        auto& outs = slot->inst->audioOutputs();
        uint32_t port = 0;
        for (uint32_t i = 0; i < outs.size(); ++i)
            if (outs[i].isMain) {
                port = i;
                break;
            }
        uint32_t chs = outs[port].channels;
        if (chs == 1) {
            audioRoutes_.push_back({cfg.id, port, 0, 0, 1.0f});
            audioRoutes_.push_back({cfg.id, port, 0, 1, 1.0f});
        } else {
            for (uint32_t c = 0; c < std::min<uint32_t>(chs, 2); ++c) audioRoutes_.push_back({cfg.id, port, c, c, 1.0f});
        }
    }
    plugins_.push_back(std::move(slot));
    rebuildInjectionMaps();
    if (mode_ != EngineMode::Stopped) publishGraph();
    return cfg.id;
}

bool Engine::reloadPlugin(const std::string& id, std::string& error) {
    bool ok = false;
    host_->invoke([&] {
        auto* s = findPlugin(id);
        if (!s) {
            error = "no such plugin: " + id;
            return;
        }
        ok = reloadSlot(*s, error);
        if (mode_ != EngineMode::Stopped) publishGraph();
    });
    return ok;
}

bool Engine::reloadSlot(PluginSlot& s, std::string& error) {
    if (s.inst) {
        if (std::vector<uint8_t> state; s.inst->saveState(state)) s.cfg.state = std::move(state);
        quiesceSlot(s);
        s.inst.reset();
    }
    bool stateRejected = false;
    auto inst = makePlugin(s.cfg, stateRejected, error);
    if (!inst) {
        s.status = error;
        return false;
    }
    s.cfg.pluginId = inst->description().id;
    if (s.cfg.name.empty()) s.cfg.name = inst->description().name;
    inst->setDisplayName(s.cfg.name);
    if (stateRejected) logWarn(s.cfg.id + ": plugin rejected the saved state");
    s.inst = std::move(inst);
    s.crashReported = false;
    s.status = "loaded";
    if (mode_ != EngineMode::Stopped && !activateSlot(s, error)) return false;
    return true;
}

bool Engine::removePlugin(const std::string& id, std::string& error) {
    bool ok = false;
    host_->invoke([&] {
        auto it = std::find_if(plugins_.begin(), plugins_.end(), [&](auto& p) { return p->cfg.id == id; });
        if (it == plugins_.end()) {
            error = "no such plugin: " + id;
            return;
        }
        {
            std::unique_lock lock(injectMutex_);
            injectPlugins_.erase(id);
        }
        quiesceSlot(**it);  // the audio thread no longer sees it after this
        std::erase_if(midiRoutes_, [&](auto& r) { return r.plugin == id; });
        std::erase_if(audioRoutes_, [&](auto& r) { return r.plugin == id; });
        std::unique_ptr<PluginSlot> slot = std::move(*it);
        plugins_.erase(it);
        slot.reset();  // closes GUI, deactivates, destroys
        ok = true;
    });
    if (ok) markChanged();
    return ok;
}

bool Engine::setPluginGuiVisible(const std::string& id, bool visible, std::string& error) {
    bool ok = false;
    host_->invoke([&] {
        auto* s = findPlugin(id);
        if (!s || !s->inst) {
            error = (s ? "plugin not loaded: " : "no such plugin: ") + id;
            return;
        }
        if (visible) {
            ok = s->inst->openGui(error);
        } else {
            s->inst->closeGui();
            ok = true;
        }
    });
    return ok;
}

bool Engine::showPluginGuiIn(const std::string& id, void* parent, std::string& error) {
    if (!parent) {
        error = "no parent window";
        return false;
    }
    bool ok = false;
    host_->invoke([&] {
        auto* s = findPlugin(id);
        if (!s || !s->inst) {
            error = (s ? "plugin not loaded: " : "no such plugin: ") + id;
            return;
        }
        ok = s->inst->openGui(error, parent);
    });
    return ok;
}

bool Engine::getPluginState(const std::string& id, std::vector<uint8_t>& out, std::string& error) {
    bool ok = false;
    host_->invoke([&] {
        auto* s = findPlugin(id);
        if (!s) {
            error = "no such plugin: " + id;
            return;
        }
        if (s->inst) {
            ok = s->inst->saveState(out);
            if (!ok) error = "the plugin did not save its state";
        } else if (s->cfg.state) {
            out = *s->cfg.state;
            ok = true;
        } else {
            error = "the plugin is not loaded and has no state";
        }
    });
    return ok;
}

bool Engine::setPluginState(const std::string& id, const std::vector<uint8_t>& state, std::string& error) {
    bool ok = false;
    host_->invoke([&] {
        auto* s = findPlugin(id);
        if (!s) {
            error = "no such plugin: " + id;
            return;
        }
        if (s->inst) {
            ok = s->inst->loadState(state);
            if (!ok) error = "the plugin rejected the state";
        } else {
            s->cfg.state = state;
            ok = true;
        }
    });
    if (ok) markChanged();
    return ok;
}

bool Engine::setMasterGain(float gain) {
    if (!(gain >= 0.0f) || !std::isfinite(gain)) return false;
    masterGain_.store(gain, std::memory_order_relaxed);
    markChanged();
    return true;
}

bool Engine::setPluginName(const std::string& id, const std::string& name, std::string& error) {
    bool ok = false;
    host_->invoke([&] {
        auto* s = findPlugin(id);
        if (!s) {
            error = "no such plugin: " + id;
            return;
        }
        std::string n = name;
        if (n.empty()) n = s->inst ? s->inst->description().name : s->cfg.pluginId;
        s->cfg.name = n;
        if (s->inst) s->inst->setDisplayName(n);
        ok = true;
    });
    if (ok) markChanged();
    return ok;
}

namespace {
// Moves the slot with `id` to `index`. Only the unique_ptrs move, never the slots, so the
// audio thread's graph (which points into them) is unaffected.
template <typename Slots>
bool moveSlot(Slots& slots, const std::string& id, size_t index) {
    auto it = std::find_if(slots.begin(), slots.end(), [&](auto& s) { return s->cfg.id == id; });
    if (it == slots.end()) return false;
    auto slot = std::move(*it);
    slots.erase(it);
    slots.insert(slots.begin() + std::min(index, slots.size()), std::move(slot));
    return true;
}
}  // namespace

bool Engine::movePlugin(const std::string& id, size_t index, std::string& error) {
    bool ok = false;
    host_->invoke([&] {
        ok = moveSlot(plugins_, id, index);
        if (!ok) error = "no such plugin: " + id;
    });
    if (ok) markChanged();
    return ok;
}

bool Engine::moveMidiSource(const std::string& id, size_t index, std::string& error) {
    bool ok = false;
    host_->invoke([&] {
        ok = moveSlot(sources_, id, index);
        if (!ok) error = "no such MIDI source: " + id;
    });
    if (ok) markChanged();
    return ok;
}

void Engine::pluginStateChanged(HostedPlugin&) { markChanged(); }

void Engine::pluginGuiClosed(HostedPlugin& inst) {
    if (auto* s = findPlugin(inst)) pushEvent({EngineEvent::Type::EditorClosed, s->cfg.id, {}, 0, 0});
}

void Engine::pluginEditorResized(HostedPlugin& inst, uint32_t width, uint32_t height) {
    if (auto* s = findPlugin(inst)) pushEvent({EngineEvent::Type::EditorResized, s->cfg.id, {}, width, height});
}

Engine::PluginSlot* Engine::findPlugin(const HostedPlugin& inst) const {
    for (auto& p : plugins_)
        if (p->inst.get() == &inst) return p.get();
    return nullptr;
}

// ---------------------------------------------------------------------------
// events

void Engine::pushEvent(EngineEvent e) {
    {
        std::lock_guard lock(eventMutex_);
        if (e.type == EngineEvent::Type::Changed) {
            if (changedQueued_) return;
            changedQueued_ = true;
        }
        if (events_.size() >= 1024) {
            if (events_.front().type == EngineEvent::Type::Changed) changedQueued_ = false;
            events_.pop_front();
        }
        events_.push_back(std::move(e));
    }
    if (eventNotify_) eventNotify_();
}

void Engine::checkEvents() {
    for (auto& p : plugins_)
        if (p->inst && p->inst->crashed() && !p->crashReported) {
            p->crashReported = true;
            const std::string report = p->inst->crashReport();
            logError(p->cfg.name + ": " + report +
                     (p->inst->separateProcess()
                          ? ". It stays silent until it is loaded again."
                          : ". It is no longer called and stays silent; save the session and restart Brack to use it again."));
            pushEvent({EngineEvent::Type::PluginCrashed, p->cfg.id, report, 0, 0});
        }
    const bool stalled = mode_ == EngineMode::Device && clockActive_.load();
    if (stalled != stallSeen_) {
        stallSeen_ = stalled;
        pushEvent({stalled ? EngineEvent::Type::DeviceStalled : EngineEvent::Type::DeviceResumed, {}, {}, 0, 0});
    }
    const uint64_t changes = changeSeq_.load(std::memory_order_relaxed);
    if (changes != changeSeen_) {
        changeSeen_ = changes;
        pushEvent({EngineEvent::Type::Changed, {}, {}, 0, 0});
    }
}

bool Engine::pollEvent(EngineEvent& out) {
    std::lock_guard lock(eventMutex_);
    if (events_.empty()) return false;
    out = std::move(events_.front());
    events_.pop_front();
    if (out.type == EngineEvent::Type::Changed) changedQueued_ = false;
    return true;
}

void Engine::setEventNotify(std::function<void()> notify) {
    host_->invoke([&] { eventNotify_ = std::move(notify); });
}

void Engine::onIdle() {
    {
        const auto now = std::chrono::steady_clock::now();
        if (now - lastCachePublish_ >= std::chrono::milliseconds(50)) {
            lastCachePublish_ = now;
            publishCachedSnapshot();
        }
    }
    {
        const auto now = std::chrono::steady_clock::now();
        if (now - lastRouteCheck_ >= std::chrono::seconds(1)) {
            lastRouteCheck_ = now;
            if (mode_ == EngineMode::Device) checkOutputRoute();
            checkMidiInputs();
        }
    }
    for (auto& p : plugins_) {
        if (!p->inst) continue;
        p->inst->idle();
        if (p->inst->restartPending() && p->inst->isActive() && mode_ != EngineMode::Stopped) {
            quiesceSlot(*p);
            p->inst->deactivate();
            std::string err;
            if (!activateSlot(*p, err)) logError(p->cfg.id + ": restart failed: " + err);
            publishGraph();
        }
    }
    checkEvents();
}

// ---------------------------------------------------------------------------
// MIDI sources

void Engine::checkMidiInputs() {
    std::vector<std::string> available;
    bool listed = false;
    for (auto& s : sources_) {
        if (s->cfg.kind != MidiSourceKind::Hardware) continue;  // virtual ports: see below
        if (s->ok && s->port && s->port->lost()) {
            logWarn("MIDI source " + s->cfg.id + ": the connection to " + s->cfg.name + " was closed; reconnecting");
            s->port.reset();
            s->ok = false;
            s->status = "connection lost";
            pushEvent({EngineEvent::Type::MidiSourceLost, s->cfg.id, s->status, 0, 0});
        }
        if (s->ok) continue;
        // Reopen once the port is listed again. Quietly: openSourcePort() logs every failure,
        // and a port that stays away would log once a second.
        if (!listed) {
            available = listHardwareMidiInputs();
            listed = true;
        }
        if (std::find(available.begin(), available.end(), s->cfg.name) == available.end()) continue;
        openSourcePort(*s, true);
        if (s->ok) {
            logInfo("MIDI source " + s->cfg.id + ": reconnected to " + s->cfg.name);
            pushEvent({EngineEvent::Type::MidiSourceReconnected, s->cfg.id, {}, 0, 0});
        }
    }
    // Virtual ports are not recreated behind the user's back: on affected Windows builds
    // removing or recreating one can wedge the MIDI service (microsoft/MIDI#1047).
}

void Engine::openSourcePort(SourceSlot& s, bool quiet) {
    s.port.reset();
    s.ok = false;
    std::string err;
    SourceSlot* sp = &s;
    auto onMessage = [sp](const uint8_t* d, size_t n, int64_t whenNs) {
        sp->queue->push(d, (uint32_t)n, 0, kMidiNow, whenNs);
        sp->count.fetch_add(1, std::memory_order_relaxed);
    };
    switch (s.cfg.kind) {
        case MidiSourceKind::Api:
            s.ok = true;
            s.status = "ok";
            return;
        case MidiSourceKind::Hardware: s.port = openHardwareMidiInput(s.cfg.name, onMessage, err); break;
        case MidiSourceKind::Virtual: s.port = createVirtualMidiInput(s.cfg.name, onMessage, err); break;
    }
    s.ok = s.port != nullptr;
    s.status = s.ok ? "ok" : err;
    if (!s.ok && !quiet) logError("MIDI source " + s.cfg.id + ": " + err);
}

std::string Engine::addMidiSource(const MidiSourceConfig& cfgIn, std::string& error) {
    std::string result;
    host_->invoke([&] {
        MidiSourceConfig cfg = cfgIn;
        if (!cfg.id.empty() && findSource(cfg.id)) {
            error = "MIDI source id already in use: " + cfg.id;
            return;
        }
        if (cfg.kind != MidiSourceKind::Api && cfg.name.empty()) {
            error = "MIDI source needs a port name";
            return;
        }
        if (cfg.kind == MidiSourceKind::Virtual)
            for (auto& s : sources_)
                if (s->cfg.kind == MidiSourceKind::Virtual && s->cfg.name == cfg.name) {
                    error = "virtual port name already in use: " + cfg.name;
                    return;
                }
        if (cfg.id.empty()) cfg.id = uniqueId(cfg.name.empty() ? "api" : cfg.name, false);
        auto slot = std::make_unique<SourceSlot>();
        slot->cfg = cfg;
        openSourcePort(*slot);
        sources_.push_back(std::move(slot));
        rebuildInjectionMaps();
        if (mode_ != EngineMode::Stopped) publishGraph();
        result = cfg.id;
    });
    if (!result.empty()) markChanged();
    return result;
}

bool Engine::removeMidiSource(const std::string& id, std::string& error) {
    bool ok = false;
    host_->invoke([&] {
        auto it = std::find_if(sources_.begin(), sources_.end(), [&](auto& s) { return s->cfg.id == id; });
        if (it == sources_.end()) {
            error = "no such MIDI source: " + id;
            return;
        }
        {
            std::unique_lock lock(injectMutex_);
            injectSources_.erase(id);
        }
        (*it)->port.reset();  // no more driver callbacks after this
        std::erase_if(midiRoutes_, [&](auto& r) { return r.source == id; });
        std::unique_ptr<SourceSlot> slot = std::move(*it);
        sources_.erase(it);
        if (mode_ != EngineMode::Stopped) publishGraph();
        ok = true;
    });
    if (ok) markChanged();
    return ok;
}

bool Engine::reopenMidiSource(const std::string& id, std::string& error) {
    bool ok = false;
    host_->invoke([&] {
        auto* s = findSource(id);
        if (!s) {
            error = "no such MIDI source: " + id;
            return;
        }
        openSourcePort(*s);
        ok = s->ok;
        if (!ok) error = s->status;
    });
    return ok;
}

// ---------------------------------------------------------------------------
// routing

bool Engine::addMidiRoute(const MidiRoute& r, std::string& error) {
    bool ok = false;
    host_->invoke([&] {
        if (!findSource(r.source)) {
            error = "no such MIDI source: " + r.source;
            return;
        }
        auto* p = findPlugin(r.plugin);
        if (!p) {
            error = "no such plugin: " + r.plugin;
            return;
        }
        if (p->inst && r.notePort >= p->inst->notePorts().size()) {
            error = "plugin " + r.plugin + " has no note port " + std::to_string(r.notePort);
            return;
        }
        if (std::find(midiRoutes_.begin(), midiRoutes_.end(), r) == midiRoutes_.end()) midiRoutes_.push_back(r);
        if (mode_ != EngineMode::Stopped) publishGraph();
        ok = true;
    });
    if (ok) markChanged();
    return ok;
}

bool Engine::removeMidiRoute(const MidiRoute& r) {
    bool ok = false;
    host_->invoke([&] {
        auto it = std::find(midiRoutes_.begin(), midiRoutes_.end(), r);
        if (it == midiRoutes_.end()) return;
        midiRoutes_.erase(it);
        if (mode_ != EngineMode::Stopped) publishGraph();
        ok = true;
    });
    if (ok) markChanged();
    return ok;
}

bool Engine::setAudioRoutes(const std::string& plugin, const std::vector<AudioRoute>& routes, std::string& error) {
    bool ok = false;
    host_->invoke([&] {
        if (!findPlugin(plugin)) {
            error = "no such plugin: " + plugin;
            return;
        }
        std::erase_if(audioRoutes_, [&](auto& r) { return r.plugin == plugin; });
        for (auto r : routes) {
            r.plugin = plugin;
            audioRoutes_.push_back(r);
        }
        if (mode_ != EngineMode::Stopped) publishGraph();
        ok = true;
    });
    if (ok) markChanged();
    return ok;
}

namespace {
bool sameAudioRoute(const AudioRoute& a, const AudioRoute& b) {
    return a.plugin == b.plugin && a.port == b.port && a.channel == b.channel && a.output == b.output;
}
}  // namespace

bool Engine::addAudioRoute(const AudioRoute& r, std::string& error) {
    bool ok = false;
    host_->invoke([&] {
        if (!findPlugin(r.plugin)) {
            error = "no such plugin: " + r.plugin;
            return;
        }
        auto it = std::find_if(audioRoutes_.begin(), audioRoutes_.end(), [&](auto& x) { return sameAudioRoute(x, r); });
        if (it != audioRoutes_.end()) it->gain = r.gain;
        else audioRoutes_.push_back(r);
        if (mode_ != EngineMode::Stopped) publishGraph();
        ok = true;
    });
    if (ok) markChanged();
    return ok;
}

bool Engine::removeAudioRoute(const AudioRoute& r) {
    bool ok = false;
    host_->invoke([&] {
        auto it = std::find_if(audioRoutes_.begin(), audioRoutes_.end(), [&](auto& x) { return sameAudioRoute(x, r); });
        if (it == audioRoutes_.end()) return;
        audioRoutes_.erase(it);
        if (mode_ != EngineMode::Stopped) publishGraph();
        ok = true;
    });
    if (ok) markChanged();
    return ok;
}

// ---------------------------------------------------------------------------
// injection

namespace {
template <typename Queues>
MidiSend pushMidi(std::shared_mutex& mutex, const Queues& queues, std::string_view id, const uint8_t* data,
                  uint32_t size, uint16_t notePort, uint64_t time, int64_t timeNs = 0) {
    if (!isValidMessage(data, size)) return MidiSend::Malformed;
    std::shared_lock lock(mutex);
    auto it = queues.find(id);
    if (it == queues.end()) return MidiSend::UnknownId;
    return it->second->push(data, size, notePort, time, timeNs) ? MidiSend::Sent : MidiSend::QueueFull;
}
}  // namespace

MidiSend Engine::sendMidiToPlugin(std::string_view pluginId, uint16_t notePort, const uint8_t* data, uint32_t size,
                                  uint64_t time) {
    return pushMidi(injectMutex_, injectPlugins_, pluginId, data, size, notePort, time);
}

MidiSend Engine::sendMidiToSource(std::string_view sourceId, const uint8_t* data, uint32_t size, uint64_t time) {
    return pushMidi(injectMutex_, injectSources_, sourceId, data, size, 0, time);
}

int64_t Engine::nowNs() { return steadyNowNs(); }

MidiSend Engine::sendMidiToPluginAtTime(std::string_view pluginId, uint16_t notePort, const uint8_t* data,
                                        uint32_t size, int64_t timeNs) {
    if (timeNs <= 0) return MidiSend::Malformed;
    return pushMidi(injectMutex_, injectPlugins_, pluginId, data, size, notePort, kMidiNow, timeNs);
}

MidiSend Engine::sendMidiToSourceAtTime(std::string_view sourceId, const uint8_t* data, uint32_t size,
                                        int64_t timeNs) {
    if (timeNs <= 0) return MidiSend::Malformed;
    return pushMidi(injectMutex_, injectSources_, sourceId, data, size, 0, kMidiNow, timeNs);
}

// ---------------------------------------------------------------------------
// inspection

EngineSnapshot Engine::snapshot() const {
    EngineSnapshot s;
    host_->invoke([&] { s = buildSnapshot(); });
    return s;
}

EngineSnapshot Engine::cachedSnapshot() const {
    EngineSnapshot s;
    {
        std::lock_guard lock(cachedMutex_);
        s = cached_;
    }
    // Live values, so meters move even while the host thread is busy.
    s.cpuLoad = cpuLoad_.load();
    s.masterGain = masterGain_.load();
    for (uint32_t c = 0; c < s.outputPeaks.size(); ++c) s.outputPeaks[c] = peaks_[c].load();
    s.deviceStalled = s.mode == EngineMode::Device && clockActive_.load();
    return s;
}

void Engine::refreshSnapshot() {
    host_->invoke([this] { publishCachedSnapshot(); });
}

void Engine::publishCachedSnapshot() {
    EngineSnapshot s = buildSnapshot();
    std::lock_guard lock(cachedMutex_);
    cached_ = std::move(s);
}

EngineSnapshot Engine::buildSnapshot() const {
    EngineSnapshot s;
    {
        s.mode = mode_;
        s.config = config_;
        s.deviceName = mode_ == EngineMode::Device ? device_.deviceName() : std::string();
        s.outputSampleRate = mode_ != EngineMode::Stopped ? outRate_ : 0;
        s.processSampleRate = mode_ != EngineMode::Stopped ? procRate_ : 0;
        s.outputChannels = mode_ != EngineMode::Stopped ? outChannels_ : 0;
        s.periodFrames = periodFrames_;
        s.resampling = resampler_ != nullptr;
        s.deviceStalled = mode_ == EngineMode::Device && clockActive_.load();
        s.resamplerLatencyMs = resampler_ ? resampler_->latencyInputFrames() * 1000.0 / procRate_ : 0.0;
        s.cpuLoad = cpuLoad_.load();
        s.masterGain = masterGain_.load();
        for (uint32_t c = 0; c < s.outputChannels; ++c) s.outputPeaks.push_back(peaks_[c].load());
        for (auto& p : plugins_) {
            EngineSnapshot::Plugin sp;
            sp.id = p->cfg.id;
            sp.name = p->cfg.name;
            sp.pluginId = p->cfg.pluginId;
            pluginFormatFromPath(pathFromUtf8(p->cfg.path), sp.format);
            sp.path = p->cfg.path;
            sp.status = p->status;
            sp.loaded = p->inst != nullptr;
            if (p->inst) {
                sp.active = p->inst->isActive();
                sp.hasGui = p->inst->hasGui();
                sp.guiOpen = p->inst->isGuiOpen();
                sp.failed = p->inst->processFailed();
                sp.crashed = p->inst->crashed();
                sp.architecture = p->inst->description().architecture;
                sp.separateProcess = p->inst->separateProcess();
                if (sp.crashed) sp.status = p->inst->crashReport();
                sp.notePorts = p->inst->notePorts();
                sp.audioOutputs = p->inst->audioOutputs();
                sp.latency = p->inst->latency();
            }
            s.plugins.push_back(std::move(sp));
        }
        for (auto& src : sources_) {
            EngineSnapshot::Source ss;
            ss.id = src->cfg.id;
            ss.name = src->cfg.name;
            ss.kind = src->cfg.kind;
            ss.status = src->status;
            ss.ok = src->ok;
            ss.messages = src->count.load();
            ss.dropped = src->queue->droppedCount();
            s.sources.push_back(std::move(ss));
        }
        s.midiRoutes = midiRoutes_;
        s.audioRoutes = audioRoutes_;
    }
    return s;
}

std::vector<PluginDescription> Engine::scanPlugins(const std::vector<std::string>& extraDirs,
                                                   const std::string& cachePathUtf8, bool standardLocations) {
    std::vector<std::filesystem::path> dirs;
    if (standardLocations) dirs = defaultPluginSearchPaths();
    for (auto& d : extraDirs) dirs.push_back(pathFromUtf8(d));
    if (cachePathUtf8.empty()) return brack::scanPlugins(dirs);
    const auto cachePath = pathFromUtf8(cachePathUtf8);
    PluginScanCache cache;
    cache.load(cachePath);
    auto result = brack::scanPlugins(dirs, &cache);
    if (std::string err; !cache.save(cachePath, err)) logWarn("plugin cache: " + err);
    return result;
}

}  // namespace brack
