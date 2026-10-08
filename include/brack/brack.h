/*
 * Brack - CLAP / VST3 / VST2 instrument host: public C API (brack.dll / libbrack).
 *
 * Conventions
 *  - All strings are UTF-8, NUL terminated.
 *  - Functions returning int return BRACK_OK (0) on success or a negative
 *    BRACK_ERR_* code; brack_last_error() then describes the failure for the
 *    calling thread.
 *  - Every function is thread safe, except that brack_engine_destroy must not overlap other
 *    calls on the same engine. Engine mutations are serialised on the
 *    engine's own host thread; plugin editor windows live there too, so the
 *    caller does not need to pump any message loop. Except on macOS, where
 *    plugins expect AppKit's main thread: the host thread is the process's
 *    main thread, whose run loop the application keeps running (a Cocoa
 *    application does; a console program calls CFRunLoopRun(), say). Calls
 *    from other threads wait for it.
 *  - brack_send_midi* never waits for the engine and may be called from any
 *    thread, including real-time threads of the client application.
 *  - Functions that return text (JSON) write into a caller buffer: pass
 *    buf=NULL/size=0 to query the required size (including the terminator)
 *    via *needed.
 *  - Plugins run in plugin host processes (brack-host-x64.exe, brack-host-x86.exe;
 *    brack-host-<arch> elsewhere), one each: a plugin that crashes or hangs ends only its own,
 *    and plugins built for another architecture run too. Ship the plugin hosts in the build's
 *    bin (x64 and x86 builds: x86 and x64; ARM64: arm64 too; Apple silicon: x64 and arm64)
 *    beside brack.dll, or for libbrack.so / libbrack.dylib in ../libexec/brack as
 *    cmake --install puts them. A plugin of an architecture with no plugin host does not load.
 *    brack_config.plugins_in_process runs plugins in the caller's process instead.
 *  - Everything the GUI does to a rack, this API does too.
 *
 * Usage
 *    brack_engine* e = brack_engine_create();
 *    char id[BRACK_ID_MAX];
 *    brack_add_plugin(e, NULL, "C:/path/Synth.clap", NULL, id, sizeof id);  // routed to outputs 1/2
 *    brack_config cfg; brack_config_init(&cfg);
 *    cfg.process_sample_rate = 96000;   // plugins at 96 kHz, converted to the device's rate
 *    brack_set_config(e, &cfg);
 *    brack_start(e);                    // or brack_start_manual() and brack_render()
 *    const uint8_t on[3] = {0x90, 60, 100};
 *    brack_send_midi(e, id, 0, on, 3);  // straight to the plugin, from any thread
 *    ...
 *    brack_engine_destroy(e);
 */
#ifndef BRACK_H
#define BRACK_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#if defined(_WIN32)
#if defined(BRACK_BUILDING_DLL)
#define BRACK_API __declspec(dllexport)
#else
#define BRACK_API __declspec(dllimport)
#endif
#define BRACK_CALL __cdecl
#else
#define BRACK_API __attribute__((visibility("default")))
#define BRACK_CALL
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define BRACK_API_VERSION 1

enum {
    BRACK_OK = 0,
    BRACK_ERR_FAILED = -1,          /* see brack_last_error() */
    BRACK_ERR_INVALID_ARGUMENT = -2,
    BRACK_ERR_NOT_FOUND = -3,       /* the plugin, MIDI source or route named does not exist */
    BRACK_ERR_BUFFER_TOO_SMALL = -4,
    BRACK_ERR_QUEUE_FULL = -5,      /* brack_send_midi*: the queue is full, the message dropped */
};

/* Size of a buffer that holds any id Brack generates, terminator included. */
#define BRACK_ID_MAX 64

typedef struct brack_engine brack_engine;

enum brack_log_level { BRACK_LOG_DEBUG = 0, BRACK_LOG_INFO = 1, BRACK_LOG_WARNING = 2, BRACK_LOG_ERROR = 3 };
typedef void(BRACK_CALL* brack_log_fn)(void* user, int level, const char* message);

enum brack_resampler_quality { BRACK_SRC_STANDARD = 0, BRACK_SRC_HIGH = 1, BRACK_SRC_ULTRA = 2 };

enum brack_midi_source_kind {
    BRACK_SOURCE_HARDWARE = 0, /* an existing MIDI input port, opened by name */
    BRACK_SOURCE_VIRTUAL = 1,  /* a virtual port published by brack, visible to other applications */
    BRACK_SOURCE_API = 2,      /* fed only through brack_send_midi_to_source() */
};

