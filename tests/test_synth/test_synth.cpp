// Minimal CLAP instrument used by brack's tests.
//
// Three plugins in one file:
//   brack.test.synth             note port with MIDI + CLAP dialects (prefers MIDI)
//   brack.test.synth-clap        note port with the CLAP dialect only
//   brack.test.synth-crash-init  crashes in init
// Both are polyphonic sine synths with a stereo output. Every MIDI event they receive is
// recorded and, when the plugin is destroyed, written as hex lines to the file named by
// BRACK_TESTSYNTH_LOG (if set); an event not at the start of its block shows its frame
// ("midi port=0 @100: ..."), or with BRACK_TESTSYNTH_FRAMES set, every event its steady time
// ("midi port=0 frame=4196: ..."). State: a 4-byte gain, then optionally a uint32 of ms to take
// loading it (as a plugin loading its samples would) and another of ms to take activating.
//
// The editor is a plain child window of 320 x 200 (embedded only, not resizable). Like editors
// that know their size only once they have a parent window (JUCE's), it says it is half that until
// set_parent, which asks the host for the real size. On X11 (built where the X11 development files
// are) it is a window named "brack test synth" on a connection of its own, which the host watches
// for it (posix-fd-support). libX11 is loaded when the editor is made (dlopen), so that the synth
// loads where libX11 is missing. It is an XEmbed client that leaves it to the host to map it
// (_XEMBED_INFO with XEMBED_MAPPED), and its name tells what it has heard on that connection:
// "brack test synth: embedded, exposed" once the host has told it it is embedded and it has
// handled an Expose, then ", active" and ", focused" from the host's XEmbed messages and
// ", scale 1.5" (say) after a set_scale other than 1.
//
// For the host's crash containment, SysEx F0 7D 63 01 F7 makes the plugin crash (an access
// violation) in process(), F0 7D 63 08 F7 run out of stack there, F0 7D 63 09 F7 throw a C++
// exception out of it (which only Windows can catch: elsewhere it unwinds into the host's own
// unwinder and aborts), and F0 7D 63 02 F7 makes its next state save crash. What only a
// plugin host process contains, since Brack never sees it coming (each takes a process down):
//   F0 7D 63 03 F7  an access violation on a thread of the plugin's own
//   F0 7D 63 04 F7  abort()
//   F0 7D 63 05 F7  __fastfail(), as a detected heap corruption ends a process (elsewhere a trap)
//   F0 7D 63 06 F7  process() never returns
//   F0 7D 63 07 F7  an access violation in the editor's window procedure (the editor is open;
//                   Windows only)
//   F0 7D 63 0A F7  a stack overflow on a thread of the plugin's own
// A copy named crash-at-<stage> crashes in clap_entry.init ("init"), get_factory ("factory"),
// the factory's plugin count ("count") or clap_entry.deinit ("deinit"); see crash_stage.h.
//
// Like a parameter turned in an editor, CC 7 makes process() report a CLAP_EVENT_PARAM_VALUE
// and CC 8 a CLAP_EVENT_PARAM_GESTURE_END through its output events.

#include <clap/clap.h>
#ifdef _WIN32
#include <intrin.h>
#elif defined(__APPLE__)
#import <AppKit/AppKit.h>
#elif defined(BRACK_TEST_SYNTH_X11)
#include <X11/Xlib.h>
#include <dlfcn.h>
#endif

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "crash_stage.h"

namespace {

constexpr int kVoices = 16;

// An access violation, as a plugin bug would raise one.
void crash() {
    volatile int* volatile nowhere = nullptr;
    *nowhere = 1;
}

// Endless recursion, which no compiler can see ends never: a stack overflow.
volatile bool g_bottom = false;
int overflow(int depth) {
    volatile char frame[4096];
    frame[0] = (char)depth;
    if (g_bottom) return 0;
    return overflow(depth + 1) + frame[0];
}

#ifdef _WIN32
constexpr UINT kCrashMessage = WM_APP + 0x63;
WNDPROC g_staticProc = nullptr;

// The editor's window procedure: the STATIC control's, but for kCrashMessage.
LRESULT CALLBACK viewProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == kCrashMessage) crash();
    return CallWindowProcW(g_staticProc, hwnd, msg, wp, lp);
}
#endif

