#pragma once
// Format-independent plugin interface. The engine sees every plugin (CLAP, VST2, VST3), in this
// process or another, through HostedPlugin; MIDI reaches it as an InputEventList, which uses
// CLAP's event structs as brack's own event representation.

#include <clap/clap.h>

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <type_traits>
#include <vector>

#include "plugin/host_window.h"
#include "plugin/library.h"

namespace brack {

class HostThread;
class HostedPlugin;

enum class PluginFormat { Clap, Vst2, Vst3 };
const char* pluginFormatName(PluginFormat f);   // "clap", "vst2", "vst3"
const char* pluginFormatLabel(PluginFormat f);  // "CLAP", "VST2", "VST3"
// The format of a plugin file: pluginFormatFromPath() in plugin/plugin_files.h.

struct PluginDescription {
    PluginFormat format = PluginFormat::Clap;
    std::string path;  // UTF-8 path of the plugin file (or .vst3 bundle)
    std::string id;    // plugin inside the file: CLAP id, VST2 unique id, VST3 class id
    std::string name;
    std::string vendor;
    std::string version;
    std::string description;
    std::vector<std::string> features;  // CLAP features, VST3 sub-categories
    bool instrument = false;
    // The binary's architecture, a buildArchitecture() name ("x64", "x86", ...). It may differ
    // from Brack's own: such a plugin runs in a plugin host process of its architecture.
    std::string architecture;
    bool isInstrument() const { return instrument; }
};

// "name [CLAP]"; a plugin of another architecture than Brack's says which, "name [CLAP, x64]":
// the same plugin installed for two architectures would otherwise be two identical rows.
std::string pluginLabel(const PluginDescription& d);

// Implemented by the engine. Calls arrive on the host thread unless noted.
class PluginHostListener {
public:
    virtual ~PluginHostListener() = default;
    virtual void pluginGuiClosed(HostedPlugin& p) = 0;
    // The editor's window changed size (it asked to, or was resized), in physical pixels.
    virtual void pluginEditorResized(HostedPlugin& p, uint32_t width, uint32_t height) = 0;
    // Any thread, including the audio thread; must not block. The plugin says its state
    // changed (a parameter edited in its editor, say): what a session saves is now different.
    virtual void pluginStateChanged(HostedPlugin& p) = 0;
};

struct NotePortInfo {
    clap_id id = 0;
    std::string name;
    uint32_t dialects = 0;  // CLAP_NOTE_DIALECT_*; VST2/VST3 ports report MIDI
};

struct AudioPortInfo {
    clap_id id = 0;
    std::string name;
    uint32_t channels = 0;
    bool isMain = false;
};

// Real-time event list handed to HostedPlugin::beginProcess(). Pre-allocated; the
// audio thread fills it without allocating.
class InputEventList {
public:
    explicit InputEventList(size_t maxEvents = 4096, size_t sysexArenaBytes = 256 * 1024);
    void clear() {
        count_ = 0;
        arenaUsed_ = 0;
    }
    // `time`: frame offset within the block being processed.
    bool pushMidi(uint16_t port, const uint8_t* data, uint32_t size, uint32_t time = 0);  // CLAP_EVENT_MIDI / _SYSEX
    bool pushNote(uint16_t type, uint16_t port, uint8_t channel, uint8_t key, double velocity, uint32_t time = 0);
    // Orders the events by time, keeping the order of events at the same time (formats and
    // CLAP plugins expect them sorted). Without allocating.
    void sortByTime();
    const clap_input_events_t* get() const { return &clap_; }
    size_t size() const { return count_; }
    const clap_event_header_t* at(size_t i) const { return &events_[i].header; }

private:
    union Event {
        clap_event_header_t header;
        clap_event_midi_t midi;
        clap_event_midi_sysex_t sysex;
        clap_event_note_t note;
    };
    static uint32_t sizeCb(const clap_input_events_t* l);
    static const clap_event_header_t* getCb(const clap_input_events_t* l, uint32_t i);
    std::vector<Event> events_;
    size_t count_ = 0;
    std::vector<uint8_t> arena_;
    size_t arenaUsed_ = 0;
    clap_input_events_t clap_;
};

// An open plugin editor. Lives on the host thread.
class PluginEditor {
public:
    virtual ~PluginEditor() = default;
    virtual void show() = 0;
    virtual void hide() = 0;
    virtual void setTitle(const std::string& title) = 0;
    // The plugin crashed: hide the window and stop forwarding anything to the plugin, without
    // calling it (not even to tear its view down). The editor object is then leaked.
    virtual void abandon() = 0;
};

// A plugin as the engine sees it: in this process (PluginInstance) or in a plugin host process
// of its own (RemotePlugin). Calls are on the host thread unless noted.
class HostedPlugin {
public:
    virtual ~HostedPlugin() = default;
    HostedPlugin(const HostedPlugin&) = delete;
    HostedPlugin& operator=(const HostedPlugin&) = delete;