/* Structs with a struct_size grow only at their end. The caller sets struct_size to the size its
 * header gives them; brack.dll reads and writes that much of them, no more, and a field the
 * caller's struct does not have takes its default. */
typedef struct brack_config {
    uint32_t struct_size;           /* sizeof(brack_config) */
    const char* audio_device;       /* output device name, NULL or "" = system default */
    uint32_t device_sample_rate;    /* exclusive mode only; 0 = device native rate */
    uint32_t channels;              /* output channels, 0 = 2 */
    uint32_t buffer_frames;         /* device period, 0 = 256 */
    int32_t exclusive;              /* non-zero = WASAPI exclusive mode (Windows; ignored elsewhere) */
    uint32_t process_sample_rate;   /* plugin rate, 0 = same as output (no conversion) */
    uint32_t block_size;            /* max frames per plugin process call, 0 = 256 */
    int32_t resampler_quality;      /* brack_resampler_quality */
    /* Non-zero: plugins run inside this process rather than in a plugin host process each.
     * Faster to load, but a plugin that crashes outside a call from Brack, or hangs, takes the
     * process down. Plugins built for another architecture than brack.dll's run in a plugin host
     * process anyway. */
    int32_t plugins_in_process;
} brack_config;

BRACK_API uint32_t BRACK_CALL brack_api_version(void);
BRACK_API const char* BRACK_CALL brack_version_string(void);
/* Last error message of the calling thread ("" if none). Valid until the next call on this thread. */
BRACK_API const char* BRACK_CALL brack_last_error(void);
/* Global log callback (any thread). NULL restores logging to stderr. */
BRACK_API void BRACK_CALL brack_set_log_callback(brack_log_fn fn, void* user);

BRACK_API brack_engine* BRACK_CALL brack_engine_create(void);
BRACK_API void BRACK_CALL brack_engine_destroy(brack_engine* e);

/* ---- configuration / transport ---- */
/* Fills defaults. Inline: the struct's size is this header's, whatever brack.dll's is. */
static inline void brack_config_init(brack_config* cfg) {
    memset(cfg, 0, sizeof *cfg);
    cfg->struct_size = sizeof *cfg;
    cfg->channels = 2;
    cfg->buffer_frames = 256;
    cfg->block_size = 256;
    cfg->resampler_quality = BRACK_SRC_HIGH;
}
BRACK_API int BRACK_CALL brack_set_config(brack_engine* e, const brack_config* cfg);
/* The configuration last set (not what the device ended up with: see brack_get_status).
 * cfg->struct_size must be set. audio_device points to storage of the calling thread, valid
 * until its next brack_get_config. */
BRACK_API int BRACK_CALL brack_get_config(brack_engine* e, brack_config* cfg);
/* Opens the configured audio device and starts rendering. */
BRACK_API int BRACK_CALL brack_start(brack_engine* e);
/* Manual mode: no audio device. The application calls brack_render() from its own audio thread. */
BRACK_API int BRACK_CALL brack_start_manual(brack_engine* e, uint32_t sample_rate, uint32_t channels, uint32_t max_frames);
BRACK_API void BRACK_CALL brack_stop(brack_engine* e);
/* Manual mode only. outputs[c] receives `frames` float samples for channel c. Real-time safe.
 * macOS, with plugins_in_process: the plugins run on the calling thread, which the application
 * puts in an audio workgroup (a Core Audio I/O callback's thread is in its device's already; one of
 * the application's own joins kAudioDevicePropertyIOThreadOSWorkgroup or an AudioWorkIntervalCreate
 * interval). Elsewhere Apple silicon may run it on efficiency cores, and heavy plugins miss their
 * deadlines. Plugin hosts put their own audio threads in one. */
BRACK_API void BRACK_CALL brack_render(brack_engine* e, float* const* outputs, uint32_t channels, uint32_t frames);
/* Output frames rendered since the engine started (manual or device mode): the clock that
 * brack_send_midi_at() times refer to. Never blocks. In manual mode, the frames of the next
 * brack_render() call start at the position it returns before that call. */
BRACK_API uint64_t BRACK_CALL brack_get_render_position(brack_engine* e);

/* ---- plugins ----
 * plugin_path: a .clap file, a .vst3 (bundle folder or file) or a VST2 .dll; the extension
 * decides the format.
 * id_out (optional) receives the instance id; generated from the plugin name when `id` is NULL/"".
 * id_out_size: at least BRACK_ID_MAX, or strlen(id) + 1 when `id` is given; a smaller one fails
 * with BRACK_ERR_BUFFER_TOO_SMALL and adds nothing. Likewise for brack_add_midi_source.
 * plugin_id selects the plugin inside the file (CLAP id, VST3 class id as 32 hex digits, VST2
 * unique id as its four characters or 0xXXXXXXXX); NULL/"" selects the first. */