struct Voice {
    bool on = false;
    int channel = 0, key = 0;
    double phase = 0, freq = 0, amp = 0;
};

struct Synth {
    clap_plugin_t plugin{};
    const clap_host_t* host = nullptr;
    bool midiDialect = true;
    double sampleRate = 48000;
    float gain = 0.25f;
    uint32_t activateMs = 0;  // from the state
    bool crashOnSave = false;
    Voice voices[kVoices];
    std::vector<std::string> received;  // reserved up front; tests only send a few messages
    bool logFrames = false;             // BRACK_TESTSYNTH_FRAMES: log each event's steady time
    int64_t steadyTime = 0;             // of the block being processed
    const clap_output_events_t* out = nullptr;  // during process()
#ifdef _WIN32
    HWND view = nullptr;  // the editor
#elif defined(__APPLE__)
    NSView* view = nil;
#elif defined(BRACK_TEST_SYNTH_X11)
    Display* display = nullptr;  // the editor's own connection
    Window view = 0;
    bool embedded = false, exposed = false, active = false, focused = false;  // from that connection
    double scale = 1.0;
#endif

    void noteOn(int ch, int key, double vel) {
        for (auto& v : voices)
            if (!v.on) {
                v = {true, ch, key, 0.0, 440.0 * std::pow(2.0, (key - 69) / 12.0), vel};
                return;
            }
    }
    void noteOff(int ch, int key) {
        for (auto& v : voices)
            if (v.on && v.channel == ch && v.key == key) v.on = false;
    }
    void record(const uint8_t* d, uint32_t n, uint16_t port, const char* kind, uint32_t time) {
        char buf[64];
        std::string line = kind;
        if (logFrames) std::snprintf(buf, sizeof buf, " port=%u frame=%lld:", port, (long long)(steadyTime + time));
        else if (time) std::snprintf(buf, sizeof buf, " port=%u @%u:", port, time);
        else std::snprintf(buf, sizeof buf, " port=%u:", port);
        line += buf;
        for (uint32_t i = 0; i < n; ++i) {
            std::snprintf(buf, sizeof buf, " %02X", d[i]);
            line += buf;
        }
        received.push_back(std::move(line));
    }
    void paramValue(double value) {
        clap_event_param_value_t e{};
        e.header = {sizeof e, 0, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_PARAM_VALUE, 0};
        e.param_id = 0;
        e.note_id = -1;
        e.port_index = e.channel = e.key = -1;
        e.value = value;
        out->try_push(out, &e.header);
    }
    void gestureEnd() {
        clap_event_param_gesture_t e{};
        e.header = {sizeof e, 0, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_PARAM_GESTURE_END, 0};
        e.param_id = 0;
        out->try_push(out, &e.header);
    }
    void handle(const clap_event_header_t* h) {
        if (h->space_id != CLAP_CORE_EVENT_SPACE_ID) return;
        switch (h->type) {
            case CLAP_EVENT_MIDI: {
                auto* e = reinterpret_cast<const clap_event_midi_t*>(h);
                record(e->data, 3, e->port_index, "midi", h->time);
                uint8_t st = e->data[0] & 0xF0, ch = e->data[0] & 0x0F;
                if (st == 0x90 && e->data[2] > 0) noteOn(ch, e->data[1], e->data[2] / 127.0);
                else if (st == 0x80 || st == 0x90) noteOff(ch, e->data[1]);
                else if (st == 0xB0 && e->data[1] == 7) paramValue(e->data[2] / 127.0);
                else if (st == 0xB0 && e->data[1] == 8) gestureEnd();
                break;
            }
            case CLAP_EVENT_MIDI_SYSEX: {
                auto* e = reinterpret_cast<const clap_event_midi_sysex_t*>(h);
                record(e->buffer, e->size, e->port_index, "sysex", h->time);
                if (e->size == 5 && e->buffer[1] == 0x7D && e->buffer[2] == 0x63) {
                    switch (e->buffer[3]) {
                        case 0x01: crash(); break;
                        case 0x02: crashOnSave = true; break;
                        case 0x03: std::thread(crash).detach(); break;
                        case 0x04: std::abort();
#ifdef _WIN32
                        case 0x05: __fastfail(FAST_FAIL_FATAL_APP_EXIT);
                        case 0x07: if (view) PostMessageW(view, kCrashMessage, 0, 0); break;
#else
                        case 0x05: __builtin_trap();
#endif
                        case 0x06: for (;;) std::this_thread::sleep_for(std::chrono::seconds(1));
                        case 0x08: overflow(0); break;
                        case 0x09: throw std::runtime_error("brack test synth: thrown out of process()");
                        case 0x0A: std::thread([] { overflow(0); }).detach(); break;
                    }
                }
                break;
            }
            case CLAP_EVENT_NOTE_ON: {
                auto* e = reinterpret_cast<const clap_event_note_t*>(h);
                char buf[96];
                std::snprintf(buf, sizeof buf, "note-on port=%d: ch=%d key=%d vel=%.4f", e->port_index, e->channel, e->key, e->velocity);
                received.emplace_back(buf);
                noteOn(e->channel, e->key, e->velocity);
                break;
            }
            case CLAP_EVENT_NOTE_OFF: {
                auto* e = reinterpret_cast<const clap_event_note_t*>(h);
                char buf[96];
                std::snprintf(buf, sizeof buf, "note-off port=%d: ch=%d key=%d", e->port_index, e->channel, e->key);
                received.emplace_back(buf);
                noteOff(e->channel, e->key);
                break;
            }
            default: break;
        }
    }
};

