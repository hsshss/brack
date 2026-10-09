#pragma once
// The brack engine: hosts CLAP / VST3 / VST2 instruments, routes MIDI sources to plugin note
// ports and plugin audio outputs to output channels, renders at an internal
// processing rate and converts to the device rate with a high quality SRC.
//
// Threads:
//   host thread  - owned by the engine; all mutations and plugin main-thread calls
//   audio thread - device callback (or the caller of render() in manual mode)
//   any thread   - public methods (they marshal onto the host thread), MIDI injection

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "audio/audio_output.h"
#include "audio/resampler.h"
#include "plugin/plugin.h"
#include "util/arrival_clock.h"
#include "util/callback_cadence.h"
#include "util/frame_map.h"

namespace brack {

class HostThread;
class MidiInputPort;
class MidiQueue;

enum class MidiSourceKind { Hardware, Virtual, Api };

// What became of a message given to Engine::sendMidiTo*.
enum class MidiSend { Sent, Malformed, UnknownId, QueueFull };
const char* midiSourceKindName(MidiSourceKind k);
bool parseMidiSourceKind(const std::string& s, MidiSourceKind& out);

struct MidiSourceConfig {
    std::string id;    // unique; generated when empty
    MidiSourceKind kind = MidiSourceKind::Api;
    std::string name;  // hardware: port name to open; virtual: port name to publish
};

struct PluginConfig {
    std::string id;        // unique; generated when empty
    std::string path;      // .clap, .vst3 or VST2 .dll (UTF-8); the extension decides the format
    std::string pluginId;  // plugin inside the file (CLAP id, VST3 class id, VST2 unique id); first when empty
    std::string name;      // display name; plugin name when empty
    std::optional<std::vector<uint8_t>> state;
};

struct MidiRoute {
    std::string source;
    std::string plugin;
    uint16_t notePort = 0;
    bool operator==(const MidiRoute&) const = default;
};

struct AudioRoute {
    std::string plugin;
    uint32_t port = 0;     // plugin audio output port index
    uint32_t channel = 0;  // channel within that port
    uint32_t output = 0;   // engine output channel
    float gain = 1.0f;
};

struct EngineConfig {
    AudioOutputConfig audio;
    uint32_t processSampleRate = 0;  // plugin rate; 0 = same as the output
    uint32_t blockSize = 256;        // max frames per plugin process() call
    ResamplerQuality resamplerQuality = ResamplerQuality::High;
    // Plugins run in this process rather than in a plugin host process each. Faster to load,
    // but a plugin that crashes outside a call from Brack, or hangs, takes Brack down with it.
    // Plugins built for another architecture than Brack's run in a plugin host process anyway.
    bool pluginsInProcess = false;
    // A session's plugins are loaded and activated one after another rather than all at once, for
    // plugins that fail when two of them start together (design notes, "起動").
    bool loadPluginsSerially = false;
};

enum class EngineMode { Stopped, Device, Manual };

// Something that happened in the engine that a client may want to react to (see pollEvent()).
struct EngineEvent {
    enum class Type {
        Changed,                // what a session saves changed (see changeCount()); at most one queued
        PluginCrashed,          // id: the plugin; message: where it crashed
        EditorClosed,           // id: the plugin whose editor closed (by the user or a call)
        EditorResized,          // id: the plugin; width/height: its editor's new size, physical pixels
        DeviceStalled,          // the output device stopped calling back (see EngineSnapshot::deviceStalled)
        DeviceResumed,          // and is back
        MidiSourceLost,         // id: the MIDI source whose port went away; message: why
        MidiSourceReconnected,  // id: the MIDI source, open again
    };
    Type type = Type::Changed;
    std::string id;
    std::string message;
    uint32_t width = 0, height = 0;
};

struct EngineSnapshot {
    EngineMode mode = EngineMode::Stopped;
    EngineConfig config;
    std::string deviceName;
    uint32_t outputSampleRate = 0, processSampleRate = 0, outputChannels = 0, periodFrames = 0;
    bool resampling = false;
    bool deviceStalled = false;  // device mode: the device stopped calling back (e.g. unplugged);
                                 // processing continues on brack's own clock until it returns
    double resamplerLatencyMs = 0;
    float masterGain = 1.0f;
    float cpuLoad = 0;
    std::vector<float> outputPeaks;