/* Also routes the plugin's main output to outputs 1 and 2 (see brack_add_plugin_ex). */
BRACK_API int BRACK_CALL brack_add_plugin(brack_engine* e, const char* id, const char* plugin_path, const char* plugin_id,
                                          char* id_out, size_t id_out_size);
enum brack_add_plugin_flags {
    BRACK_ADD_PLUGIN_NO_AUDIO_ROUTES = 1, /* leave the plugin unrouted (silent until brack_connect_audio) */
};
BRACK_API int BRACK_CALL brack_add_plugin_ex(brack_engine* e, const char* id, const char* plugin_path,
                                             const char* plugin_id, uint32_t flags, char* id_out, size_t id_out_size);
BRACK_API int BRACK_CALL brack_remove_plugin(brack_engine* e, const char* plugin_id);
/* Loads the plugin again as a new instance, with the state a session would save for it, keeping
 * its id, name and routes: for a plugin that crashed (BRACK_EVENT_PLUGIN_CRASHED) or failed to
 * load. One that crashed with plugins_in_process cannot load again until the process restarts. */
BRACK_API int BRACK_CALL brack_reload_plugin(brack_engine* e, const char* plugin_id);
/* Moves a plugin to position `index` in the list (clamped): the order status, sessions and
 * UIs show. Processing does not depend on it. */
BRACK_API int BRACK_CALL brack_move_plugin(brack_engine* e, const char* plugin_id, uint32_t index);
/* Opens (visible != 0) or closes the plugin's editor in a window of its own. */
BRACK_API int BRACK_CALL brack_show_plugin_gui(brack_engine* e, const char* plugin_id, int visible);
/* Opens the plugin's editor inside parent_window, a window of the application's, as a child at
 * its top left without a frame. Give it a window of its own (a container) and size that to the
 * editor: BRACK_EVENT_EDITOR_RESIZED reports the editor's size. An editor open elsewhere moves
 * here. Close it with brack_show_plugin_gui(e, id, 0) before destroying parent_window.
 * parent_window is the platform's window handle: an HWND on Windows, an NSView* on macOS (with
 * plugins_in_process only: a view is not shared between processes), an X11 Window id cast to a
 * pointer on Linux.
 * Windows: the editor runs on Brack's thread; Brack keeps answering messages sent to the
 * application's windows while a call waits for that thread. */
BRACK_API int BRACK_CALL brack_show_plugin_gui_in(brack_engine* e, const char* plugin_id, void* parent_window);
/* Changes the display name only (saved in sessions, shown in editor titles and logs); the id
 * stays. NULL or "" reverts to the plugin's own name. */
BRACK_API int BRACK_CALL brack_set_plugin_name(brack_engine* e, const char* plugin_id, const char* name);
/* The plugin's state as a session saves it, in the plugin's own format (binary). For a plugin
 * that failed to load, the state it was given. buf=NULL/size=0 queries the size via *needed. */
BRACK_API int BRACK_CALL brack_get_plugin_state(brack_engine* e, const char* plugin_id, uint8_t* buf, size_t size,
                                                size_t* needed);
BRACK_API int BRACK_CALL brack_set_plugin_state(brack_engine* e, const char* plugin_id, const uint8_t* data,
                                                size_t size);

/* ---- MIDI sources ---- */
BRACK_API int BRACK_CALL brack_add_midi_source(brack_engine* e, const char* id, int kind, const char* name,
                                               char* id_out, size_t id_out_size);
BRACK_API int BRACK_CALL brack_remove_midi_source(brack_engine* e, const char* source_id);
BRACK_API int BRACK_CALL brack_move_midi_source(brack_engine* e, const char* source_id, uint32_t index);
/* Opens the source's port again (a hardware port that came back, say). Hardware sources are
 * also reopened automatically once their port is listed again. */
BRACK_API int BRACK_CALL brack_reopen_midi_source(brack_engine* e, const char* source_id);

/* ---- routing ---- */
BRACK_API int BRACK_CALL brack_connect_midi(brack_engine* e, const char* source_id, const char* plugin_id, uint32_t note_port);
BRACK_API int BRACK_CALL brack_disconnect_midi(brack_engine* e, const char* source_id, const char* plugin_id, uint32_t note_port);
/* Routes channel `channel` of audio output port `port` of a plugin to engine output `output`
 * (adds to the plugin's existing audio routes; for a route that exists, sets its gain). */