Synth* S(const clap_plugin_t* p) { return static_cast<Synth*>(p->plugin_data); }

// ---- extensions ----
uint32_t audioPortsCount(const clap_plugin_t*, bool isInput) { return isInput ? 0 : 1; }
bool audioPortsGet(const clap_plugin_t*, uint32_t index, bool isInput, clap_audio_port_info_t* info) {
    if (isInput || index != 0) return false;
    info->id = 0;
    std::snprintf(info->name, sizeof info->name, "Main Out");
    info->flags = CLAP_AUDIO_PORT_IS_MAIN;
    info->channel_count = 2;
    info->port_type = CLAP_PORT_STEREO;
    info->in_place_pair = CLAP_INVALID_ID;
    return true;
}
const clap_plugin_audio_ports_t kAudioPorts{audioPortsCount, audioPortsGet};

uint32_t notePortsCount(const clap_plugin_t*, bool isInput) { return isInput ? 1 : 0; }
bool notePortsGet(const clap_plugin_t* p, uint32_t index, bool isInput, clap_note_port_info_t* info) {
    if (!isInput || index != 0) return false;
    info->id = 0;
    info->supported_dialects = S(p)->midiDialect ? (CLAP_NOTE_DIALECT_MIDI | CLAP_NOTE_DIALECT_CLAP) : CLAP_NOTE_DIALECT_CLAP;
    info->preferred_dialect = S(p)->midiDialect ? CLAP_NOTE_DIALECT_MIDI : CLAP_NOTE_DIALECT_CLAP;
    std::snprintf(info->name, sizeof info->name, "Notes");
    return true;
}
const clap_plugin_note_ports_t kNotePorts{notePortsCount, notePortsGet};

