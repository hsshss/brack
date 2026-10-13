#pragma once
// The brack engine: hosts CLAP / VST3 / VST2 instruments, each with a chain of effects on its main
// output (plugin/chain.h), routes MIDI sources to plugin note ports and plugin audio outputs to
// output channels, runs the mix through the master's chain of effects, renders at an internal
// processing rate and converts to the device rate with a high quality SRC.
//
// Threads:
//   host thread  - owned by the engine; all mutations and plugin main-thread calls
//   audio thread - device callback (or the caller of render() in manual mode)
//   any thread   - public methods (they marshal onto the host thread), MIDI injection

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
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
#include "plugin/chain.h"
#include "util/arrival_clock.h"
#include "util/callback_cadence.h"
#include "util/frame_map.h"
#include "util/meters.h"

namespace brack {

class HostThread;
class MidiInputPort;
class MidiQueue;

enum class MidiSourceKind { Hardware, Virtual, Api };

// What became of a message given to Engine::sendMidiTo*.
enum class MidiSend { Sent, Malformed, UnknownId, QueueFull, NotLoaded };
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
        EditorClosed,           // id: the plugin whose editor closed, whatever closed it
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
    struct Level {  // a channel's
        float peak = 0;
        uint64_t clips = 0;  // blocks with a sample past full scale, since the meter began
    };
    struct Load {  // the time to process a block, against the time it lasts
        float average = 0;
        float peak = 0;  // the most within about the last second
    };

    EngineMode mode = EngineMode::Stopped;
    EngineConfig config;
    std::string deviceName;
    uint32_t outputSampleRate = 0, processSampleRate = 0, outputChannels = 0, periodFrames = 0;
    bool resampling = false;
    bool deviceStalled = false;  // device mode: the device stopped calling back (e.g. unplugged);
                                 // processing continues on brack's own clock until it returns
    double resamplerLatencyMs = 0;
    float masterGain = 1.0f;
    Load load;               // of render(), against the buffer it fills
    uint64_t overruns = 0;   // the device's callbacks, since it was opened, that took longer than their buffer lasts
    std::vector<Level> output;  // after the master gain
    // The mix before and after the master's chain (the same without one).
    std::vector<Level> masterInput, masterOutput;
    Load masterLoad;  // the master's chain's

    struct Plugin {
        std::string id, name, pluginId, path, status;
        PluginFormat format = PluginFormat::Clap;
        bool loaded = false, active = false, hasGui = false, guiOpen = false, failed = false;
        // The plugin's chain crashed and is no longer called (the status of the plugin that
        // crashed says where).
        bool crashed = false;
        std::string architecture;      // of its binary: "x64", "x86", ... (empty if not loaded)
        bool separateProcess = false;  // runs in a plugin host process (with its chain)
        std::vector<NotePortInfo> notePorts;
        std::vector<AudioPortInfo> audioOutputs;
        uint32_t latency = 0;
        bool bypass = false;          // an effect's
        std::vector<Plugin> effects;  // an instrument's chain, in order
        // An instrument's (an effect has none): its chain's output channels, port after port
        // as audioOutputs lists them, before the routes' gains; its chain's load; the MIDI
        // messages it took (once each, however many of its note ports).
        std::vector<Level> outputLevels;
        Load load;
        uint64_t midiMessages = 0;
    };
    struct Source {
        std::string id, name, status;
        MidiSourceKind kind = MidiSourceKind::Api;
        bool ok = false;
        uint64_t messages = 0, dropped = 0;
    };
    std::vector<Plugin> plugins;
    std::vector<Plugin> masterEffects;
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
    // An instrument goes with its effects.
    bool removePlugin(const std::string& id, std::string& error);
    // Loads the plugin's chain again as new instances, with the states a session would save for
    // them, keeping their ids, names and routes: for a chain that crashed or a plugin that failed
    // to load. A plugin that crashed in this process cannot load again until Brack restarts; one
    // from a plugin host process can.
    bool reloadPlugin(const std::string& id, std::string& error);
    // Adds an effect to the chain of the instrument `owner`, or of the master if it is empty, at
    // `index` among its effects (clamped). It must be built for the instrument's architecture
    // (the master's: Brack's). Returns its id, generated if cfg.id is empty, unique among every
    // plugin's; or empty on error. The methods that take a plugin id take an effect's too, but
    // for routing and MIDI; moving it moves it among its chain's effects. For a chain in a plugin
    // host the effect loads on the calling thread; it fails if the chain is removed or loaded
    // again meanwhile.
    std::string addEffect(const std::string& owner, size_t index, const PluginConfig& cfg, std::string& error);
    // A bypassed effect is not processed: what comes to it goes on unchanged.
    bool setEffectBypass(const std::string& id, bool bypass, std::string& error);
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
    // Any thread. Output frames rendered since the engine started (manual or device mode; a
    // restart by setConfig runs on): the clock MIDI times refer to. Without sample rate conversion, a message timed t lands at
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
    // brack_describe_session's JSON; any thread, no engine. False, with `error`, if unreadable.
    static bool describeSessionFile(const std::string& pathUtf8, std::string& out, std::string& error);
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
    struct EffectSlot;
    struct SourceSlot;
    struct RtGraph;

private:
    // PluginHostListener (host thread)
    void pluginGuiClosed(HostedPlugin& p) override;
    void pluginEditorResized(HostedPlugin& p, uint32_t width, uint32_t height) override;
    void pluginStateChanged(HostedPlugin& p) override;  // any thread
    void markChanged() { changeSeq_.fetch_add(1, std::memory_order_relaxed); }