BRACK_API int BRACK_CALL brack_connect_audio(brack_engine* e, const char* plugin_id, uint32_t port, uint32_t channel,
                                             uint32_t output, float gain);
BRACK_API int BRACK_CALL brack_disconnect_audio(brack_engine* e, const char* plugin_id, uint32_t port, uint32_t channel,
                                                uint32_t output);
BRACK_API int BRACK_CALL brack_clear_audio_routes(brack_engine* e, const char* plugin_id);
/* Linear gain applied to every output channel after the SRC (1 = unity, 0 = silent; saved in
 * sessions). Takes effect on the next buffer, ramped. Never blocks: safe from any thread. */
BRACK_API int BRACK_CALL brack_set_master_gain(brack_engine* e, float gain);
BRACK_API float BRACK_CALL brack_get_master_gain(brack_engine* e);

/* ---- direct MIDI injection (no MIDI port involved) ----
 * One complete MIDI 1.0 message per call: 1-3 bytes, or a SysEx F0 ... F7.
 * The bytes reach the plugin unchanged, in call order. With an audio device, a message lands a
 * device period after the call, at the frame matching when in the period it came, as messages
 * from MIDI ports do: a client sending as things happen needs no clock of its own. In manual
 * mode it lands at the start of the next brack_render(). */
BRACK_API int BRACK_CALL brack_send_midi(brack_engine* e, const char* plugin_id, uint32_t note_port, const uint8_t* data,
                                         uint32_t size);
/* Feeds a MIDI source (normally a BRACK_SOURCE_API one); routing decides which plugins receive it. */
BRACK_API int BRACK_CALL brack_send_midi_to_source(brack_engine* e, const char* source_id, const uint8_t* data,
                                                   uint32_t size);
/* The same, delivered at render position `time` (brack_get_render_position), frame accurate:
 * to play at frame f of the next brack_render() call, pass the position before that call + f.
 * A time already rendered means as soon as possible. With sample rate conversion
 * (process_sample_rate), the conversion's latency delays everything alike, and a message sent
 * less than a block ahead may land up to one block late. */
BRACK_API int BRACK_CALL brack_send_midi_at(brack_engine* e, const char* plugin_id, uint32_t note_port, uint64_t time,
                                            const uint8_t* data, uint32_t size);
BRACK_API int BRACK_CALL brack_send_midi_to_source_at(brack_engine* e, const char* source_id, uint64_t time,
                                                      const uint8_t* data, uint32_t size);
/* Brack's clock: nanoseconds, steadily increasing (the C++ steady_clock). Never blocks. */
BRACK_API int64_t BRACK_CALL brack_now_ns(void);
/* The same, for the moment `time` on Brack's clock (brack_now_ns() plus how far ahead): with an
 * audio device, it lands where a message sent with brack_send_midi at that moment would, so a
 * client may send ahead of time to keep its own thread's jitter out. A moment already past means
 * as soon as possible. Messages held until their moment take room in the plugin's queue, so
 * send seconds ahead sparingly. In manual mode it lands as brack_send_midi's do (brack_render()
 * calls are the clock there; time messages with brack_send_midi_at). */
BRACK_API int BRACK_CALL brack_send_midi_at_time(brack_engine* e, const char* plugin_id, uint32_t note_port,
                                                 int64_t time, const uint8_t* data, uint32_t size);
BRACK_API int BRACK_CALL brack_send_midi_to_source_at_time(brack_engine* e, const char* source_id, int64_t time,
                                                           const uint8_t* data, uint32_t size);

/* ---- sessions ----
 * Loading replaces the rack, or fails and leaves it untouched. A running engine restarts with
 * the session's configuration; if that fails (say, its audio device is missing) the load still
 * succeeds and the engine is left stopped: see brack_get_status and the log. */
BRACK_API int BRACK_CALL brack_load_session(brack_engine* e, const char* path);
BRACK_API int BRACK_CALL brack_save_session(brack_engine* e, const char* path);
BRACK_API int BRACK_CALL brack_load_session_json(brack_engine* e, const char* json);
BRACK_API int BRACK_CALL brack_save_session_json(brack_engine* e, char* buf, size_t size, size_t* needed);
/* Removes every plugin, MIDI source and route. The configuration and master gain stay. */
BRACK_API int BRACK_CALL brack_clear(brack_engine* e);
/* Goes up whenever what a session saves changes: the rack, routing, configuration, master gain,
 * or a plugin's state as the plugin reports it (an edit in its editor, say). MIDI does not
 * count. Compare with the value at the last save to know whether to save. Never blocks. */
BRACK_API uint64_t BRACK_CALL brack_change_count(brack_engine* e);