    struct Plugin {
        std::string id, name, pluginId, path, status;
        PluginFormat format = PluginFormat::Clap;
        bool loaded = false, active = false, hasGui = false, guiOpen = false, failed = false;
        bool crashed = false;  // the plugin crashed and is no longer called (status says where)
        std::string architecture;      // of its binary: "x64", "x86", ... (empty if not loaded)
        bool separateProcess = false;  // runs in a plugin host process of its own
        std::vector<NotePortInfo> notePorts;
        std::vector<AudioPortInfo> audioOutputs;
        uint32_t latency = 0;
    };
    struct Source {
        std::string id, name, status;
        MidiSourceKind kind = MidiSourceKind::Api;
        bool ok = false;
        uint64_t messages = 0, dropped = 0;
    };
    std::vector<Plugin> plugins;
    std::vector<Source> sources;
    std::vector<MidiRoute> midiRoutes;
    std::vector<AudioRoute> audioRoutes;
};

class Engine final : private PluginHostListener {
public:
    Engine();
    ~Engine() override;
    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;

    // ---- configuration / transport ----
    // Applies a new configuration; restarts the engine if it is running.
    bool setConfig(const EngineConfig& cfg, std::string& error);
    EngineConfig config() const;
    bool startDevice(std::string& error);
    // Manual mode: no device; the caller drives render() from its own audio thread.
    bool startManual(uint32_t sampleRate, uint32_t channels, uint32_t maxFrames, std::string& error);
    void stop();
    // Audio thread (manual mode). out[c] receives `frames` samples per channel.
    void render(float* const* out, uint32_t channels, uint32_t frames);

    // ---- plugins ----
    // Longest id the engine generates for a plugin or MIDI source, in bytes.
    static constexpr size_t kMaxGeneratedId = 63;
    // Returns the instance id (generated if cfg.id is empty), or empty on error.
    std::string addPlugin(const PluginConfig& cfg, bool autoRouteAudio, std::string& error);
    bool removePlugin(const std::string& id, std::string& error);
    // Loads the plugin again as a new instance, with the state a session would save for it, keeping
    // its id, name and routes: for a plugin that crashed or failed to load. A plugin that crashed
    // in this process cannot load again until Brack restarts; one from a plugin host process can.
    bool reloadPlugin(const std::string& id, std::string& error);
    bool setPluginGuiVisible(const std::string& id, bool visible, std::string& error);
    // Opens the editor as a child of `parent`, a window of the caller's (of the kind
    // HostWindow::nativeHandle() describes), at its top left; EngineEvent::EditorResized reports
    // its size. Close it (setPluginGuiVisible) before destroying the parent.
    bool showPluginGuiIn(const std::string& id, void* parent, std::string& error);
    // The plugin's state as a session saves it; for a plugin that failed to load, the state it
    // was given. Setting it on a plugin that failed to load keeps it for the next save.
    bool getPluginState(const std::string& id, std::vector<uint8_t>& out, std::string& error);
    bool setPluginState(const std::string& id, const std::vector<uint8_t>& state, std::string& error);
    // Display name only; the id (used by routes, sessions and the API) stays.
    // An empty name reverts to the plugin's own name.
    bool setPluginName(const std::string& id, const std::string& name, std::string& error);
    // Moves a plugin / MIDI source to `index` in the list (clamped). The order is what
    // snapshots, sessions and UIs show; processing does not depend on it.
    bool movePlugin(const std::string& id, size_t index, std::string& error);
    bool moveMidiSource(const std::string& id, size_t index, std::string& error);