    // host thread helpers
    // Continue: a restart for a new configuration keeps the MIDI still waiting, and render
    // positions run on, so a note-off sent meanwhile is not lost.
    enum class Timeline { Fresh, Continue };
    bool runDevice(Timeline t, std::string& error);
    bool runManual(Timeline t, uint32_t sampleRate, uint32_t channels, uint32_t maxFrames, std::string& error);
    void stopRunning();
    bool startRunning(Timeline t, EngineMode mode, uint32_t outRate, uint32_t channels, uint32_t maxOutFrames,
                      std::string& error);
    bool activateSlot(PluginSlot& s, std::string& error);
    // Every loaded chain: those in plugin hosts at once, each host answering on its own; those in
    // this process one after another on this thread, as their formats require.
    void activateSlots();
    // Stops the slot's chain and takes it out of processing. Its MIDI waits until settleSlot(),
    // unless `removing`: then the slot leaves the graph.
    void quiesceSlot(PluginSlot& s, bool removing = false);
    void resumeSlot(PluginSlot& s);  // after quiesceSlot(): processing again, and settleSlot()
    // After the slot's chain changed or was made: into the graph, its MIDI passed on again; the
    // master's goes once empty.
    void settleSlot(PluginSlot& s);
    void publishGraph();
    void onIdle();
    // An id from `base` that no plugin (`plugin`) or MIDI source has, nor `adding`, a slot not yet
    // in the rack.
    std::string uniqueId(const std::string& base, bool plugin, const PluginSlot* adding = nullptr) const;
    // Before a slot goes in the rack (or the master's, its effects set): an id for each plugin
    // without one, and a new one, with a warning, for an effect whose id is taken (an effect's id is
    // in no route; a taken instrument id is the caller's to refuse).
    void assignIds(PluginSlot& s) const;
    // An instrument's slot.
    PluginSlot* findPlugin(const std::string& id) const;
    // Any plugin by id: an instrument (`effect` null) or an effect of `slot`'s chain.
    struct PluginRef {
        PluginSlot* slot = nullptr;
        EffectSlot* effect = nullptr;
        explicit operator bool() const { return slot != nullptr; }
        PluginConfig& cfg() const;
        HostedPlugin* inst() const;  // null while it is not loaded
    };
    PluginRef findAny(const std::string& id) const;
    PluginRef findAny(const HostedPlugin& inst) const;
    // The instruments' slots, then the master's, without allocating. The rack must not change
    // while it is walked.
    struct SlotRange {
        const std::vector<std::unique_ptr<PluginSlot>>& plugins;
        PluginSlot* master;
        struct Iterator {
            const SlotRange* range;
            size_t i;
            PluginSlot* operator*() const { return i < range->plugins.size() ? range->plugins[i].get() : range->master; }
            Iterator& operator++() {
                ++i;
                return *this;
            }
            bool operator!=(const Iterator& o) const { return i != o.i; }
        };
        Iterator begin() const { return {this, 0}; }
        Iterator end() const { return {this, plugins.size() + (master ? 1 : 0)}; }
    };
    SlotRange slots() const { return {plugins_, master_.get()}; }
    bool isMaster(const PluginSlot& s) const { return &s == master_.get(); }
    SourceSlot* findSource(const std::string& id) const;
    void openSourcePort(SourceSlot& s, bool quiet = false);
    // The slot's chain, with its instrument (none for the master) given cfg's saved state
    // (`stateRejected`: it would not take it), and its effects with theirs (an effect that does
    // not load is left out, its slot saying why). In this process or a plugin host process, as the
    // configuration and the instrument's binary say: one in a plugin host is made on the calling
    // thread, any thread; one in this process, on the host thread. Null, with `error`, if the
    // instrument does not load (or the master's plugin host does not start).
    std::unique_ptr<HostedChain> makeChain(PluginSlot& s, bool& stateRejected, std::string& error);
    // An effect from its slot, in two steps (HostedChain::prepareEffect()): made for `chain`, with
    // its state and name, while the chain may be processed; then put in it at `index`, while it is
    // not. Each fails (null, false) with the slot's status saying why.
    HostedPlugin* prepareEffect(HostedChain& chain, EffectSlot& e);
    bool insertEffect(HostedChain& chain, size_t index, EffectSlot& e);
    // addEffect()'s insert step (host thread). `fresh`: the master's chain was made for `e`.
    std::string addPreparedEffect(PluginSlot& s, size_t index, EffectSlot e, bool prepared, bool fresh,
                                  std::string& error);
    // Host thread: puts a slot with a chain from makeChain() in the rack. Its id, or empty with `error`.
    std::string installPlugin(std::unique_ptr<PluginSlot> slot, bool stateRejected, bool autoRouteAudio,
                              std::string& error);
    bool reloadSlot(PluginSlot& s, std::string& error);
    // About once a second: notice hardware inputs whose connection was closed, and reopen
    // hardware inputs that failed once their port is listed again.
    void checkMidiInputs();
    void rebuildInjectionMaps();
    void pushEvent(EngineEvent e);  // host thread
    void checkEvents();             // host thread, every idle pass
    // The chains' meters a snapshot shows: its instruments', in its order, and the master's.
    struct SnapshotMeters {
        std::vector<std::shared_ptr<const ChainMeters>> plugins;
        std::shared_ptr<const ChainMeters> master;
    };
    // Host thread: all but the meters, which readMeters() fills in on any thread.
    EngineSnapshot buildSnapshot(SnapshotMeters& meters) const;
    void readMeters(EngineSnapshot& s, const SnapshotMeters& meters) const;
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
    std::unique_ptr<PluginSlot> master_;  // no instrument; a chain while it has effects
    std::vector<std::unique_ptr<SourceSlot>> sources_;
    std::vector<MidiRoute> midiRoutes_;
    std::vector<AudioRoute> audioRoutes_;
    // Chains an addEffect() prepares an effect for off the host thread: one at a time per chain.
    std::mutex addingMutex_;
    std::condition_variable addingDone_;
    std::vector<const HostedChain*> adding_;
    uint32_t outRate_ = 0, procRate_ = 0, outChannels_ = 0, periodFrames_ = 0;

