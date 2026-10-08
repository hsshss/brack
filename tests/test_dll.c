/* Drives brack.dll (libbrack.so) through its C API only: load a plugin, inject MIDI directly,
 * render; then everything else an application uses: events, rack edits, plugin state, timed MIDI,
 * an editor inside the application's window (Windows), cached scans, and the test synth of every
 * format and architecture in one scan and one rack. */
#ifdef _WIN32
#include <windows.h>
#else
#include <limits.h>
#include <time.h>
#include <unistd.h>
#endif
#ifdef __APPLE__
#include <CoreFoundation/CoreFoundation.h>
#include <pthread.h>
#endif

#include <brack/brack.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#ifdef _WIN32
#define sleep_ms(ms) Sleep(ms)
#define count_up(n) InterlockedIncrement(n)
typedef LONG counter;
/* An editor in a window of this thread's needs its messages answered. */
static void pump_messages(void) {
    MSG msg;
    while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) DispatchMessageW(&msg);
}
static void temp_file(char* out, size_t size, const char* name) {
    GetTempPathA((DWORD)size, out);
    strncat(out, name, size - strlen(out) - 1);
}
#else
#define MAX_PATH 4096
static void sleep_ms(unsigned ms) {
    struct timespec t = {(time_t)(ms / 1000), (long)(ms % 1000) * 1000000L};
    nanosleep(&t, NULL);
}
#define count_up(n) __atomic_add_fetch(n, 1, __ATOMIC_SEQ_CST)
typedef long counter;
static void pump_messages(void) {}
static void temp_file(char* out, size_t size, const char* name) { snprintf(out, size, "/tmp/%s", name); }
#endif

