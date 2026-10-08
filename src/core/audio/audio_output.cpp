#include "audio/audio_output.h"

#include "util/common.h"
#ifdef __linux__
#include <strings.h>
#include <unistd.h>

#include "util/realtime_linux.h"
#endif
#ifdef _WIN32
#include "util/realtime_win32.h"
#endif
#ifdef __APPLE__
#include "util/realtime_mac.h"
#endif

#define MA_NO_DECODING
#define MA_NO_ENCODING
#define MA_NO_GENERATION
#define MA_NO_RESOURCE_MANAGER
#define MA_NO_NODE_GRAPH
#define MA_NO_ENGINE
#define MINIAUDIO_IMPLEMENTATION
#include <miniaudio.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <mutex>
#include <thread>

namespace brack {

namespace {

struct Context {
    ma_context ctx{};
    bool ok = false;
    Context() {
        ma_context_config cfg = ma_context_config_init();
        ma_backend backend;
        bool chosen = false;
#ifdef _WIN32
        backend = ma_backend_wasapi;
        chosen = true;
#else
        // BRACK_AUDIO_BACKEND names one (pulseaudio, alsa, jack, coreaudio) in place of the first
        // of miniaudio's that opens: for trying the others, which the sound server usually hides.
        if (const char* name = std::getenv("BRACK_AUDIO_BACKEND"); name && *name)
            for (int b = 0; b < MA_BACKEND_COUNT && !chosen; ++b)
                if (strcasecmp(name, ma_get_backend_name((ma_backend)b)) == 0) backend = (ma_backend)b, chosen = true;
#endif
        ok = ma_context_init(chosen ? &backend : nullptr, chosen ? 1 : 0, &cfg, &ctx) == MA_SUCCESS;
    }
    ~Context() {
        if (ok) ma_context_uninit(&ctx);
    }
};

Context& context() {
    // Deliberately never destroyed. When brack.dll is unloaded at process exit, the
    // other threads are already gone, and ma_context_uninit() would wait forever for
    // WASAPI's command thread to answer. The OS reclaims everything anyway.
    static Context* c = new Context();
    return *c;
}
std::mutex g_contextMutex;  // device enumeration is not thread safe

std::string deviceKeyOf(const ma_device_id& id, const char* name) {
#ifdef _WIN32
    (void)name;
    return narrow(id.wasapi);
#else
    (void)id;
    return name ? name : "";
#endif
}

}  // namespace

std::string defaultAudioOutputKey() {
    std::lock_guard lock(g_contextMutex);
    auto& c = context();
    if (!c.ok) return {};
    ma_device_info* infos = nullptr;
    ma_uint32 count = 0;
    if (ma_context_get_devices(&c.ctx, &infos, &count, nullptr, nullptr) != MA_SUCCESS) return {};
    for (ma_uint32 i = 0; i < count; ++i)
        if (infos[i].isDefault) return deviceKeyOf(infos[i].id, infos[i].name);
    return {};
}

std::vector<AudioDeviceInfo> listAudioOutputDevices() {
    std::vector<AudioDeviceInfo> result;
    std::lock_guard lock(g_contextMutex);
    auto& c = context();
    if (!c.ok) return result;
    ma_device_info* infos = nullptr;
    ma_uint32 count = 0;
    if (ma_context_get_devices(&c.ctx, &infos, &count, nullptr, nullptr) != MA_SUCCESS) return result;
    for (ma_uint32 i = 0; i < count; ++i) {
        AudioDeviceInfo d;
        d.name = infos[i].name;
        d.isDefault = infos[i].isDefault != 0;
        ma_device_info full{};
        if (ma_context_get_device_info(&c.ctx, ma_device_type_playback, &infos[i].id, &full) == MA_SUCCESS) {
            for (ma_uint32 f = 0; f < full.nativeDataFormatCount; ++f) {
                auto& nf = full.nativeDataFormats[f];
                if (nf.sampleRate && std::find(d.sampleRates.begin(), d.sampleRates.end(), nf.sampleRate) == d.sampleRates.end())
                    d.sampleRates.push_back(nf.sampleRate);
                d.maxChannels = std::max<uint32_t>(d.maxChannels, nf.channels);
            }
            std::sort(d.sampleRates.begin(), d.sampleRates.end());
        }
        result.push_back(std::move(d));
    }
    return result;
}

struct AudioOutput::Impl {
    ma_device device{};
    bool open = false;
    std::atomic<bool> running{false};
    AudioRenderFn render;
    std::vector<std::vector<float>> planar;
    std::vector<float*> ptrs;
    uint32_t channels = 0;
    std::string name, key;
    bool movesWithDefault = false;
    std::atomic<bool> moved{false};
#ifdef __APPLE__
    std::atomic<int> realtimeSeen{-1};  // the first callback's thread under the time-constraint policy: 1, or 0
#endif
#ifdef __linux__
    // The device's thread names itself in its first callback, and `raiser` has it made real-time
    // (util/realtime_linux.h), which can take a D-Bus call that the callback must not wait for.
    std::atomic<pid_t> deviceThread{0};  // -1: closed first
    std::thread raiser;

