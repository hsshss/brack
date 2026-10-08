#pragma once
#include <clap/clap.h>

#include <map>
#include <memory>
#include <string>
#include <vector>

#include "clap/clap_module.h"
#include "plugin/plugin.h"

namespace brack {

class ClapInstance final : public PluginInstance {
public:
    ClapInstance(HostThread& host, PluginHostListener& listener, std::shared_ptr<ClapModule> module,
                 PluginDescription desc, std::string displayName);
    ~ClapInstance() override;

    // Encodes MIDI in a dialect the port accepts: raw MIDI whenever it can.
    void appendMidi(InputEventList& list, uint16_t port, const uint8_t* data, uint32_t size,
                    uint32_t time) const override;

    // ---- clap_host callbacks ----
    void onLog(clap_log_severity severity, const char* msg);
    void onRequestRestart() { requestRestart(); }
    void onRequestCallback() { callbackRequested_ = true; }
    void onParamsRequestFlush() { flushRequested_ = true; }
    void onStateDirty() { stateChanged(); }
    void onRescanPorts() { portsChanged(); }
    bool onGuiRequestResize(uint32_t w, uint32_t h);
    bool onGuiRequestShow();
    bool onGuiRequestHide();
    void onGuiClosed(bool wasDestroyed);
    bool onRegisterTimer(uint32_t periodMs, clap_id* id);
    bool onUnregisterTimer(clap_id id);
#ifndef _WIN32
    bool onRegisterFd(int fd, clap_posix_fd_flags_t flags);
    bool onModifyFd(int fd, clap_posix_fd_flags_t flags);
    bool onUnregisterFd(int fd);
#endif

private:
    bool initPlugin(std::string& error) override;
    void idlePlugin() override;
    bool saveStatePlugin(std::vector<uint8_t>& out) override;
    bool loadStatePlugin(const std::vector<uint8_t>& in) override;
    bool pluginHasGui() const override;
    uint32_t pluginLatency() const override;
    void queryPorts() override;
    bool activatePlugin(double sampleRate, uint32_t maxFrames, std::string& error) override;
    void deactivatePlugin() override;
    bool startProcessing() override;
    void stopProcessing() override;
    bool processPlugin(uint32_t frames, uint64_t steadyTime, const InputEventList& in) override;
    std::unique_ptr<PluginEditor> createEditor(std::string& error) override;

    std::shared_ptr<ClapModule> module_;
    clap_host_t clapHost_{};
    const clap_plugin_t* plugin_ = nullptr;

    // Plugin extensions (queried once after init).
    const clap_plugin_audio_ports_t* extAudioPorts_ = nullptr;
    const clap_plugin_note_ports_t* extNotePorts_ = nullptr;
    const clap_plugin_gui_t* extGui_ = nullptr;
    const clap_plugin_state_t* extState_ = nullptr;
    const clap_plugin_params_t* extParams_ = nullptr;
    const clap_plugin_timer_support_t* extTimer_ = nullptr;
    const clap_plugin_latency_t* extLatency_ = nullptr;
    const clap_plugin_posix_fd_support_t* extFd_ = nullptr;

    std::vector<uint8_t> portAcceptsMidi_;  // per note port: dialect includes CLAP_NOTE_DIALECT_MIDI
    std::vector<clap_audio_buffer_t> inBufs_, outBufs_;

    clap_output_events_t outEvents_{};  // ctx = this; see outTryPush() in the .cpp

    std::atomic<bool> callbackRequested_{false};
    std::atomic<bool> flushRequested_{false};

    std::map<clap_id, uint32_t> timers_;  // clap timer id -> host timer id
    clap_id nextTimerId_ = 1;
    std::map<int, uint32_t> fds_;  // file descriptor -> HostThread::watchFd() id
};

// The CLAP editor: embedded in a HostWindow, or the plugin's own floating window.
class ClapEditor : public PluginEditor {
public:
    virtual bool requestResize(uint32_t width, uint32_t height) = 0;
};
std::unique_ptr<ClapEditor> createClapEditor(ClapInstance& owner, const clap_plugin_t* plugin,
                                               const clap_plugin_gui_t* gui, const std::string& title,
                                               std::string& error);
bool clapEditorSupported(const clap_plugin_t* plugin, const clap_plugin_gui_t* gui);

}  // namespace brack