    // Creates the plugin; afterwards description() and hasGui() are final.
    virtual bool init(std::string& error) = 0;
    // Afterwards the ports are final until the next activation.
    virtual bool activate(double sampleRate, uint32_t maxFrames, std::string& error) = 0;
    virtual void deactivate() = 0;
    virtual bool isActive() const = 0;
    virtual void idle() = 0;  // about every 10 ms: deferred plugin requests, editor idle; reports a crash

    // A crashed plugin answers with the state it last saved or loaded.
    virtual bool saveState(std::vector<uint8_t>& out) = 0;
    virtual bool loadState(const std::vector<uint8_t>& in) = 0;

    bool hasGui() const { return hasGui_; }
    virtual bool isGuiOpen() const = 0;
    // Opens (or shows) the editor: in a top-level window of its own, or in `parent`, a window
    // of the application's (HWND on Windows), as a child at its top left. An editor open in
    // another place is closed and opened again in this one.
    virtual bool openGui(std::string& error, void* parent = nullptr) = 0;
    virtual void closeGui() = 0;

    // Stop/start handshake with the audio thread (formats start and stop processing there).
    virtual void requestStopProcessing() = 0;
    virtual bool stopProcessingAcknowledged() const = 0;

    const std::vector<NotePortInfo>& notePorts() const { return notePorts_; }
    const std::vector<AudioPortInfo>& audioInputs() const { return audioIns_; }
    const std::vector<AudioPortInfo>& audioOutputs() const { return audioOuts_; }
    uint32_t latency() const { return isActive() ? latency_.load(std::memory_order_relaxed) : 0; }
    const PluginDescription& description() const { return desc_; }
    virtual void setDisplayName(std::string name) = 0;  // also retitles an open editor
    virtual bool restartPending() const = 0;
    virtual bool processFailed() const = 0;
    virtual bool crashed() const = 0;
    // What the crash was ("crashed in process: access violation at X.dll+0x1234").
    virtual std::string crashReport() const = 0;
    // Runs in a plugin host process rather than in this one.
    virtual bool separateProcess() const = 0;

    // ---- audio thread ----
    // Appends one MIDI message for note port `port`, at frame `time` of the block. Raw MIDI is
    // passed through untouched whenever the format allows it.
    virtual void appendMidi(InputEventList& list, uint16_t port, const uint8_t* data, uint32_t size,
                            uint32_t time) const = 0;
    // Processes `frames` frames, in two steps so that plugins in other processes run side by
    // side: beginProcess() for every plugin of the block, then endProcess() for each, which
    // returns false when the plugin produced no audio. `in` stays untouched until then.
    virtual void beginProcess(uint32_t frames, uint64_t steadyTime, const InputEventList& in) = 0;
    virtual bool endProcess() = 0;
    virtual const float* outputChannel(uint32_t port, uint32_t channel) const = 0;

protected:
    explicit HostedPlugin(PluginDescription desc) : desc_(std::move(desc)) {}

    PluginDescription desc_;
    std::vector<NotePortInfo> notePorts_;
    std::vector<AudioPortInfo> audioIns_, audioOuts_;
    std::atomic<uint32_t> latency_{0};
    bool hasGui_ = false;
};

// One plugin instance in this process. The base class owns what every format shares: the
// activation state, the stop/start handshake with the audio thread, the audio buffers, the
// editor and the display name. Formats implement the hooks below.
//
// Crash containment: every call into the plugin goes through callPlugin(), which catches a
// structured exception (an access violation, say) the plugin raises instead of letting it end
// the process. The instance is then "crashed": it is never called again (not even to destroy
// it: its memory may be corrupt), produces silence, and keeps the state it last saved. Its
// module stays loaded for the rest of the process. Exceptions on the plugin's own threads, or
// in its window procedures, which Windows calls directly, are beyond this: a plugin host
// process contains those.
class PluginInstance : public HostedPlugin {
public:
    ~PluginInstance() override;  // host thread; derived destructors call shutdown() first