/* ---- events ----
 * Things that happen without a call: a plugin crashes, an editor is closed, the device stops.
 * Queued (the latest 1024 kept); take them with brack_poll_event. */
enum brack_event_type {
    BRACK_EVENT_CHANGED = 1,                  /* brack_change_count went up; at most one queued */
    BRACK_EVENT_PLUGIN_CRASHED = 2,           /* id: the plugin, message: where it crashed */
    BRACK_EVENT_EDITOR_CLOSED = 3,            /* id: the plugin whose editor closed */
    BRACK_EVENT_EDITOR_RESIZED = 4,           /* id: the plugin; width/height: editor size, physical pixels */
    BRACK_EVENT_DEVICE_STALLED = 5,           /* the output device stopped calling back */
    BRACK_EVENT_DEVICE_RESUMED = 6,           /* and is back */
    BRACK_EVENT_MIDI_SOURCE_LOST = 7,         /* id: the source whose port went away, message: why */
    BRACK_EVENT_MIDI_SOURCE_RECONNECTED = 8,  /* id: the source, open again */
};
typedef struct brack_event {
    uint32_t struct_size; /* sizeof(brack_event), set by the caller */
    int32_t type;         /* brack_event_type */
    char id[256];         /* plugin or source id, "" if none (cut short at a character boundary if longer) */
    char message[1024];   /* "" if none (cut short likewise) */
    uint32_t width, height;
} brack_event;
/* 1 and *out filled if an event was queued, 0 if none, negative on error. Never blocks. */
BRACK_API int BRACK_CALL brack_poll_event(brack_engine* e, brack_event* out);
/* Called whenever an event is queued, on Brack's thread: return quickly, for example by waking
 * a thread of your own that then calls brack_poll_event. NULL stops the calls. */
typedef void(BRACK_CALL* brack_event_notify_fn)(void* user);
BRACK_API int BRACK_CALL brack_set_event_notify(brack_engine* e, brack_event_notify_fn fn, void* user);

/* ---- inspection (JSON) ----
 * brack_get_status: engine state, plugins (with note/audio ports, and the architecture they run
 * as and whether in a plugin host process), sources, routes. Waits for Brack's thread, which
 * may be busy (loading a plugin, say).
 * brack_get_status_cached: the same, as of at most ~50 ms ago (meters current), without waiting.
 * brack_list_audio_devices / brack_list_midi_inputs / brack_scan_plugins: arrays. */
BRACK_API int BRACK_CALL brack_get_status(brack_engine* e, char* buf, size_t size, size_t* needed);
BRACK_API int BRACK_CALL brack_get_status_cached(brack_engine* e, char* buf, size_t size, size_t* needed);
BRACK_API int BRACK_CALL brack_list_audio_devices(char* buf, size_t size, size_t* needed);
BRACK_API int BRACK_CALL brack_list_midi_inputs(char* buf, size_t size, size_t* needed);
/* Lists the plugins in the standard CLAP, VST3 and VST2 locations and in extra_dirs (optional,
 * ';'-separated). Runs on the calling thread and holds up no engine. It loads every plugin file
 * in a plugin host process of the file's architecture (VST2 plugins are even instantiated), so
 * query the size with a generous buffer rather than an empty one, or give a cache.
 * cache_path (optional): a file of the application's (created if missing); files unchanged since
 * it was written are not loaded again. Plugins of another architecture are listed too, and run
 * in that architecture's plugin host; those of an architecture no plugin host here runs are left
 * out, but kept in the cache, so builds of every architecture can share it. */
enum brack_scan_flags {
    BRACK_SCAN_EXTRA_DIRS_ONLY = 1, /* skip the standard locations (and CLAP_PATH, VST3_PATH, VST_PATH) */
};
BRACK_API int BRACK_CALL brack_scan_plugins(const char* extra_dirs, const char* cache_path, uint32_t flags, char* buf,
                                            size_t size, size_t* needed);
/* 1 if virtual MIDI ports can be published on this system; otherwise 0 and brack_last_error() says why. */
BRACK_API int BRACK_CALL brack_virtual_midi_available(void);
/* 1 if this Windows build has the Windows MIDI Services bug (microsoft/MIDI#1047) that wedges
 * the MIDI service when a virtual port is removed (including when the engine is destroyed with
 * one): MIDI may then stop working until the next reboot. Warn the user before creating
 * virtual ports. 0 if not affected or virtual ports are unavailable. */
BRACK_API int BRACK_CALL brack_virtual_midi_removal_hangs(void);

#ifdef __cplusplus
}
#endif

#endif /* BRACK_H */