static int failures = 0;
#define CHECK(cond)                                                                     \
    do {                                                                                \
        if (!(cond)) {                                                                  \
            fprintf(stderr, "CHECK failed: %s (line %d): %s\n", #cond, __LINE__, brack_last_error()); \
            ++failures;                                                                 \
        }                                                                               \
    } while (0)

static float render_peak(brack_engine* e, unsigned total) {
    static float l[256], r[256];
    float* out[2] = {l, r};
    float peak = 0;
    for (unsigned done = 0; done < total; done += 256) {
        brack_render(e, out, 2, 256);
        for (int i = 0; i < 256; ++i) {
            if (fabsf(l[i]) > peak) peak = fabsf(l[i]);
            if (fabsf(r[i]) > peak) peak = fabsf(r[i]);
        }
    }
    return peak;
}

static void BRACK_CALL on_log(void* user, int level, const char* msg) {
    (void)user;
    printf("  [log %d] %s\n", level, msg);
}

static volatile counter notified = 0;
static void BRACK_CALL on_event(void* user) {
    (void)user;
    count_up(&notified);
}

/* The status JSON; free() it. */
static char* status_of(brack_engine* e, int cached) {
    size_t needed = 0;
    if ((cached ? brack_get_status_cached : brack_get_status)(e, NULL, 0, &needed) != BRACK_OK) return NULL;
    char* s = (char*)malloc(needed);
    if (s && (cached ? brack_get_status_cached : brack_get_status)(e, s, needed, NULL) != BRACK_OK) s[0] = 0;
    return s;
}

/* Waits up to two seconds for an event of `type`, answering window messages meanwhile (an
 * editor in a window of this thread's needs that). Events of other types are skipped. */
static int wait_event(brack_engine* e, int type, brack_event* out) {
    for (int i = 0; i < 200; ++i) {
        out->struct_size = sizeof *out;
        while (brack_poll_event(e, out) == 1)
            if (out->type == type) return 1;
        pump_messages();
        sleep_ms(10);
    }
    return 0;
}

#ifdef _WIN32
static void drain_events(brack_engine* e) {
    brack_event ev;
    ev.struct_size = sizeof ev;
    sleep_ms(50);
    while (brack_poll_event(e, &ev) == 1) {
    }
}
#endif

static int file_has_line(const char* path, const char* line) {
    FILE* f = fopen(path, "rb");
    if (!f) return 0;
    char buf[512];
    int found = 0;
    while (!found && fgets(buf, sizeof buf, f)) {
        buf[strcspn(buf, "\r\n")] = 0;
        found = strcmp(buf, line) == 0;
    }
    fclose(f);
    return found;
}

static void events_and_changes(const char* synth) {
    brack_engine* e = brack_engine_create();
    CHECK(brack_set_event_notify(e, on_event, NULL) == BRACK_OK);
    const uint64_t c0 = brack_change_count(e);
    CHECK(brack_add_plugin(e, "a", synth, NULL, NULL, 0) == BRACK_OK);
    CHECK(brack_change_count(e) > c0);
    brack_event ev;
    CHECK(wait_event(e, BRACK_EVENT_CHANGED, &ev));

    /* Changes coalesce into one queued CHANGED event. */
    CHECK(brack_set_master_gain(e, 0.5f) == BRACK_OK);
    CHECK(brack_set_master_gain(e, 0.6f) == BRACK_OK);
    CHECK(brack_set_master_gain(e, 0.7f) == BRACK_OK);
    sleep_ms(100);
    int changed = 0;
    ev.struct_size = sizeof ev;
    while (brack_poll_event(e, &ev) == 1) changed += ev.type == BRACK_EVENT_CHANGED;
    CHECK(changed == 1);
    CHECK(brack_poll_event(e, &ev) == 0);

    /* A plugin crashing on the audio thread is reported with where it crashed. */
    CHECK(brack_start_manual(e, 48000, 2, 256) == BRACK_OK);
    const uint8_t crash[] = {0xF0, 0x7D, 0x63, 0x01, 0xF7};
    CHECK(brack_send_midi(e, "a", 0, crash, sizeof crash) == BRACK_OK);
    render_peak(e, 512);
    CHECK(wait_event(e, BRACK_EVENT_PLUGIN_CRASHED, &ev));
    printf("crash event: %s: %s\n", ev.id, ev.message);
    /* Caught where Brack called it. */
#ifdef _WIN32
    const char* expected = "crashed in process: access violation";
#else
    const char* expected = "crashed in process: segmentation fault";
#endif
    CHECK(strcmp(ev.id, "a") == 0 && strstr(ev.message, expected) != NULL);
    CHECK(notified > 0);

    /* Loaded again (in a new plugin host process), it plays. */
    CHECK(brack_reload_plugin(e, "a") == BRACK_OK);
    const uint8_t on[] = {0x90, 0x3C, 0x64};
    CHECK(brack_send_midi(e, "a", 0, on, 3) == BRACK_OK);
    CHECK(render_peak(e, 4800) > 0.1f);
    CHECK(brack_reload_plugin(e, "nope") == BRACK_ERR_NOT_FOUND);

    CHECK(brack_set_event_notify(e, NULL, NULL) == BRACK_OK);
    ev.struct_size = 4;
    CHECK(brack_poll_event(e, &ev) == BRACK_ERR_INVALID_ARGUMENT);
    brack_engine_destroy(e);
}

static void rack_edits(const char* synth) {
    brack_engine* e = brack_engine_create();
    brack_config cfg;
    brack_config_init(&cfg);
    cfg.audio_device = "brack test device";
    cfg.block_size = 128;
    cfg.resampler_quality = BRACK_SRC_STANDARD;
    CHECK(brack_set_config(e, &cfg) == BRACK_OK);
    brack_config got;
    memset(&got, 0, sizeof got);
    got.struct_size = sizeof got;
    CHECK(brack_get_config(e, &got) == BRACK_OK);
    CHECK(strcmp(got.audio_device, "brack test device") == 0 && got.block_size == 128 &&
          got.resampler_quality == BRACK_SRC_STANDARD && got.channels == 2 && got.plugins_in_process == 0);
    cfg.plugins_in_process = 1;
    CHECK(brack_set_config(e, &cfg) == BRACK_OK);
    CHECK(brack_get_config(e, &got) == BRACK_OK && got.plugins_in_process == 1);
    cfg.plugins_in_process = 0;
    CHECK(brack_set_config(e, &cfg) == BRACK_OK);

    /* A caller whose header has larger structs (a newer brack.h than this brack.dll): what the
     * library knows is read and written, and the rest of the caller's memory is left alone. */
    struct {
        brack_config cfg;
        unsigned char rest[16];
    } newer;
    brack_config_init(&newer.cfg);
    memset(newer.rest, 0xAB, sizeof newer.rest);
    newer.cfg.struct_size = sizeof newer;
    newer.cfg.block_size = 64;
    CHECK(brack_set_config(e, &newer.cfg) == BRACK_OK);
    newer.cfg.block_size = 0;
    CHECK(brack_get_config(e, &newer.cfg) == BRACK_OK && newer.cfg.block_size == 64 && newer.cfg.struct_size == sizeof newer);
    int untouched = 1;
    for (size_t i = 0; i < sizeof newer.rest; ++i) untouched = untouched && newer.rest[i] == 0xAB;
    struct {
        brack_event ev;
        unsigned char rest[16];
    } newerEvent;
    memset(&newerEvent, 0xAB, sizeof newerEvent);
    newerEvent.ev.struct_size = sizeof newerEvent;
    int polled = 0;
    for (int i = 0; i < 100 && polled != 1; ++i) {
        polled = brack_poll_event(e, &newerEvent.ev); /* the configuration changed */
        if (polled != 1) sleep_ms(10);
    }
    for (size_t i = 0; i < sizeof newerEvent.rest; ++i) untouched = untouched && newerEvent.rest[i] == 0xAB;
    printf("larger structs: %s\n", untouched ? "the rest left alone" : "WRITTEN PAST WHAT THE LIBRARY KNOWS");
    CHECK(polled == 1 && untouched);
    CHECK(brack_set_config(e, &cfg) == BRACK_OK);

    CHECK(brack_start_manual(e, 48000, 2, 256) == BRACK_OK);
    CHECK(brack_add_plugin(e, "a", synth, NULL, NULL, 0) == BRACK_OK);
    CHECK(brack_add_plugin_ex(e, "b", synth, NULL, BRACK_ADD_PLUGIN_NO_AUDIO_ROUTES, NULL, 0) == BRACK_OK);

    /* b is silent until routed; one route can be taken away again. */
    const uint8_t on[3] = {0x90, 69, 127};
    CHECK(brack_send_midi(e, "b", 0, on, 3) == BRACK_OK);
    CHECK(render_peak(e, 4800) == 0.0f);
    CHECK(brack_connect_audio(e, "b", 0, 0, 0, 1.0f) == BRACK_OK);
    CHECK(render_peak(e, 4800) > 0.1f);
    CHECK(brack_disconnect_audio(e, "b", 0, 0, 0) == BRACK_OK);
    CHECK(brack_disconnect_audio(e, "b", 0, 0, 0) == BRACK_ERR_NOT_FOUND);
    render_peak(e, 4800);
    CHECK(render_peak(e, 4800) == 0.0f);

    /* Order of plugins and sources. */
    CHECK(brack_move_plugin(e, "b", 0) == BRACK_OK);
    CHECK(brack_move_plugin(e, "nope", 0) == BRACK_ERR_NOT_FOUND);
    CHECK(brack_add_midi_source(e, "s1", BRACK_SOURCE_API, NULL, NULL, 0) == BRACK_OK);
    CHECK(brack_add_midi_source(e, "s2", BRACK_SOURCE_API, NULL, NULL, 0) == BRACK_OK);
    CHECK(brack_move_midi_source(e, "s2", 0) == BRACK_OK);
    CHECK(brack_reopen_midi_source(e, "s1") == BRACK_OK);
    CHECK(brack_reopen_midi_source(e, "nope") != BRACK_OK);
    char* s = status_of(e, 0);
    CHECK(s && strstr(s, "\"id\": \"b\"") < strstr(s, "\"id\": \"a\""));
    CHECK(s && strstr(s, "\"id\": \"s2\"") < strstr(s, "\"id\": \"s1\""));
    free(s);

    /* The cached status does not wait for the engine; it catches up within ~50 ms. */
    sleep_ms(100);
    s = status_of(e, 1);
    CHECK(s && strstr(s, "\"mode\": \"manual\"") && strstr(s, "\"id\": \"s2\""));
    free(s);

    CHECK(brack_clear(e) == BRACK_OK);
    s = status_of(e, 0);
    CHECK(s && strstr(s, "\"plugins\": []") && strstr(s, "\"midiSources\": []"));
    free(s);
    CHECK(brack_get_master_gain(e) == 1.0f);
    brack_engine_destroy(e);
}

static void plugin_state(const char* synth) {
    brack_engine* e = brack_engine_create();
    CHECK(brack_add_plugin(e, "a", synth, NULL, NULL, 0) == BRACK_OK);
    size_t needed = 0;
    CHECK(brack_get_plugin_state(e, "a", NULL, 0, &needed) == BRACK_OK && needed == sizeof(float));
    const float gain = 0.5f;
    CHECK(brack_set_plugin_state(e, "a", (const uint8_t*)&gain, sizeof gain) == BRACK_OK);
    float back = 0;
    CHECK(brack_get_plugin_state(e, "a", (uint8_t*)&back, sizeof back, NULL) == BRACK_OK && back == 0.5f);
    uint8_t one;
    CHECK(brack_get_plugin_state(e, "a", &one, 1, NULL) == BRACK_ERR_BUFFER_TOO_SMALL);
    CHECK(brack_set_plugin_state(e, "a", (const uint8_t*)&gain, 2) == BRACK_ERR_FAILED); /* the plugin rejects it */
    CHECK(brack_get_plugin_state(e, "nope", NULL, 0, &needed) != BRACK_OK);
    brack_engine_destroy(e);
}

/* Messages land on the frame they were timed for (the test synth logs frames). */
static void timed_midi(const char* synth, const char* log) {
    brack_engine* e = brack_engine_create();
    CHECK(brack_start_manual(e, 48000, 2, 256) == BRACK_OK); /* plugin blocks of 256 frames */
    CHECK(brack_add_plugin(e, "t", synth, NULL, NULL, 0) == BRACK_OK);
    CHECK(brack_add_midi_source(e, "src", BRACK_SOURCE_API, NULL, NULL, 0) == BRACK_OK);
    CHECK(brack_connect_midi(e, "src", "t", 0) == BRACK_OK);
    const uint64_t pos = brack_get_render_position(e);
    CHECK(pos == 0);
    const uint8_t n60[3] = {0x90, 60, 100}, n62[3] = {0x90, 62, 100}, n64[3] = {0x90, 64, 100};
    const uint8_t n65[3] = {0x90, 65, 100}, n67[3] = {0x90, 67, 100};
    CHECK(brack_send_midi_at(e, "t", 0, pos + 1000, n64, 3) == BRACK_OK); /* sent out of order */
    CHECK(brack_send_midi_at(e, "t", 0, pos + 100, n60, 3) == BRACK_OK);
    CHECK(brack_send_midi_at(e, "t", 0, pos + 300, n62, 3) == BRACK_OK);
    CHECK(brack_send_midi_to_source_at(e, "src", pos + 10, n67, 3) == BRACK_OK);
    render_peak(e, 1024);
    CHECK(brack_get_render_position(e) == 1024);
    CHECK(brack_send_midi_at(e, "t", 0, 5, n65, 3) == BRACK_OK); /* already past: at once */
    render_peak(e, 256);
    /* Manual mode has no real time to go by: a time on Brack's clock is as soon as possible. */
    const uint8_t n69[3] = {0x90, 69, 100};
    const int64_t now = brack_now_ns();
    CHECK(now > 0 && brack_now_ns() >= now);
    CHECK(brack_send_midi_at_time(e, "t", 0, now + 1000000000, n69, 3) == BRACK_OK);
    CHECK(brack_send_midi_at_time(e, "t", 0, 0, n69, 3) == BRACK_ERR_INVALID_ARGUMENT);
    render_peak(e, 256);
    CHECK(brack_remove_plugin(e, "t") == BRACK_OK); /* writes the log */
    brack_engine_destroy(e);
    CHECK(file_has_line(log, "midi port=0 @10: 90 43 64"));
    CHECK(file_has_line(log, "midi port=0 @100: 90 3C 64"));
    CHECK(file_has_line(log, "midi port=0 @44: 90 3E 64"));  /* 300 = 256 + 44 */
    CHECK(file_has_line(log, "midi port=0 @232: 90 40 64")); /* 1000 = 768 + 232 */
    CHECK(file_has_line(log, "midi port=0: 90 41 64"));
    CHECK(file_has_line(log, "midi port=0: 90 45 64"));
}

#ifdef _WIN32
static LRESULT CALLBACK parent_proc(HWND h, UINT m, WPARAM w, LPARAM l) { return DefWindowProcW(h, m, w, l); }

/* The editor inside a window of this thread's, as an application would host it. */
static void embedded_editor(const char* synth) {
    WNDCLASSW wc;
    memset(&wc, 0, sizeof wc);
    wc.lpfnWndProc = parent_proc;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.lpszClassName = L"BrackTestParent";
    RegisterClassW(&wc);
    HWND parent = CreateWindowExW(0, L"BrackTestParent", L"parent", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, 0, 0, 640,
                                  480, NULL, NULL, wc.hInstance, NULL);
    CHECK(parent != NULL);

    brack_engine* e = brack_engine_create();
    CHECK(brack_add_plugin(e, "g", synth, NULL, NULL, 0) == BRACK_OK);
    drain_events(e);
    CHECK(brack_show_plugin_gui_in(e, "g", NULL) == BRACK_ERR_INVALID_ARGUMENT);
    CHECK(brack_show_plugin_gui_in(e, "g", parent) == BRACK_OK);
    /* The synth's editor says it is half as big until it has a parent window: the last size counts. */
    brack_event ev;
    memset(&ev, 0, sizeof ev);
    int sized = 0;
    for (int i = 0; i < 4 && !(sized && ev.width == 320); ++i) sized = wait_event(e, BRACK_EVENT_EDITOR_RESIZED, &ev);
    CHECK(sized);
    printf("editor: %s %ux%u\n", ev.id, ev.width, ev.height);
    CHECK(strcmp(ev.id, "g") == 0 && ev.width == 320 && ev.height == 200);
    HWND frame = FindWindowExW(parent, NULL, L"BrackPluginEditor", NULL);
    CHECK(frame != NULL && IsChild(parent, frame));
    CHECK(frame != NULL && FindWindowExW(frame, NULL, L"Static", NULL) != NULL); /* the plugin's own window */

    CHECK(brack_show_plugin_gui(e, "g", 0) == BRACK_OK);
    CHECK(wait_event(e, BRACK_EVENT_EDITOR_CLOSED, &ev) && strcmp(ev.id, "g") == 0);
    CHECK(FindWindowExW(parent, NULL, L"BrackPluginEditor", NULL) == NULL);
    brack_engine_destroy(e);
    DestroyWindow(parent);
}
#endif

/* `path` made absolute (as a scan lists files), or as it is if it cannot be. */
static void absolute(const char* path, char* out, size_t size) {
#ifdef _WIN32
    if (_fullpath(out, path, size)) return;
#else
    char full[PATH_MAX];
    if (realpath(path, full) && strlen(full) < size) {
        strcpy(out, full);
        return;
    }
#endif
    snprintf(out, size, "%s", path);
}

static const char* file_name(const char* path) {
    const char* slash = strrchr(path, '/');
    const char* back = strrchr(path, '\\');
    if (back > slash) slash = back;
    return slash ? slash + 1 : path;
}

/* The folder `path` is in, made absolute. */
static void folder_of(const char* path, char* out, size_t size) {
    absolute(path, out, size);
    const char* name = file_name(out);
    if (name > out) out[name - out - 1] = 0;
}

static void cached_scan(const char* synth) {
    char dir[MAX_PATH], cache[MAX_PATH];
    folder_of(synth, dir, sizeof dir);
    temp_file(cache, sizeof cache, sizeof(void*) == 8 ? "brack_test_dll_cache-64.json" : "brack_test_dll_cache-32.json");
    remove(cache);

    for (int pass = 0; pass < 2; ++pass) { /* the second from the cache */
        size_t needed = 0;
        CHECK(brack_scan_plugins(dir, cache, BRACK_SCAN_EXTRA_DIRS_ONLY, NULL, 0, &needed) == BRACK_OK);
        char* json = (char*)malloc(needed);
        CHECK(json &&
              brack_scan_plugins(dir, cache, BRACK_SCAN_EXTRA_DIRS_ONLY, json, needed, NULL) == BRACK_OK);
        CHECK(json && strstr(json, "\"brack.test.synth\"") != NULL);
        free(json);
        FILE* written = fopen(cache, "rb");
        CHECK(written != NULL);
        if (written) fclose(written);
    }
    remove(cache);

    const size_t size = 256 * 1024; /* without a cache, a size query would scan twice */
    char* json = (char*)malloc(size);
    CHECK(json && brack_scan_plugins(dir, NULL, BRACK_SCAN_EXTRA_DIRS_ONLY, json, size, NULL) == BRACK_OK);
    CHECK(json && strstr(json, "\"brack.test.synth\"") != NULL);
    free(json);
}

/* Each failure says which kind it is, and one that would hand back an id cut short adds nothing. */
static void error_codes(const char* synth) {
    brack_engine* e = brack_engine_create();
    char small[8], id[BRACK_ID_MAX], again[BRACK_ID_MAX];
    CHECK(brack_add_plugin(e, NULL, synth, NULL, small, sizeof small) == BRACK_ERR_BUFFER_TOO_SMALL);
    CHECK(brack_add_plugin(e, "longer-id", synth, NULL, small, sizeof small) == BRACK_ERR_BUFFER_TOO_SMALL);
    CHECK(brack_remove_plugin(e, "longer-id") == BRACK_ERR_NOT_FOUND);
    CHECK(brack_add_plugin(e, "p", synth, NULL, small, sizeof small) == BRACK_OK && strcmp(small, "p") == 0);

    char name[200];
    memset(name, 'a', sizeof name - 1);
    name[sizeof name - 1] = 0;
    CHECK(brack_add_midi_source(e, NULL, BRACK_SOURCE_API, name, id, sizeof id) == BRACK_OK);
    CHECK(brack_add_midi_source(e, NULL, BRACK_SOURCE_API, name, again, sizeof again) == BRACK_OK);
    CHECK(strlen(id) < BRACK_ID_MAX && strlen(again) < BRACK_ID_MAX && strcmp(id, again) != 0);

    CHECK(brack_show_plugin_gui(e, "nope", 1) == BRACK_ERR_NOT_FOUND);
    CHECK(brack_get_plugin_state(e, "nope", NULL, 0, NULL) == BRACK_ERR_NOT_FOUND);
    CHECK(brack_reopen_midi_source(e, "nope") == BRACK_ERR_NOT_FOUND);
    CHECK(brack_connect_midi(e, "nope", "p", 0) == BRACK_ERR_NOT_FOUND);
    CHECK(brack_connect_midi(e, id, "nope", 0) == BRACK_ERR_NOT_FOUND);
    CHECK(brack_connect_midi(e, id, "p", 99) == BRACK_ERR_FAILED);
    CHECK(brack_connect_midi(e, id, "p", 0x10000) == BRACK_ERR_INVALID_ARGUMENT); /* not port 0 */

    const uint8_t on[3] = {0x90, 60, 100}, bad[2] = {60, 100};
    CHECK(brack_send_midi(e, "nope", 0, on, 3) == BRACK_ERR_NOT_FOUND);
    CHECK(brack_send_midi_to_source(e, "nope", on, 3) == BRACK_ERR_NOT_FOUND);
    CHECK(brack_send_midi(e, "p", 0, bad, 2) == BRACK_ERR_INVALID_ARGUMENT);
    CHECK(brack_send_midi(e, "p", 0x10000, on, 3) == BRACK_ERR_INVALID_ARGUMENT);
    int result = BRACK_OK; /* nothing renders, so the queue fills */
    for (int i = 0; i < 1000000 && result == BRACK_OK; ++i) result = brack_send_midi(e, "p", 0, on, 3);
    CHECK(result == BRACK_ERR_QUEUE_FULL);
    brack_engine_destroy(e);
}

static int exists(const char* path) {
    struct stat st;
    return stat(path, &st) == 0;
}

/* Whether the JSON string at `json` (past its opening quote) is `path`: either separator, and on
 * Windows either case, as the scan writes paths its own way. */
static int same_path(const char* json, const char* path) {
    for (;; ++json, ++path) {
        char a = *json, b = *path;
        if (a == '"') return b == 0;
        if (a == '\\') a = *++json; /* an escape: the character it stands for */
        if (a == '\\') a = '/';
        if (b == '\\') b = '/';
#ifdef _WIN32
        if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
        if (b >= 'A' && b <= 'Z') b = (char)(b - 'A' + 'a');
#endif
        if (a != b || b == 0) return 0;
    }
}

/* The text value of `key` in the scan's entry for `path` (empty if the scan does not list it). */
static void listed(const char* scan, const char* path, const char* key, char* out, size_t size) {
    static const char tag[] = "\"path\": \"";
    out[0] = 0;
    const char* at = scan;
    while ((at = strstr(at, tag)) != NULL && !same_path(at + strlen(tag), path)) at += strlen(tag);
    if (!at) return;
    while (at > scan && *at != '{') --at; /* an entry holds no other object */
    char k[64];
    snprintf(k, sizeof k, "\"%s\": \"", key);
    const char* v = strstr(at, k);
    if (!v) return;
    v += strlen(k);
    size_t n = strcspn(v, "\"");
    if (n >= size) n = size - 1;
    memcpy(out, v, n);
    out[n] = 0;
}

/* The value of `key` in the status's entry for plugin `id`. Its keys come sorted, and its nested
 * objects (ports) hold none of these: "architecture" comes before "id", "separateProcess" after. */
static void plugin_field(const char* status, const char* id, const char* key, char* out, size_t size) {
    char want[96], k[64];
    snprintf(want, sizeof want, "\"id\": \"%s\"", id);
    snprintf(k, sizeof k, "\"%s\": ", key);
    out[0] = 0;
    const char* at = strstr(status, want);
    if (!at) return;
    const char* v = NULL;
    if (strcmp(key, "id") > 0) {
        v = strstr(at, k);
    } else {
        for (const char* p = status; (p = strstr(p, k)) && p < at; ++p) v = p;
    }
    if (!v) return;
    v += strlen(k);
    if (*v == '"') ++v;
    size_t n = strcspn(v, "\",\r\n}");
    if (n >= size) n = size - 1;
    memcpy(out, v, n);
    out[n] = 0;
}

#if defined(__aarch64__) || defined(_M_ARM64)
#define THIS_ARCHITECTURE "arm64"
#elif defined(__x86_64__) || defined(_M_X64)
#define THIS_ARCHITECTURE "x64"
#else
#define THIS_ARCHITECTURE "x86"
#endif

/* The test synth in each format, from this build and from others whose builds are there (their
 * bin folders: other architectures', or universal ones): one scan lists each one this computer
 * runs, with the architecture it runs as (this one's, when the file has it too), and none it does
 * not run. In one rack, each on an output of its own, every one listed sounds, like this build's
 * synth. With plugins in this process (those of this architecture), and in plugin hosts.
 * A folder given as "<folder>=<architecture>" or "<folder>=refused" must also turn out so
 * (tools/arch_matrix.sh gives each combination's), and must hold the test synth. "=no-host",
 * "=not-run" and "=no-binary" say why it must be refused: this build ships no plugin host for it,
 * this computer does not run that architecture, or the VST3 bundle holds no binary for this
 * computer (an ARM64EC one on an x64 PC). */
static int refusal(const char* expected) {
    return strcmp(expected, "refused") == 0 || strcmp(expected, "no-host") == 0 || strcmp(expected, "not-run") == 0 ||
           strcmp(expected, "no-binary") == 0;
}

static void mixed_architectures(char** synths, char** other_bins, int other_count) {
    enum { kFormats = 3, kMaxBuilds = 6, kMaxRack = kMaxBuilds * kFormats, kFrames = 19 * 256 };
    char paths[kMaxBuilds][kFormats][MAX_PATH];
    char expected[kMaxBuilds][16];
    char dirs[MAX_PATH * kMaxBuilds] = "";
    int builds = 0;
    for (int b = 0; b <= other_count && builds < kMaxBuilds; ++b) {
        char dir[MAX_PATH];
        if (b == 0) {
            folder_of(synths[0], dir, sizeof dir);
            strcpy(expected[builds], THIS_ARCHITECTURE);
        } else {
            char given[MAX_PATH];
            snprintf(given, sizeof given, "%s", other_bins[b - 1]);
            char* eq = strrchr(given, '=');
            expected[builds][0] = 0;
            if (eq && eq > file_name(given) - 1) {
                *eq = 0;
                snprintf(expected[builds], sizeof expected[builds], "%s", eq + 1);
            }
            absolute(given, dir, sizeof dir);
        }
        int any = 0;
        for (int f = 0; f < kFormats; ++f) {
            if (b == 0) {
                absolute(synths[f], paths[builds][f], MAX_PATH);
            } else if (strlen(dir) + 1 + strlen(file_name(synths[f])) < MAX_PATH) {
                strcpy(paths[builds][f], dir);
                strcat(paths[builds][f], "/");
                strcat(paths[builds][f], file_name(synths[f]));
            } else {
                paths[builds][f][0] = 0;
            }
            if (exists(paths[builds][f])) any = 1;
        }
        if (!any) {
            printf("mixed architectures: no build of the test synth in %s\n", dir);
            CHECK(expected[builds][0] == 0);
            continue;
        }
        if (builds) strcat(dirs, ";");
        strcat(dirs, dir);
        ++builds;
    }

    char cache[MAX_PATH];
    temp_file(cache, sizeof cache, sizeof(void*) == 8 ? "brack_test_dll_mixed-64.json" : "brack_test_dll_mixed-32.json");
    remove(cache);
    /* A buffer big enough at once, so that the first scan's text is the one read; the second
     * comes from the cache. */
    const size_t size = 1 << 20;
    char* scan = (char*)calloc(size, 1);
    char* again = (char*)calloc(size, 1);
    CHECK(brack_scan_plugins(dirs, cache, BRACK_SCAN_EXTRA_DIRS_ONLY, scan, size, NULL) == BRACK_OK);
    CHECK(brack_scan_plugins(dirs, cache, BRACK_SCAN_EXTRA_DIRS_ONLY, again, size, NULL) == BRACK_OK);
    CHECK(strcmp(scan, again) == 0);
    free(again);
    remove(cache);

    char own[16];
    listed(scan, paths[0][0], "architecture", own, sizeof own);
    CHECK(strcmp(own, THIS_ARCHITECTURE) == 0);
    int foreign = 0;

    for (int in_process = 0; in_process < 2; ++in_process) {
        const char* where = in_process ? "plugins in this process" : "plugin hosts";
        brack_engine* e = brack_engine_create();
        brack_config cfg;
        brack_config_init(&cfg);
        cfg.plugins_in_process = in_process;
        CHECK(brack_set_config(e, &cfg) == BRACK_OK);

        char ids[kMaxRack][16];
        int build_of[kMaxRack], format_of[kMaxRack], rack = 0;
        for (int b = 0; b < builds; ++b)
            for (int f = 0; f < kFormats; ++f) {
                const char* path = paths[b][f];
                if (!exists(path)) continue;
                char arch[16];
                listed(scan, path, "architecture", arch, sizeof arch);
                snprintf(ids[rack], sizeof ids[rack], "b%d-f%d", b, f);
                if (brack_add_plugin_ex(e, ids[rack], path, NULL, BRACK_ADD_PLUGIN_NO_AUDIO_ROUTES, NULL, 0) != BRACK_OK) {
                    const char* why = brack_last_error();
                    const int not_run = strstr(why, "does not run") != NULL, no_host = strstr(why, "no plugin host") != NULL;
                    const int no_binary = strstr(why, "binary in the bundle") != NULL;
                    printf("  %s, %s: not loaded: %s\n", path, where, why);
                    CHECK(b > 0 && arch[0] == 0 && (not_run || no_host || (no_binary && strcmp(expected[b], "no-binary") == 0)));
                    if (expected[b][0] && !refusal(expected[b]))
                        fprintf(stderr, "  expected to run as %s\n", expected[b]);
                    CHECK(!expected[b][0] || refusal(expected[b]));
                    if (strcmp(expected[b], "no-host") == 0) CHECK(no_host && !not_run);
                    if (strcmp(expected[b], "not-run") == 0) CHECK(not_run && !no_host);
                    if (strcmp(expected[b], "no-binary") == 0) CHECK(no_binary);
                    continue;
                }
                CHECK(brack_connect_audio(e, ids[rack], 0, 0, (uint32_t)rack, 1.0f) == BRACK_OK);
                if (strcmp(arch, own) != 0 && !in_process) ++foreign;
                build_of[rack] = b;
                format_of[rack] = f;
                ++rack;
            }

        char* status = status_of(e, 0);
        CHECK(status != NULL);
        if (!status) status = (char*)calloc(1, 1);
        for (int i = 0; i < rack; ++i) {
            const char* path = paths[build_of[i]][format_of[i]];
            char arch[16], ran[16], separate[8];
            listed(scan, path, "architecture", arch, sizeof arch);
            plugin_field(status, ids[i], "architecture", ran, sizeof ran);
            plugin_field(status, ids[i], "separateProcess", separate, sizeof separate);
            printf("  %s, %s: listed as %s, runs as %s, separate process %s\n", path, where,
                   arch[0] ? arch : "(not listed)", ran, separate);
            CHECK(arch[0] != 0 && strcmp(arch, ran) == 0);
            if (expected[build_of[i]][0] && strcmp(expected[build_of[i]], arch) != 0)
                fprintf(stderr, "  expected %s\n", refusal(expected[build_of[i]]) ? "to be refused" : expected[build_of[i]]);
            CHECK(!expected[build_of[i]][0] || strcmp(expected[build_of[i]], arch) == 0);
            CHECK(strcmp(separate, (in_process && strcmp(arch, own) == 0) ? "false" : "true") == 0);
        }
        free(status);
        if (!rack) {
            brack_engine_destroy(e);
            continue;
        }

        CHECK(brack_start_manual(e, 48000, (uint32_t)rack, 256) == BRACK_OK);
        const uint8_t on[] = {0x90, 0x3C, 0x64};
        for (int i = 0; i < rack; ++i) CHECK(brack_send_midi(e, ids[i], 0, on, 3) == BRACK_OK);
        float* sound = (float*)calloc((size_t)rack * kFrames, sizeof(float));
        float* outs[kMaxRack];
        for (int done = 0; done < kFrames; done += 256) {
            for (int i = 0; i < rack; ++i) outs[i] = sound + (size_t)i * kFrames + done;
            brack_render(e, outs, (uint32_t)rack, 256);
        }
        for (int i = 0; i < rack; ++i) {
            int ours = -1;
            for (int j = 0; j < rack; ++j)
                if (build_of[j] == 0 && format_of[j] == format_of[i]) ours = j;
            CHECK(ours >= 0);
            if (ours < 0) continue;
            float peak = 0, diff = 0;
            for (int s = 0; s < kFrames; ++s) {
                const float v = sound[(size_t)i * kFrames + s], d = v - sound[(size_t)ours * kFrames + s];
                if (fabsf(v) > peak) peak = fabsf(v);
                if (fabsf(d) > diff) diff = fabsf(d);
            }
            printf("  %s, %s: output %d, peak %.3f, at most %g from this build's\n", paths[build_of[i]][format_of[i]],
                   where, i + 1, peak, diff);
            CHECK(peak > 0.1f && diff < 1e-4f);
        }
        free(sound);
        brack_engine_destroy(e);
    }
    printf("mixed architectures: %d plugin(s) of another architecture in one rack with this %s build's\n", foreign, own);
    free(scan);
}

static int run(int argc, char** argv) {
    if (argc < 4) {
        fprintf(stderr, "usage: test_dll <brack-test-synth.clap> <vst2 synth> <vst3 synth> [<other architectures' bin folders>...]\n");
        return 2;
    }
    CHECK(brack_api_version() == BRACK_API_VERSION);
    brack_set_log_callback(on_log, NULL);

    /* The test synth logs what it receives here (read when its module loads, so set first). */
    char log[MAX_PATH];
    temp_file(log, sizeof log, sizeof(void*) == 8 ? "brack_test_dll-64.log" : "brack_test_dll-32.log");
    remove(log);
#ifdef _WIN32
    char env[MAX_PATH + 32];
    snprintf(env, sizeof env, "BRACK_TESTSYNTH_LOG=%s", log);
    _putenv(env);
#else
    setenv("BRACK_TESTSYNTH_LOG", log, 1);
#endif

    brack_engine* e = brack_engine_create();
    CHECK(e != NULL);

    char id[BRACK_ID_MAX];
    CHECK(brack_add_plugin(e, NULL, argv[1], NULL, id, sizeof id) == BRACK_OK);
    printf("plugin id: %s\n", id);
    CHECK(brack_add_plugin(e, id, argv[1], NULL, NULL, 0) != BRACK_OK); /* duplicate id */
    CHECK(brack_add_plugin(e, "x", "does-not-exist.clap", NULL, NULL, 0) != BRACK_OK);

    for (int pass = 0; pass < 2; ++pass) {
        /* pass 0: plugins at the output rate; pass 1: plugins at 96 kHz converted to 48 kHz */
        brack_config cfg;
        brack_config_init(&cfg);
        cfg.process_sample_rate = pass ? 96000 : 0;
        cfg.resampler_quality = BRACK_SRC_ULTRA;
        CHECK(brack_set_config(e, &cfg) == BRACK_OK);
        CHECK(brack_start_manual(e, 48000, 2, 256) == BRACK_OK);

        CHECK(render_peak(e, 4800) == 0.0f);
        const uint8_t on[3] = {0x90, 69, 127}, off[3] = {0x80, 69, 0};
        CHECK(brack_send_midi(e, id, 0, on, 3) == BRACK_OK);
        float peak = render_peak(e, 24000);
        printf("pass %d peak: %.3f\n", pass, peak);
        CHECK(peak > 0.2f);
        CHECK(brack_send_midi(e, id, 0, off, 3) == BRACK_OK);
        render_peak(e, 4800);
        CHECK(render_peak(e, 4800) < 1e-3f);
        brack_stop(e);
    }

    size_t needed = 0;
    CHECK(brack_get_status(e, NULL, 0, &needed) == BRACK_OK && needed > 1);
    char* status = (char*)malloc(needed);
    CHECK(brack_get_status(e, status, needed, NULL) == BRACK_OK);
    CHECK(strstr(status, "brack.test.synth") != NULL);
    char tiny[4];
    CHECK(brack_get_status(e, tiny, sizeof tiny, NULL) == BRACK_ERR_BUFFER_TOO_SMALL);
    free(status);

    CHECK(brack_set_plugin_name(e, id, "My synth") == BRACK_OK);
    CHECK(brack_set_plugin_name(e, "nope", "x") == BRACK_ERR_NOT_FOUND);
    needed = 0;
    brack_get_status(e, NULL, 0, &needed);
    status = (char*)malloc(needed);
    CHECK(brack_get_status(e, status, needed, NULL) == BRACK_OK && strstr(status, "\"My synth\"") != NULL);
    free(status);

    CHECK(brack_get_master_gain(e) == 1.0f);
    CHECK(brack_set_master_gain(e, 0.25f) == BRACK_OK && brack_get_master_gain(e) == 0.25f);
    CHECK(brack_set_master_gain(e, -1.0f) == BRACK_ERR_INVALID_ARGUMENT);

    CHECK(brack_remove_plugin(e, id) == BRACK_OK);
    CHECK(brack_remove_plugin(e, id) == BRACK_ERR_NOT_FOUND);
    brack_engine_destroy(e);

    events_and_changes(argv[1]);
    rack_edits(argv[1]);
    plugin_state(argv[1]);
    timed_midi(argv[1], log);
#ifdef _WIN32
    embedded_editor(argv[1]);
#endif
    cached_scan(argv[1]);
    error_codes(argv[1]);
    mixed_architectures(argv + 1, argv + 4, argc - 4);
    const int hangs = brack_virtual_midi_removal_hangs();
    printf("virtual port removal hangs the MIDI service: %d\n", hangs);
    CHECK(hangs == 0 || hangs == 1);

    brack_set_log_callback(NULL, NULL);
    remove(log);
    printf(failures ? "FAILED (%d)\n" : "PASS\n", failures);
    return failures ? 1 : 0;
}

#ifdef __APPLE__
/* macOS: the engine works on the main thread, whose run loop the application runs (brack.h). */
struct args {
    int argc;
    char** argv;
    int result;
};
static void* run_test(void* p) {
    struct args* a = (struct args*)p;
    a->result = run(a->argc, a->argv);
    CFRunLoopStop(CFRunLoopGetMain());
    return NULL;
}
int main(int argc, char** argv) {
    struct args a = {argc, argv, 0};
    pthread_t thread;
    pthread_create(&thread, NULL, run_test, &a);
    CFRunLoopRun();
    pthread_join(thread, NULL);
    return a.result;
}
#else
int main(int argc, char** argv) { return run(argc, argv); }
#endif