    // ---- host thread ----
    bool init(std::string& error) override;
    bool activate(double sampleRate, uint32_t maxFrames, std::string& error) override;
    void deactivate() override;
    bool isActive() const override { return active_; }
    void idle() override;

    bool saveState(std::vector<uint8_t>& out) override;
    bool loadState(const std::vector<uint8_t>& in) override;

    bool isGuiOpen() const override { return editor_ != nullptr; }
    bool openGui(std::string& error, void* parent = nullptr) override;
    void closeGui() override;

    void requestStopProcessing() override;
    bool stopProcessingAcknowledged() const override { return stopAck_.load(std::memory_order_acquire); }
    void forceStopProcessing();  // when no audio thread will run any more

    std::string displayName() const;
    void setDisplayName(std::string name) override;
    bool restartPending() const override { return restartRequested_; }
    bool processFailed() const override { return processFailed_; }
    bool crashed() const override { return crashed_.load(std::memory_order_acquire); }
    std::string crashReport() const override;
    bool separateProcess() const override { return false; }

    // ---- audio thread ----
    void appendMidi(InputEventList& list, uint16_t port, const uint8_t* data, uint32_t size,
                    uint32_t time) const override;
    // Processes `frames` frames; returns false when the plugin produced no audio.
    bool process(uint32_t frames, uint64_t steadyTime, const InputEventList& in);
    void beginProcess(uint32_t frames, uint64_t steadyTime, const InputEventList& in) override;
    bool endProcess() override;
    const float* outputChannel(uint32_t port, uint32_t channel) const override;

    HostThread& hostThread() { return host_; }
    // For the formats creating their editor window: where openGui() wants it.
    HostWindow::Placement editorPlacement();
    bool isMainThread() const;
    // The user closed the editor's window: hide it now (unless already gone), tear it down from
    // the host thread later (not from inside the window procedure or the plugin's call stack).
    void editorClosedByUser(bool hideNow = true);

    // Runs f(), which calls into the plugin, catching a crash (see above). False if the plugin
    // crashed now or before (f is then not run). `what` names the call in the crash report and
    // must be a string literal: this may run on the audio thread.
    template <typename F>
    bool callPlugin(const char* what, F&& f) {
        if (crashed()) return false;
        GuardFault fault;
        if (guardedCall(f, fault)) return true;
        recordCrash(what, fault);
        return false;
    }

protected:
    PluginInstance(HostThread& host, PluginHostListener& listener, PluginDescription desc, std::string displayName);
    // Closes the editor and deactivates while the derived object is still whole.
    void shutdown();

    // ---- format hooks (called through callPlugin() by the base class) ----
    virtual bool initPlugin(std::string& error) = 0;
    virtual void idlePlugin() {}
    virtual bool saveStatePlugin(std::vector<uint8_t>& out) = 0;
    virtual bool loadStatePlugin(const std::vector<uint8_t>& in) = 0;
    virtual bool pluginHasGui() const = 0;  // after init
    virtual uint32_t pluginLatency() const = 0;  // after activation
    // Host thread, while inactive: fills notePorts_, audioIns_ and audioOuts_.
    virtual void queryPorts() = 0;
    // Host thread. The buffers for the current ports are allocated when this runs.
    virtual bool activatePlugin(double sampleRate, uint32_t maxFrames, std::string& error) = 0;
    virtual void deactivatePlugin() = 0;
    // Audio thread (or the host thread standing in for it).
    virtual bool startProcessing() = 0;
    virtual void stopProcessing() = 0;
    // Audio thread; frames <= maxFrames_. False: the plugin failed and is not called again.
    virtual bool processPlugin(uint32_t frames, uint64_t steadyTime, const InputEventList& in) = 0;
    virtual std::unique_ptr<PluginEditor> createEditor(std::string& error) = 0;