    // ---- MIDI sources ----
    std::string addMidiSource(const MidiSourceConfig& cfg, std::string& error);
    bool removeMidiSource(const std::string& id, std::string& error);
    bool reopenMidiSource(const std::string& id, std::string& error);

    // ---- routing ----
    bool addMidiRoute(const MidiRoute& r, std::string& error);
    bool removeMidiRoute(const MidiRoute& r);
    bool setAudioRoutes(const std::string& plugin, const std::vector<AudioRoute>& routes, std::string& error);
    // One route more or less. Routes are told apart by plugin, port, channel and output (not gain).
    bool addAudioRoute(const AudioRoute& r, std::string& error);
    bool removeAudioRoute(const AudioRoute& r);
    // Linear gain on every output channel, after the SRC (1 = unity, 0 = silent). Any
    // thread, takes effect on the next buffer with a ramp. False for a negative or NaN gain.
    bool setMasterGain(float gain);
    float masterGain() const { return masterGain_.load(std::memory_order_relaxed); }

    // ---- direct MIDI injection (any thread, never blocks on the engine) ----
    // One complete MIDI 1.0 message per call (SysEx as F0..F7), delivered unchanged.
    // `time`: the render position (renderPosition()) to deliver it at, frame accurate. A time
    // already rendered means as soon as possible. kMidiNow: with a device, a period after the
    // call, where in the period it came (ArrivalClock); in manual mode, as soon as possible.
    static constexpr uint64_t kMidiNow = UINT64_MAX;
    MidiSend sendMidiToPlugin(std::string_view pluginId, uint16_t notePort, const uint8_t* data, uint32_t size,
                              uint64_t time = kMidiNow);
    MidiSend sendMidiToSource(std::string_view sourceId, const uint8_t* data, uint32_t size, uint64_t time = kMidiNow);
    // Any thread. Brack's clock: steady_clock nanoseconds, the clock the times below are on.
    static int64_t nowNs();
    // Delivered at `timeNs` on Brack's clock: with a device, a period after that moment, as a
    // message sent then with kMidiNow; a moment past, at once; in manual mode, as kMidiNow.
    MidiSend sendMidiToPluginAtTime(std::string_view pluginId, uint16_t notePort, const uint8_t* data, uint32_t size,
                                    int64_t timeNs);
    MidiSend sendMidiToSourceAtTime(std::string_view sourceId, const uint8_t* data, uint32_t size, int64_t timeNs);
    // Any thread. Output frames rendered since the engine started (manual or device mode): the
    // clock MIDI times refer to. Without sample rate conversion, a message timed t lands at
    // frame t exactly; with it, the conversion's latency delays everything alike, and a message
    // sent less than a plugin block ahead may land up to a block late.
    uint64_t renderPosition() const { return renderPos_.load(std::memory_order_acquire); }

    // ---- session ----
    std::string saveSessionJson();
    // Replaces the rack with the session's, or (false, with `error`) leaves it untouched when
    // the session cannot be read. A running engine restarts with the session's configuration;
    // if that fails the load still succeeds and the engine is left stopped (logged).
    bool loadSessionJson(const std::string& json, std::string& error);
    bool saveSessionFile(const std::string& pathUtf8, std::string& error);
    bool loadSessionFile(const std::string& pathUtf8, std::string& error);
    void clear();

    // ---- inspection ----
    EngineSnapshot snapshot() const;
    // The latest snapshot without waiting for the host thread, which may be busy
    // (loading a plugin, say). Refreshed there every ~50 ms and by refreshSnapshot();
    // meters and the stall flag are always current. For UIs that must not block.
    EngineSnapshot cachedSnapshot() const;
    void refreshSnapshot();
    // Any thread. Goes up whenever what saveSessionJson() would write changes: the rack, the
    // routing, the configuration, or a plugin's state (as the plugin reports it, e.g. an edit
    // in its editor). MIDI the plugins receive does not count. For autosaving.
    uint64_t changeCount() const { return changeSeq_.load(std::memory_order_relaxed); }

