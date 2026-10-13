// Minimal CLAP effects used by brack's tests: the main input times a gain, on the main output.
//
//   brack.test.gain       stereo in and out
//   brack.test.gain-mono  mono in and out
//   brack.test.gain-slow  stereo, taking 300 ms to init (a plugin slow to load)
//
// State: the gain, a 4-byte float (0.5 until set). A negative gain makes process() crash (an
// access violation), for the host's crash containment, and a state with a gain of -1000 makes its
// load crash; a gain of 1000 makes process() return an error. On
// Windows, an editor: a blank view.

#include <clap/clap.h>
#ifdef _WIN32
#include <windows.h>
#endif

#include <chrono>
#include <cstdio>
#include <cstring>
#include <thread>

namespace {

struct Gain {
    clap_plugin_t plugin{};
    uint32_t channels = 2;
    bool slow = false;
    float gain = 0.5f;
#ifdef _WIN32
    HWND view = nullptr;
#endif
};

Gain* G(const clap_plugin_t* p) { return static_cast<Gain*>(p->plugin_data); }

uint32_t audioPortsCount(const clap_plugin_t*, bool) { return 1; }
bool audioPortsGet(const clap_plugin_t* p, uint32_t index, bool isInput, clap_audio_port_info_t* info) {
    if (index != 0) return false;
    info->id = 0;
    std::snprintf(info->name, sizeof info->name, isInput ? "Main In" : "Main Out");
    info->flags = CLAP_AUDIO_PORT_IS_MAIN;
    info->channel_count = G(p)->channels;
    info->port_type = G(p)->channels == 2 ? CLAP_PORT_STEREO : CLAP_PORT_MONO;
    info->in_place_pair = CLAP_INVALID_ID;
    return true;
}
const clap_plugin_audio_ports_t kAudioPorts{audioPortsCount, audioPortsGet};

bool stateSave(const clap_plugin_t* p, const clap_ostream_t* os) {
    return os->write(os, &G(p)->gain, sizeof(float)) == sizeof(float);
}
bool stateLoad(const clap_plugin_t* p, const clap_istream_t* is) {
    float g;
    if (is->read(is, &g, sizeof g) != sizeof g) return false;
    if (g == -1000.0f) {
        volatile int* volatile nowhere = nullptr;
        *nowhere = 1;
    }
    G(p)->gain = g;
    return true;
}
const clap_plugin_state_t kState{stateSave, stateLoad};

#ifdef _WIN32
constexpr uint32_t kGuiWidth = 200, kGuiHeight = 100;
bool guiSupported(const clap_plugin_t*, const char* api, bool floating) {
    return !floating && !std::strcmp(api, CLAP_WINDOW_API_WIN32);
}
bool guiPreferred(const clap_plugin_t*, const char** api, bool* floating) {
    *api = CLAP_WINDOW_API_WIN32;
    *floating = false;
    return true;
}
bool guiCreate(const clap_plugin_t* p, const char* api, bool floating) { return guiSupported(p, api, floating); }
void guiDestroy(const clap_plugin_t* p) {
    if (G(p)->view) DestroyWindow(G(p)->view);
    G(p)->view = nullptr;
}
bool guiSetScale(const clap_plugin_t*, double) { return false; }
bool guiGetSize(const clap_plugin_t*, uint32_t* w, uint32_t* h) {
    *w = kGuiWidth;
    *h = kGuiHeight;
    return true;
}
bool guiCanResize(const clap_plugin_t*) { return false; }
bool guiGetResizeHints(const clap_plugin_t*, clap_gui_resize_hints_t*) { return false; }
bool guiAdjustSize(const clap_plugin_t* p, uint32_t* w, uint32_t* h) { return guiGetSize(p, w, h); }
bool guiSetSize(const clap_plugin_t*, uint32_t w, uint32_t h) { return w == kGuiWidth && h == kGuiHeight; }
bool guiSetParent(const clap_plugin_t* p, const clap_window_t* window) {
    G(p)->view = CreateWindowExW(0, L"STATIC", L"brack test gain", WS_CHILD | WS_VISIBLE, 0, 0, kGuiWidth, kGuiHeight,
                                 static_cast<HWND>(window->win32), nullptr, nullptr, nullptr);
    return G(p)->view != nullptr;
}
bool guiSetTransient(const clap_plugin_t*, const clap_window_t*) { return false; }
void guiSuggestTitle(const clap_plugin_t*, const char*) {}
bool guiShow(const clap_plugin_t*) { return true; }
bool guiHide(const clap_plugin_t*) { return true; }
const clap_plugin_gui_t kGui{guiSupported, guiPreferred, guiCreate, guiDestroy, guiSetScale,
                             guiGetSize, guiCanResize, guiGetResizeHints, guiAdjustSize, guiSetSize,
                             guiSetParent, guiSetTransient, guiSuggestTitle, guiShow, guiHide};
#endif

bool pInit(const clap_plugin_t* p) {
    if (G(p)->slow) std::this_thread::sleep_for(std::chrono::milliseconds(300));
    return true;
}
void pDestroy(const clap_plugin_t* p) { delete G(p); }
bool pActivate(const clap_plugin_t*, double, uint32_t, uint32_t) { return true; }
void pDeactivate(const clap_plugin_t*) {}
bool pStartProcessing(const clap_plugin_t*) { return true; }
void pStopProcessing(const clap_plugin_t*) {}
void pReset(const clap_plugin_t*) {}

clap_process_status pProcess(const clap_plugin_t* p, const clap_process_t* proc) {
    const Gain* g = G(p);
    if (g->gain < 0) {
        volatile int* volatile nowhere = nullptr;
        *nowhere = 1;
    }
    if (g->gain == 1000.0f) return CLAP_PROCESS_ERROR;
    for (uint32_t c = 0; c < g->channels; ++c) {
        const float* in = proc->audio_inputs[0].data32[c];
        float* out = proc->audio_outputs[0].data32[c];
        for (uint32_t i = 0; i < proc->frames_count; ++i) out[i] = in[i] * g->gain;
    }
    return CLAP_PROCESS_CONTINUE;
}

const void* pGetExtension(const clap_plugin_t*, const char* id) {
    if (!std::strcmp(id, CLAP_EXT_AUDIO_PORTS)) return &kAudioPorts;
    if (!std::strcmp(id, CLAP_EXT_STATE)) return &kState;
#ifdef _WIN32
    if (!std::strcmp(id, CLAP_EXT_GUI)) return &kGui;
#endif
    return nullptr;
}
void pOnMainThread(const clap_plugin_t*) {}

const char* const kFeatures[] = {CLAP_PLUGIN_FEATURE_AUDIO_EFFECT, CLAP_PLUGIN_FEATURE_UTILITY, nullptr};
const clap_plugin_descriptor_t kDescStereo{CLAP_VERSION_INIT, "brack.test.gain", "brack test gain", "brack", "", "", "",
                                           "1.0.0", "gain for tests", kFeatures};
const clap_plugin_descriptor_t kDescMono{CLAP_VERSION_INIT, "brack.test.gain-mono", "brack test gain (mono)", "brack", "",
                                         "", "", "1.0.0", "mono gain for tests", kFeatures};
const clap_plugin_descriptor_t kDescSlow{CLAP_VERSION_INIT, "brack.test.gain-slow", "brack test gain (slow)", "brack", "",
                                         "", "", "1.0.0", "gain slow to load, for tests", kFeatures};

const clap_plugin_t* create(const clap_plugin_descriptor_t* desc, uint32_t channels, bool slow = false) {
    auto* g = new Gain();
    g->channels = channels;
    g->slow = slow;
    clap_plugin_t& p = g->plugin;
    p.desc = desc;
    p.plugin_data = g;
    p.init = pInit;
    p.destroy = pDestroy;
    p.activate = pActivate;
    p.deactivate = pDeactivate;
    p.start_processing = pStartProcessing;
    p.stop_processing = pStopProcessing;
    p.reset = pReset;
    p.process = pProcess;
    p.get_extension = pGetExtension;
    p.on_main_thread = pOnMainThread;
    return &p;
}

uint32_t fCount(const clap_plugin_factory_t*) { return 3; }
const clap_plugin_descriptor_t* fDesc(const clap_plugin_factory_t*, uint32_t i) {
    return i == 0 ? &kDescStereo : i == 1 ? &kDescMono : i == 2 ? &kDescSlow : nullptr;
}
const clap_plugin_t* fCreate(const clap_plugin_factory_t*, const clap_host_t*, const char* id) {
    if (!std::strcmp(id, kDescStereo.id)) return create(&kDescStereo, 2);
    if (!std::strcmp(id, kDescMono.id)) return create(&kDescMono, 1);
    if (!std::strcmp(id, kDescSlow.id)) return create(&kDescSlow, 2, true);
    return nullptr;
}
const clap_plugin_factory_t kFactory{fCount, fDesc, fCreate};

bool eInit(const char*) { return true; }
void eDeinit() {}
const void* eGetFactory(const char* id) { return !std::strcmp(id, CLAP_PLUGIN_FACTORY_ID) ? &kFactory : nullptr; }

}  // namespace

extern "C" CLAP_EXPORT const clap_plugin_entry_t clap_entry{CLAP_VERSION_INIT, eInit, eDeinit, eGetFactory};