bool stateSave(const clap_plugin_t* p, const clap_ostream_t* os) {
    if (S(p)->crashOnSave) crash();
    float g = S(p)->gain;
    return os->write(os, &g, sizeof g) == sizeof g;
}
bool stateLoad(const clap_plugin_t* p, const clap_istream_t* is) {
    float g;
    if (is->read(is, &g, sizeof g) != sizeof g) return false;
    S(p)->gain = g;
    if (uint32_t ms = 0; is->read(is, &ms, sizeof ms) == sizeof ms)
        std::this_thread::sleep_for(std::chrono::milliseconds(ms));
    if (uint32_t ms = 0; is->read(is, &ms, sizeof ms) == sizeof ms) S(p)->activateMs = ms;
    return true;
}
const clap_plugin_state_t kState{stateSave, stateLoad};

#ifdef _WIN32
constexpr uint32_t kGuiWidth = 320, kGuiHeight = 200;
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
    if (S(p)->view) DestroyWindow(S(p)->view);
    S(p)->view = nullptr;
}
bool guiSetScale(const clap_plugin_t*, double) { return false; }
bool guiGetSize(const clap_plugin_t* p, uint32_t* w, uint32_t* h) {
    const uint32_t div = S(p)->view ? 1 : 2;
    *w = kGuiWidth / div;
    *h = kGuiHeight / div;
    return true;
}
bool guiCanResize(const clap_plugin_t*) { return false; }
bool guiGetResizeHints(const clap_plugin_t*, clap_gui_resize_hints_t*) { return false; }
bool guiAdjustSize(const clap_plugin_t* p, uint32_t* w, uint32_t* h) { return guiGetSize(p, w, h); }
bool guiSetSize(const clap_plugin_t*, uint32_t w, uint32_t h) { return w == kGuiWidth && h == kGuiHeight; }
bool guiSetParent(const clap_plugin_t* p, const clap_window_t* window) {
    S(p)->view = CreateWindowExW(0, L"STATIC", L"brack test synth", WS_CHILD | WS_VISIBLE, 0, 0, kGuiWidth, kGuiHeight,
                                 static_cast<HWND>(window->win32), nullptr, nullptr, nullptr);
    if (!S(p)->view) return false;
    g_staticProc = reinterpret_cast<WNDPROC>(
        SetWindowLongPtrW(S(p)->view, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&viewProc)));
    const clap_host_t* host = S(p)->host;
    if (auto* gui = static_cast<const clap_host_gui_t*>(host->get_extension(host, CLAP_EXT_GUI)))
        gui->request_resize(host, kGuiWidth, kGuiHeight);
    return true;
}
bool guiSetTransient(const clap_plugin_t*, const clap_window_t*) { return false; }
void guiSuggestTitle(const clap_plugin_t*, const char*) {}
bool guiShow(const clap_plugin_t*) { return true; }
bool guiHide(const clap_plugin_t*) { return true; }
const clap_plugin_gui_t kGui{guiSupported, guiPreferred, guiCreate, guiDestroy, guiSetScale,
                             guiGetSize, guiCanResize, guiGetResizeHints, guiAdjustSize, guiSetSize,
                             guiSetParent, guiSetTransient, guiSuggestTitle, guiShow, guiHide};