    // ---- events ----
    // Any thread. Takes the oldest queued event; false if there is none. The queue keeps the
    // latest 1024.
    bool pollEvent(EngineEvent& out);
    // Called (on the host thread, so briefly) whenever an event is queued; for a client that
    // wants to wake its own thread rather than poll. Null to stop.
    void setEventNotify(std::function<void()> notify);
    // Any thread; runs on the caller's, without holding up the engine. Plugins in extraDirs, and
    // in the standard locations unless told not to. With a cache file, files unchanged since it
    // was written are not loaded again (VST2 plugins are instantiated to be described), and it
    // is updated.
    static std::vector<PluginDescription> scanPlugins(const std::vector<std::string>& extraDirs = {},
                                                      const std::string& cachePathUtf8 = {},
                                                      bool standardLocations = true);

    struct PluginSlot;
    struct SourceSlot;
    struct RtGraph;

private:
    // PluginHostListener (host thread)
    void pluginGuiClosed(HostedPlugin& p) override;
    void pluginEditorResized(HostedPlugin& p, uint32_t width, uint32_t height) override;
    void pluginStateChanged(HostedPlugin& p) override;  // any thread
    void markChanged() { changeSeq_.fetch_add(1, std::memory_order_relaxed); }

    // host thread helpers
    void stopRunning();
    bool startRunning(EngineMode mode, uint32_t outRate, uint32_t channels, uint32_t maxOutFrames, std::string& error);
    bool activateSlot(PluginSlot& s, std::string& error);
    // Every loaded plugin: those in plugin hosts at once, each host answering on its own; those in
    // this process one after another on this thread, as their formats require.
    void activateSlots();
    void quiesceSlot(PluginSlot& s);  // stop processing + remove from graph
    void publishGraph(const PluginSlot* exclude = nullptr);
    void onIdle();
    std::string uniqueId(const std::string& base, bool plugin) const;
    PluginSlot* findPlugin(const std::string& id) const;
    SourceSlot* findSource(const std::string& id) const;
    void openSourcePort(SourceSlot& s, bool quiet = false);
    // A plugin made and given cfg's saved state (`stateRejected`: it would not take it), not yet
    // in the rack. In this process or a plugin host process, as the configuration and the binary
    // say: one in a plugin host is made on the calling thread, any thread; one in this process,
    // on the host thread. Null, with `error`, on failure.
    std::unique_ptr<HostedPlugin> makePlugin(const PluginConfig& cfg, bool& stateRejected, std::string& error);
    // Host thread: puts a plugin from makePlugin() in the rack. Its id, or empty with `error`.
    std::string installPlugin(PluginConfig cfg, std::unique_ptr<HostedPlugin> inst, bool stateRejected,
                              bool autoRouteAudio, std::string& error);
    bool reloadSlot(PluginSlot& s, std::string& error);
    // About once a second: notice hardware inputs whose connection was closed, and reopen
    // hardware inputs that failed once their port is listed again.
    void checkMidiInputs();
    void rebuildInjectionMaps();
    PluginSlot* findPlugin(const HostedPlugin& inst) const;
    void pushEvent(EngineEvent e);  // host thread
    void checkEvents();             // host thread, every idle pass
    EngineSnapshot buildSnapshot() const;
    void publishCachedSnapshot();

    // audio thread
    void processBlock(RtGraph& g, uint32_t frames);
    void deviceRender(float* const* out, uint32_t channels, uint32_t frames);  // device callback

    // Device mode keeps time even when the device does not: if its callbacks stop
    // (unplugged, driver reset), this thread renders in real time and discards the
    // output, so plugins and MIDI move on and nothing is replayed in a burst later.
    // freshDevice: a device was just (re)started, give it time to deliver.
    // keepState: an ongoing stall carries on into the next startClock().
    void startClock(bool freshDevice = true);
    void stopClock(bool keepState = false);
    void runClock();
    int64_t stallNs() const;  // how long a device is silent before it is taken to have stopped (any thread)