    // injection lookup (shared lock for senders, exclusive on host-thread changes); looked up by
    // string_view, so a sender on a real-time thread does not allocate a key
    mutable std::shared_mutex injectMutex_;
    std::map<std::string, PluginSlot*, std::less<>> injectPlugins_;
    std::map<std::string, MidiQueue*, std::less<>> injectSources_;
    MidiSend pushToPlugin(std::string_view id, const uint8_t* data, uint32_t size, uint16_t notePort, uint64_t time,
                          int64_t timeNs);

    // published to the audio thread
    std::atomic<RtGraph*> graph_{nullptr};
    std::atomic<bool> rtReady_{false};   // real-time buffers below are valid
    std::atomic<bool> holdMidi_{false};  // stopping: MIDI stays queued, not given to plugins that drop it

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
    SnapshotMeters cachedMeters_;  // cached_'s
    std::chrono::steady_clock::time_point lastCachePublish_{};  // host thread
    std::atomic<uint64_t> audioSeq_{0};  // odd while the audio thread is inside render()

    // audio-thread owned (sized on the host thread while stopped)
    std::unique_ptr<MultiChannelResampler> resampler_;
    std::vector<std::vector<float>> mix_;
    std::vector<float*> mixPtrs_;
    std::vector<uint8_t> scratch_;
    InputEventList noEvents_{1, 0};  // the master's
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

    // meters (written by the audio thread)
    LoadMeter load_;
    std::atomic<uint64_t> overruns_{0};
    std::array<LevelMeter, 32> outputMeters_, masterInMeters_, masterOutMeters_;
};

}  // namespace brack