#elif defined(__APPLE__)
constexpr uint32_t kGuiWidth = 320, kGuiHeight = 200;
bool guiSupported(const clap_plugin_t*, const char* api, bool floating) {
    return !floating && !std::strcmp(api, CLAP_WINDOW_API_COCOA);
}
bool guiPreferred(const clap_plugin_t*, const char** api, bool* floating) {
    *api = CLAP_WINDOW_API_COCOA;
    *floating = false;
    return true;
}
bool guiCreate(const clap_plugin_t* p, const char* api, bool floating) { return guiSupported(p, api, floating); }
void guiDestroy(const clap_plugin_t* p) {
    Synth* s = S(p);
    if (!s->view) return;
    const NSSize view = s->view.frame.size, host = s->view.superview.frame.size;
    char buf[96];
    std::snprintf(buf, sizeof buf, "editor: view %.0fx%.0f in %.0fx%.0f", view.width, view.height, host.width, host.height);
    s->received.emplace_back(buf);
    [s->view removeFromSuperview];
    s->view = nil;
}
bool guiSetScale(const clap_plugin_t*, double) { return false; }
bool guiGetSize(const clap_plugin_t* p, uint32_t* w, uint32_t* h) {
    const uint32_t div = S(p)->view ? 1 : 2;
    *w = kGuiWidth / div;
    *h = kGuiHeight / div;
    return true;
}
bool guiCanResize(const clap_plugin_t*) { return false; }
bool guiGetResizeHints(const clap_plugin_t*, clap_gui_resize_hints_t*) { return false; }
bool guiAdjustSize(const clap_plugin_t* p, uint32_t* w, uint32_t* h) { return guiGetSize(p, w, h); }
bool guiSetSize(const clap_plugin_t*, uint32_t w, uint32_t h) { return w == kGuiWidth && h == kGuiHeight; }
bool guiSetParent(const clap_plugin_t* p, const clap_window_t* window) {
    NSView* parent = (__bridge NSView*)window->cocoa;
    NSTextField* label = [NSTextField labelWithString:@"brack test synth"];
    label.frame = NSMakeRect(0, 0, kGuiWidth, kGuiHeight);
    [parent addSubview:label];
    S(p)->view = label;
    const clap_host_t* host = S(p)->host;
    if (auto* gui = static_cast<const clap_host_gui_t*>(host->get_extension(host, CLAP_EXT_GUI)))
        gui->request_resize(host, kGuiWidth, kGuiHeight);
    return true;
}
bool guiSetTransient(const clap_plugin_t*, const clap_window_t*) { return false; }
void guiSuggestTitle(const clap_plugin_t*, const char*) {}
bool guiShow(const clap_plugin_t*) { return true; }
bool guiHide(const clap_plugin_t*) { return true; }
const clap_plugin_gui_t kGui{guiSupported, guiPreferred, guiCreate, guiDestroy, guiSetScale,
                             guiGetSize, guiCanResize, guiGetResizeHints, guiAdjustSize, guiSetSize,
                             guiSetParent, guiSetTransient, guiSuggestTitle, guiShow, guiHide};
#elif defined(BRACK_TEST_SYNTH_X11)
constexpr uint32_t kGuiWidth = 320, kGuiHeight = 200;
#define TEST_SYNTH_XLIB_FUNCTIONS(X)                                                                  \
    X(XOpenDisplay) X(XCloseDisplay) X(XFlush) X(XPending) X(XNextEvent) X(XInternAtom) X(XSelectInput) \
    X(XCreateSimpleWindow) X(XDestroyWindow) X(XStoreName) X(XChangeProperty)