    void startRaiser() {
        deviceThread = 0;
        raiser = std::thread([this] {
            deviceThread.wait(0);
            const pid_t tid = deviceThread.load();
            if (tid <= 0) return;
            realtime::fallBackOnOverrun();
            realtime::makeRealtime(getpid(), tid, "audio output thread");
        });
    }
    void stopRaiser() {
        if (!raiser.joinable()) return;
        pid_t none = 0;
        deviceThread.compare_exchange_strong(none, -1);
        deviceThread.notify_all();
        raiser.join();
    }
#endif

    static void onNotification(const ma_device_notification* n) {
        if (n->type == ma_device_notification_type_rerouted) static_cast<Impl*>(n->pDevice->pUserData)->moved = true;
    }

    static void callback(ma_device* dev, void* output, const void*, ma_uint32 frameCount) {
        auto* self = static_cast<Impl*>(dev->pUserData);
        setCurrentThreadIsAudio(true);
#ifdef __APPLE__
        if (self->realtimeSeen.load(std::memory_order_relaxed) < 0) self->realtimeSeen = realtime::thisThreadIsRealtime() ? 1 : 0;
#endif
#ifdef __linux__
        if (self->deviceThread.load(std::memory_order_relaxed) == 0) {
            pid_t none = 0;
            if (self->deviceThread.compare_exchange_strong(none, realtime::threadId())) self->deviceThread.notify_all();
        }
#endif
        float* out = static_cast<float*>(output);
        const uint32_t ch = self->channels;
        const uint32_t chunkMax = (uint32_t)self->planar[0].size();
        uint32_t done = 0;
        while (done < frameCount) {
            uint32_t n = std::min(frameCount - done, chunkMax);
            self->render(self->ptrs.data(), ch, n);
            for (uint32_t i = 0; i < n; ++i)
                for (uint32_t c = 0; c < ch; ++c) out[(done + i) * ch + c] = self->planar[c][i];
            done += n;
        }
    }
};

AudioOutput::AudioOutput() : impl_(std::make_unique<Impl>()) {}
AudioOutput::~AudioOutput() { close(); }

bool AudioOutput::open(const AudioOutputConfig& cfg, AudioRenderFn render, std::string& error) {
    close();
    std::lock_guard lock(g_contextMutex);
    auto& c = context();
    if (!c.ok) {
        error = "audio backend initialisation failed";
        return false;
    }

    ma_device_id id{};
    bool haveId = false;
    std::string chosenName;  // the named device's, or the default's
    {
        ma_device_info* infos = nullptr;
        ma_uint32 count = 0;
        if (ma_context_get_devices(&c.ctx, &infos, &count, nullptr, nullptr) == MA_SUCCESS) {
            for (ma_uint32 i = 0; i < count; ++i) {
                if (cfg.deviceName.empty() ? infos[i].isDefault != 0 : cfg.deviceName == infos[i].name) {
                    id = infos[i].id;
                    haveId = !cfg.deviceName.empty();
                    chosenName = infos[i].name;
                    break;
                }
            }
        }
        if (!cfg.deviceName.empty() && !haveId) {
            error = "audio device not found: " + cfg.deviceName;
            return false;
        }
    }

    ma_device_config dc = ma_device_config_init(ma_device_type_playback);
    dc.playback.pDeviceID = haveId ? &id : nullptr;
    dc.playback.format = ma_format_f32;
    dc.playback.channels = cfg.channels;
#ifdef _WIN32
    const bool exclusive = cfg.exclusive;
#else
    const bool exclusive = false;  // a session from Windows may ask for it
#endif
    dc.playback.shareMode = exclusive ? ma_share_mode_exclusive : ma_share_mode_shared;
    dc.sampleRate = exclusive ? cfg.sampleRate : 0;  // shared: device mix rate, no OS resampling
    dc.periodSizeInFrames = cfg.bufferFrames;
    dc.periods = 2;
    // PipeWire's ALSA plugin ("... (currently PipeWire Media Server)") takes a whole cycle of its
    // graph at once (the quantum: at least clock.min-quantum, which is 1024 frames in a VM, and
    // 2048 at most by default) and fills what the buffer does not hold with silence: two periods
    // of 256 frames sounded for half of every cycle. Periods enough for the largest quantum; a
    // period stays one callback.
    if (c.ctx.backend == ma_backend_alsa && chosenName.find("PipeWire") != std::string::npos)
        dc.periods = std::max<ma_uint32>(2, (2048 + cfg.bufferFrames - 1) / cfg.bufferFrames);
    dc.performanceProfile = ma_performance_profile_low_latency;
    dc.noPreSilencedOutputBuffer = MA_TRUE;
    dc.dataCallback = &Impl::callback;
    dc.notificationCallback = &Impl::onNotification;
    dc.pUserData = impl_.get();
#ifdef _WIN32
    dc.wasapi.noAutoConvertSRC = MA_TRUE;
    dc.wasapi.noDefaultQualitySRC = MA_TRUE;
    // The engine follows device changes itself (Engine::checkOutputRoute): it reopens
    // the new device at its own rate and converts with brack's resampler, where
    // miniaudio's rerouting would keep the old rate and convert with a linear one.
    dc.wasapi.noAutoStreamRouting = MA_TRUE;
    // The device's thread joins MMCSS's "Pro Audio" task when the device starts.
    dc.wasapi.usage = ma_wasapi_usage_pro_audio;
#endif

    impl_->render = std::move(render);
    if (ma_device_init(&c.ctx, &dc, &impl_->device) != MA_SUCCESS) {
        error = "failed to open audio device" + (cfg.deviceName.empty() ? std::string() : (": " + cfg.deviceName));
        return false;
    }
    impl_->open = true;
    impl_->movesWithDefault = !haveId && c.ctx.backend == ma_backend_pulseaudio;
    impl_->moved = false;
#ifdef __linux__
    impl_->startRaiser();
#endif
    impl_->channels = impl_->device.playback.channels;
    impl_->name = impl_->device.playback.name;
    impl_->key = deviceKeyOf(impl_->device.playback.id, impl_->device.playback.name);
    uint32_t chunk = std::max<uint32_t>(impl_->device.playback.internalPeriodSizeInFrames, cfg.bufferFrames) * 2;
    chunk = std::max<uint32_t>(chunk, 1024);
    impl_->planar.assign(impl_->channels, std::vector<float>(chunk, 0.0f));
    impl_->ptrs.resize(impl_->channels);
    for (uint32_t i = 0; i < impl_->channels; ++i) impl_->ptrs[i] = impl_->planar[i].data();
    return true;
}

void AudioOutput::close() {
    stop();
    if (impl_->open) {
        ma_device_uninit(&impl_->device);
        impl_->open = false;
    }
#ifdef __linux__
    impl_->stopRaiser();
#endif
}

bool AudioOutput::start(std::string& error) {
    if (!impl_->open) {
        error = "audio device not open";
        return false;
    }
#ifdef __APPLE__
    impl_->realtimeSeen = -1;
#endif
    if (ma_device_start(&impl_->device) != MA_SUCCESS) {
        error = "failed to start audio device";
        return false;
    }
#ifdef _WIN32
    realtime::report("audio output thread",
                     impl_->device.wasapi.hAvrtHandle ? "" : "the device's thread did not join MMCSS's Pro Audio task");
#elif defined(__APPLE__)
    // Core Audio's I/O thread has the policy of its own accord: seen in the first callback.
    for (int i = 0; i < 100 && impl_->realtimeSeen < 0; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    if (impl_->realtimeSeen >= 0)
        realtime::report("audio output thread", impl_->realtimeSeen ? "" : "Core Audio's thread is not time-constrained");
#endif
    impl_->running = true;
    return true;
}

void AudioOutput::stop() {
    if (impl_->open && impl_->running) {
        ma_device_stop(&impl_->device);
        impl_->running = false;
    }
}

bool AudioOutput::isOpen() const { return impl_->open; }
bool AudioOutput::isRunning() const { return impl_->running; }
uint32_t AudioOutput::sampleRate() const { return impl_->open ? impl_->device.sampleRate : 0; }
uint32_t AudioOutput::channels() const { return impl_->channels; }
uint32_t AudioOutput::periodFrames() const {
    return impl_->open ? impl_->device.playback.internalPeriodSizeInFrames : 0;
}
const std::string& AudioOutput::deviceName() const { return impl_->name; }
std::string AudioOutput::backendName() const { return impl_->open ? ma_get_backend_name(impl_->device.pContext->backend) : ""; }
const std::string& AudioOutput::deviceKey() const { return impl_->key; }
bool AudioOutput::movesWithDefault() const { return impl_->open && impl_->movesWithDefault; }
bool AudioOutput::takeMoved() { return impl_->moved.exchange(false); }
void AudioOutput::movedTo(const std::string& key) {
    impl_->name = key;
    impl_->key = key;
}

}  // namespace brack