    // Device routing (host thread, about once a second): follow the system default
    // output when configured to, and reopen a device that went away.
    bool configureOutput(uint32_t outRate, uint32_t channels, uint32_t maxOutFrames, std::string& error);
    void checkOutputRoute();
    void switchOutputDevice();

    std::unique_ptr<HostThread> host_;  // first member: destroyed last
    uint32_t idleTimer_ = 0;

    // host-thread state
    EngineConfig config_;
    EngineMode mode_ = EngineMode::Stopped;
    AudioOutput device_;
    std::vector<std::unique_ptr<PluginSlot>> plugins_;
    std::vector<std::unique_ptr<SourceSlot>> sources_;
    std::vector<MidiRoute> midiRoutes_;
    std::vector<AudioRoute> audioRoutes_;
    uint32_t outRate_ = 0, procRate_ = 0, outChannels_ = 0, periodFrames_ = 0;

    // injection lookup (shared lock for senders, exclusive on host-thread changes); looked up by
    // string_view, so a sender on a real-time thread does not allocate a key
    mutable std::shared_mutex injectMutex_;
    std::map<std::string, MidiQueue*, std::less<>> injectPlugins_, injectSources_;

    // published to the audio thread
    std::atomic<RtGraph*> graph_{nullptr};
    std::atomic<bool> rtReady_{false};   // real-time buffers below are valid

    // fallback clock (device mode)
    std::thread clock_;
    std::atomic<bool> clockQuit_{false};
    std::atomic<bool> clockActive_{false};             // rendering in place of the device
    std::atomic<bool> renderBusy_{false};              // device callback and clock never render together
    std::atomic<int64_t> lastDeviceCallbackNs_{0};     // steady_clock, last device callback
    int64_t clockSinceNs_ = 0;                          // clock thread (or host thread while it is stopped)
    uint64_t clockRendered_ = 0;
    bool routeFailureLogged_ = false;                   // host thread
    std::string defaultSeen_;                           // host thread: a new default, not yet followed
    std::chrono::steady_clock::time_point lastRouteCheck_{};  // host thread

    // cached snapshot (written on the host thread, read anywhere)
    mutable std::mutex cachedMutex_;
    EngineSnapshot cached_;
    std::chrono::steady_clock::time_point lastCachePublish_{};  // host thread
    std::atomic<uint64_t> audioSeq_{0};  // odd while the audio thread is inside render()

    // audio-thread owned (sized on the host thread while stopped)
    std::unique_ptr<MultiChannelResampler> resampler_;
    std::vector<std::vector<float>> mix_;
    std::vector<float*> mixPtrs_;
    std::vector<uint8_t> scratch_;
    uint32_t blockSize_ = 256;
    uint64_t steadyTime_ = 0;                 // process frames since the start
    std::atomic<uint64_t> renderPos_{0};      // output frames since the start (renderPosition())
    FrameMap frameMap_;                       // renderPos_ to steadyTime_, for MIDI times
    ArrivalClock arrival_;                    // when a message without a time arrived to its position
    CallbackCadence cadence_;                 // device callback thread; reset while nothing renders
    std::atomic<int64_t> callbackGapNs_{0};   // cadence_.longestGapNs(), for stallNs() on other threads

    std::atomic<float> masterGain_{1.0f};
    std::atomic<uint64_t> changeSeq_{0};  // see changeCount()

    // events
    mutable std::mutex eventMutex_;
    std::deque<EngineEvent> events_;
    bool changedQueued_ = false;              // a Changed event waits in events_
    std::function<void()> eventNotify_;       // host thread
    uint64_t changeSeen_ = 0;                 // host thread: changeSeq_ at the last Changed event
    bool stallSeen_ = false;                  // host thread
    float masterApplied_ = 1.0f;  // audio thread: the gain the last buffer ended on

    // meters
    std::atomic<float> cpuLoad_{0};
    std::array<std::atomic<float>, 32> peaks_{};
};

}  // namespace brack