struct Xlib {
#define TEST_SYNTH_XLIB_MEMBER(name) decltype(&::name) name;
    TEST_SYNTH_XLIB_FUNCTIONS(TEST_SYNTH_XLIB_MEMBER)
};
// Null without libX11.so.6.
const Xlib* xlib() {
    static const Xlib* loaded = []() -> const Xlib* {
        void* lib = dlopen("libX11.so.6", RTLD_NOW | RTLD_LOCAL);
        if (!lib) return nullptr;
        static Xlib x;
        bool ok = true;
#define TEST_SYNTH_XLIB_LOAD(name)                                     \
    x.name = reinterpret_cast<decltype(x.name)>(dlsym(lib, #name)); \
    ok = ok && x.name;
        TEST_SYNTH_XLIB_FUNCTIONS(TEST_SYNTH_XLIB_LOAD)
        return ok ? &x : nullptr;
    }();
    return loaded;
}
const clap_host_posix_fd_support_t* hostFd(const clap_plugin_t* p) {
    return static_cast<const clap_host_posix_fd_support_t*>(S(p)->host->get_extension(S(p)->host, CLAP_EXT_POSIX_FD_SUPPORT));
}
bool guiSupported(const clap_plugin_t*, const char* api, bool floating) {
    return !floating && !std::strcmp(api, CLAP_WINDOW_API_X11);
}
bool guiPreferred(const clap_plugin_t*, const char** api, bool* floating) {
    *api = CLAP_WINDOW_API_X11;
    *floating = false;
    return true;
}
bool guiCreate(const clap_plugin_t* p, const char* api, bool floating) {
    const clap_host_posix_fd_support_t* fd = hostFd(p);
    if (!guiSupported(p, api, floating) || !fd || !xlib() || !(S(p)->display = xlib()->XOpenDisplay(nullptr)))
        return false;
    return fd->register_fd(S(p)->host, ConnectionNumber(S(p)->display), CLAP_POSIX_FD_READ);
}
void guiDestroy(const clap_plugin_t* p) {
    Synth* s = S(p);
    if (!s->display) return;
    if (const clap_host_posix_fd_support_t* fd = hostFd(p)) fd->unregister_fd(s->host, ConnectionNumber(s->display));
    if (s->view) xlib()->XDestroyWindow(s->display, s->view);
    xlib()->XCloseDisplay(s->display);
    s->display = nullptr;
    s->view = 0;
}
// The view's name: "brack test synth" and what it has heard (see the top of this file).
void nameView(Synth* s) {
    if (!s->view) return;
    std::string name = "brack test synth";
    const char* sep = ": ";
    for (auto [heard, what] : {std::pair{s->embedded, "embedded"}, {s->exposed, "exposed"}, {s->active, "active"},
                               {s->focused, "focused"}})
        if (heard) {
            name += sep;
            name += what;
            sep = ", ";
        }
    if (s->scale != 1.0) {
        char buf[32];
        std::snprintf(buf, sizeof buf, "%sscale %g", sep, s->scale);
        name += buf;
    }
    xlib()->XStoreName(s->display, s->view, name.c_str());
    xlib()->XFlush(s->display);
}
bool guiSetScale(const clap_plugin_t* p, double scale) {
    S(p)->scale = scale;
    nameView(S(p));
    return true;
}
bool guiGetSize(const clap_plugin_t* p, uint32_t* w, uint32_t* h) {
    const uint32_t div = S(p)->view ? 1 : 2;
    *w = kGuiWidth / div;
    *h = kGuiHeight / div;
    return true;
}
bool guiCanResize(const clap_plugin_t*) { return false; }
bool guiGetResizeHints(const clap_plugin_t*, clap_gui_resize_hints_t*) { return false; }
bool guiAdjustSize(const clap_plugin_t* p, uint32_t* w, uint32_t* h) { return guiGetSize(p, w, h); }
bool guiSetSize(const clap_plugin_t*, uint32_t w, uint32_t h) { return w == kGuiWidth && h == kGuiHeight; }
bool guiSetParent(const clap_plugin_t* p, const clap_window_t* window) {
    Synth* s = S(p);
    const Xlib& x = *xlib();
    Display* d = s->display;
    s->view = x.XCreateSimpleWindow(d, (Window)window->x11, 0, 0, kGuiWidth, kGuiHeight, 0, BlackPixel(d, DefaultScreen(d)),
                                    WhitePixel(d, DefaultScreen(d)));
    if (!s->view) return false;
    nameView(s);
    x.XSelectInput(d, s->view, ExposureMask);
    const Atom info = x.XInternAtom(d, "_XEMBED_INFO", False);
    const long versionAndFlags[2] = {0, 1};  // XEMBED_MAPPED: the embedder maps it
    x.XChangeProperty(d, s->view, info, info, 32, PropModeReplace, reinterpret_cast<const unsigned char*>(versionAndFlags), 2);
    x.XFlush(d);
    const clap_host_t* host = s->host;
    if (auto* gui = static_cast<const clap_host_gui_t*>(host->get_extension(host, CLAP_EXT_GUI)))
        gui->request_resize(host, kGuiWidth, kGuiHeight);
    return true;
}
bool guiSetTransient(const clap_plugin_t*, const clap_window_t*) { return false; }
void guiSuggestTitle(const clap_plugin_t*, const char*) {}
bool guiShow(const clap_plugin_t*) { return true; }
bool guiHide(const clap_plugin_t*) { return true; }
const clap_plugin_gui_t kGui{guiSupported, guiPreferred, guiCreate, guiDestroy, guiSetScale,
                             guiGetSize, guiCanResize, guiGetResizeHints, guiAdjustSize, guiSetSize,
                             guiSetParent, guiSetTransient, guiSuggestTitle, guiShow, guiHide};

// The host saw the connection readable: its events, on the host's main thread.
void onFd(const clap_plugin_t* p, int, clap_posix_fd_flags_t) {
    Synth* s = S(p);
    if (!s->display) return;
    const Atom xembed = xlib()->XInternAtom(s->display, "_XEMBED", False);
    while (xlib()->XPending(s->display)) {
        XEvent e;
        xlib()->XNextEvent(s->display, &e);
        if (e.xany.window != s->view) continue;
        if (e.type == Expose) s->exposed = true;
        if (e.type == ClientMessage && e.xclient.message_type == xembed) {
            switch (e.xclient.data.l[1]) {  // XEMBED_*
                case 0: s->embedded = true; break;   // EMBEDDED_NOTIFY
                case 1: s->active = true; break;     // WINDOW_ACTIVATE
                case 2: s->active = false; break;    // WINDOW_DEACTIVATE
                case 4: s->focused = true; break;    // FOCUS_IN
                case 5: s->focused = false; break;   // FOCUS_OUT
            }
        }
        nameView(s);
    }
    xlib()->XFlush(s->display);
}
const clap_plugin_posix_fd_support_t kFd{onFd};
#endif

// ---- plugin ----
bool pInit(const clap_plugin_t* p) {
    if (!std::strcmp(p->desc->id, "brack.test.synth-crash-init")) crash();
    S(p)->received.reserve(4096);
    S(p)->logFrames = std::getenv("BRACK_TESTSYNTH_FRAMES") != nullptr;
    return true;
}
void pDestroy(const clap_plugin_t* p) {
    Synth* s = S(p);
    if (const char* path = std::getenv("BRACK_TESTSYNTH_LOG")) {
        if (FILE* f = std::fopen(path, "ab")) {
            for (auto& l : s->received) std::fprintf(f, "%s\n", l.c_str());
            std::fclose(f);
        }
    }
    delete s;
}
bool pActivate(const clap_plugin_t* p, double sr, uint32_t, uint32_t) {
    S(p)->sampleRate = sr;
    std::this_thread::sleep_for(std::chrono::milliseconds(S(p)->activateMs));
    return true;
}
void pDeactivate(const clap_plugin_t*) {}
bool pStartProcessing(const clap_plugin_t*) { return true; }
void pStopProcessing(const clap_plugin_t*) {}
void pReset(const clap_plugin_t* p) {
    for (auto& v : S(p)->voices) v.on = false;
}

clap_process_status pProcess(const clap_plugin_t* p, const clap_process_t* proc) {
    Synth* s = S(p);
    const uint32_t n = proc->frames_count;
    s->out = proc->out_events;
    s->steadyTime = proc->steady_time;
    const uint32_t ne = proc->in_events->size(proc->in_events);
    for (uint32_t i = 0; i < ne; ++i) s->handle(proc->in_events->get(proc->in_events, i));
    float* l = proc->audio_outputs[0].data32[0];
    float* r = proc->audio_outputs[0].data32[1];
    for (uint32_t i = 0; i < n; ++i) {
        double acc = 0;
        for (auto& v : s->voices) {
            if (!v.on) continue;
            acc += std::sin(v.phase) * v.amp;
            v.phase += 2.0 * 3.14159265358979323846 * v.freq / s->sampleRate;
            if (v.phase > 6.283185307179586) v.phase -= 6.283185307179586;
        }
        l[i] = r[i] = (float)(acc * s->gain);
    }
    return CLAP_PROCESS_CONTINUE;
}

const void* pGetExtension(const clap_plugin_t*, const char* id) {
    if (!std::strcmp(id, CLAP_EXT_AUDIO_PORTS)) return &kAudioPorts;
    if (!std::strcmp(id, CLAP_EXT_NOTE_PORTS)) return &kNotePorts;
    if (!std::strcmp(id, CLAP_EXT_STATE)) return &kState;
#if defined(_WIN32) || defined(__APPLE__) || defined(BRACK_TEST_SYNTH_X11)
    if (!std::strcmp(id, CLAP_EXT_GUI)) return &kGui;
#endif
#ifdef BRACK_TEST_SYNTH_X11
    if (!std::strcmp(id, CLAP_EXT_POSIX_FD_SUPPORT)) return &kFd;
#endif
    return nullptr;
}
void pOnMainThread(const clap_plugin_t*) {}

const char* const kFeatures[] = {CLAP_PLUGIN_FEATURE_INSTRUMENT, CLAP_PLUGIN_FEATURE_SYNTHESIZER, nullptr};
const clap_plugin_descriptor_t kDescMidi{CLAP_VERSION_INIT, "brack.test.synth", "brack test synth", "brack", "", "", "",
                                          "1.0.0", "sine synth for tests", kFeatures};
const clap_plugin_descriptor_t kDescClap{CLAP_VERSION_INIT, "brack.test.synth-clap", "brack test synth (CLAP notes)",
                                          "brack", "", "", "", "1.0.0", "sine synth, CLAP note dialect only", kFeatures};

const clap_plugin_t* create(const clap_host_t* host, bool midi, const clap_plugin_descriptor_t* desc) {
    auto* s = new Synth();
    s->host = host;
    s->midiDialect = midi;
    s->plugin.desc = desc;
    s->plugin.plugin_data = s;
    s->plugin.init = pInit;
    s->plugin.destroy = pDestroy;
    s->plugin.activate = pActivate;
    s->plugin.deactivate = pDeactivate;
    s->plugin.start_processing = pStartProcessing;
    s->plugin.stop_processing = pStopProcessing;
    s->plugin.reset = pReset;
    s->plugin.process = pProcess;
    s->plugin.get_extension = pGetExtension;
    s->plugin.on_main_thread = pOnMainThread;
    return &s->plugin;
}

const clap_plugin_descriptor_t kDescCrashInit{CLAP_VERSION_INIT, "brack.test.synth-crash-init", "crashes in init", "brack",
                                               "", "", "", "1.0.0", "crashes in init", kFeatures};

uint32_t fCount(const clap_plugin_factory_t*) {
    crashAt("count");
    return 3;
}
const clap_plugin_descriptor_t* fDesc(const clap_plugin_factory_t*, uint32_t i) {
    return i == 0 ? &kDescMidi : i == 1 ? &kDescClap : i == 2 ? &kDescCrashInit : nullptr;
}

const clap_plugin_t* fCreate(const clap_plugin_factory_t*, const clap_host_t* host, const char* id) {
    if (!std::strcmp(id, kDescMidi.id)) return create(host, true, &kDescMidi);
    if (!std::strcmp(id, kDescCrashInit.id)) return create(host, true, &kDescCrashInit);
    if (!std::strcmp(id, kDescClap.id)) return create(host, false, &kDescClap);
    return nullptr;
}
const clap_plugin_factory_t kFactory{fCount, fDesc, fCreate};

bool eInit(const char*) {
    crashAt("init");
    return true;
}
void eDeinit() { crashAt("deinit"); }
const void* eGetFactory(const char* id) {
    crashAt("factory");
    return !std::strcmp(id, CLAP_PLUGIN_FACTORY_ID) ? &kFactory : nullptr;
}

}  // namespace

extern "C" CLAP_EXPORT const clap_plugin_entry_t clap_entry{CLAP_VERSION_INIT, eInit, eDeinit, eGetFactory};