    void requestRestart() { restartRequested_ = true; }
    // Any thread: tells the listener the plugin's state changed (see pluginStateChanged()).
    void stateChanged() { listener_.pluginStateChanged(*this); }
    // For a derived destructor: keeps something the crashed plugin may still use (its module)
    // alive for the rest of the process.
    static void keepForever(std::shared_ptr<void> p);
    // Host thread: the plugin's ports changed. Re-read now if inactive, else after a restart.
    void portsChanged();
    PluginEditor* editor() const { return editor_.get(); }

    HostThread& host_;
    PluginHostListener& listener_;
    // Per port, per channel; maxFrames_ samples each. Inputs are silent.
    std::vector<std::vector<float*>> inPtrs_, outPtrs_;
    uint32_t maxFrames_ = 0;
    double sampleRate_ = 0;
    std::shared_ptr<int> lifeToken_ = std::make_shared<int>(0);  // guards deferred host-thread work

private:
    void allocateBuffers(uint32_t maxFrames);
    void recordCrash(const char* what, const GuardFault& fault);  // any thread, no allocation

    std::string displayName_;
    mutable std::mutex nameMutex_;  // plugins may log from the audio thread while it is renamed
    std::vector<std::vector<float>> inStorage_, outStorage_;

    std::atomic<bool> active_{false};
    bool processing_ = false;  // audio thread
    // Audio thread: the block beginProcess() was given, for endProcess().
    uint32_t pendingFrames_ = 0;
    uint64_t pendingTime_ = 0;
    const InputEventList* pendingIn_ = nullptr;
    std::atomic<bool> stopRequested_{false};
    std::atomic<bool> stopAck_{true};
    std::atomic<bool> processFailed_{false};
    std::atomic<bool> restartRequested_{false};

    std::unique_ptr<PluginEditor> editor_;
    uint64_t editorGeneration_ = 0;
    void* editorParent_ = nullptr;  // where the open editor is (see openGui())

    std::optional<std::vector<uint8_t>> lastState_;  // last saved or loaded

    // Written once, by the thread that claims the crash first, before crashed_ is set.
    std::atomic<bool> crashClaimed_{false};
    std::atomic<bool> crashed_{false};
    const char* crashWhat_ = "";
    GuardFault crashFault_;
    bool crashHandled_ = false;  // host thread
};

// The architecture (a buildArchitecture() name) the plugin at `path` runs as: Brack's own if its
// binary (one of a Windows or Linux VST3 bundle's binaries, or a universal macOS binary's
// images) is built for it, else another one it has. Empty if there is none, or it cannot be read.
std::string pluginArchitecture(const std::filesystem::path& path);

// Creates (but does not init) an instance of `pluginId` in the file at `path`, in this process;
// the format follows the extension. An empty id selects the first plugin in the file.
std::unique_ptr<PluginInstance> createPluginInstance(HostThread& host, PluginHostListener& listener,
                                                     const std::string& pathUtf8, const std::string& pluginId,
                                                     std::string& error);

// Standard locations of a format on this platform, plus its environment variable
// (CLAP_PATH, VST_PATH, VST3_PATH).
std::vector<std::filesystem::path> defaultPluginSearchPaths(PluginFormat f);
// All formats' standard locations.
std::vector<std::filesystem::path> defaultPluginSearchPaths();
// Plugin files of every format below the given directories (recursively).
std::vector<std::filesystem::path> findPluginFiles(const std::vector<std::filesystem::path>& dirs);
// Lists the plugins a file contains. VST2 plugins are instantiated briefly to ask.
std::vector<PluginDescription> describePluginFile(const std::filesystem::path& file, std::string& error);
// What earlier scans found, by file. A file whose size and modification time are unchanged
// is not loaded again (VST2 plugins are instantiated to be described, which is slow).
// Only successes are kept: a file that failed is tried again on the next scan.
class PluginScanCache {
public:
    bool load(const std::filesystem::path& file);  // false (and empty) if missing or unreadable
    bool save(const std::filesystem::path& file, std::string& error) const;

    struct Entry {
        uint64_t size = 0;
        int64_t time = 0;
        std::vector<PluginDescription> plugins;
    };
    std::map<std::string, Entry> files;  // by UTF-8 path
};

// Describes every file found below dirs. Failures are logged and skipped. With a cache,
// unchanged files are taken from it, and it is updated to what this scan found.
std::vector<PluginDescription> scanPlugins(const std::vector<std::filesystem::path>& dirs,
                                           PluginScanCache* cache = nullptr);

}  // namespace brack
