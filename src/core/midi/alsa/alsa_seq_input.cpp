// Linux: MIDI input ports on the ALSA sequencer, which lists hardware ports and other programs'
// (virtual keyboards, sequencers, PipeWire's) alike. Each port Brack opens is a sequencer client
// of its own ("Brack") with one port, read on its own thread: a hardware input connects from its
// source, a virtual port waits for others to connect.
// Port listing and naming, and SysEx reassembly, follow Glosa's (src/Glosa.Midi.Linux).
// Each message carries when the sequencer delivered it (kernel-stamped in real time on a queue of
// this process's), on Brack's clock, not when the reading thread got to it.
// libasound is loaded when first needed (dlopen), not linked, so that Brack runs where it is not
// installed: without it there are no ports.
#include <alsa/asoundlib.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <map>
#include <mutex>
#include <thread>
#include <vector>

#include "midi/midi_input.h"
#include "util/common.h"

namespace brack {

namespace {

constexpr const char* kClientName = "Brack";
constexpr size_t kMaxSysex = 1 << 20;  // longer ones are dropped, as on Windows

#define BRACK_ALSA_FUNCTIONS(X)                                                                          \
    X(snd_strerror)                                                                                     \
    X(snd_seq_open) X(snd_seq_close) X(snd_seq_set_client_name) X(snd_seq_drain_output)                 \
    X(snd_seq_poll_descriptors_count) X(snd_seq_poll_descriptors) X(snd_seq_event_input)                \
    X(snd_seq_alloc_named_queue) X(snd_seq_free_queue) X(snd_seq_control_queue)                         \
    X(snd_seq_queue_timer_sizeof) X(snd_seq_get_queue_timer) X(snd_seq_set_queue_timer)                 \
    X(snd_seq_queue_timer_set_resolution)                                                               \
    X(snd_seq_queue_status_sizeof) X(snd_seq_get_queue_status) X(snd_seq_queue_status_get_real_time)    \
    X(snd_seq_client_info_sizeof) X(snd_seq_query_next_client) X(snd_seq_client_info_set_client)        \
    X(snd_seq_client_info_get_client) X(snd_seq_client_info_get_name) X(snd_seq_client_info_get_pid)    \
    X(snd_seq_port_info_sizeof) X(snd_seq_query_next_port) X(snd_seq_create_port)                       \
    X(snd_seq_port_info_set_client) X(snd_seq_port_info_set_port) X(snd_seq_port_info_set_name)         \
    X(snd_seq_port_info_set_capability) X(snd_seq_port_info_set_type)                                   \
    X(snd_seq_port_info_set_timestamping) X(snd_seq_port_info_set_timestamp_real)                       \
    X(snd_seq_port_info_set_timestamp_queue) X(snd_seq_port_info_get_port)                              \
    X(snd_seq_port_info_get_capability) X(snd_seq_port_info_get_type) X(snd_seq_port_info_get_addr)     \
    X(snd_seq_port_info_get_name) X(snd_seq_connect_from)                                               \
    X(snd_midi_event_new) X(snd_midi_event_free) X(snd_midi_event_no_status) X(snd_midi_event_decode)

struct Alsa {
#define BRACK_ALSA_MEMBER(name) decltype(&::name) name;
    BRACK_ALSA_FUNCTIONS(BRACK_ALSA_MEMBER)
#undef BRACK_ALSA_MEMBER
};

constexpr const char* kNoAlsa = "MIDI ports are not available without libasound.so.2";

// Null without libasound.so.2, or with one that lacks a function.
const Alsa* alsa() {
    static const Alsa* loaded = []() -> const Alsa* {
        void* lib = dlopen("libasound.so.2", RTLD_NOW | RTLD_LOCAL);  // kept to the end
        if (!lib) return nullptr;
        static Alsa a;
        bool ok = true;
#define BRACK_ALSA_LOAD(name)                                          \
    a.name = reinterpret_cast<decltype(a.name)>(dlsym(lib, #name)); \
    ok = ok && a.name;
        BRACK_ALSA_FUNCTIONS(BRACK_ALSA_LOAD)
#undef BRACK_ALSA_LOAD
        return ok ? &a : nullptr;
    }();
    return loaded;
}

// One of alsa-lib's opaque structs, zeroed: what its *_alloca() macro makes, on the heap.
struct FreeDeleter {
    void operator()(void* p) const { std::free(p); }
};
template <class T>
using Opaque = std::unique_ptr<T, FreeDeleter>;
template <class T>
Opaque<T> opaque(size_t (*sizeOf)()) {
    return Opaque<T>(static_cast<T*>(std::calloc(1, sizeOf())));
}

struct Source {
    snd_seq_addr_t address;
    std::string name;   // as listed
    std::string other;  // the name it is listed under when another port's name clashes, or stops
};

// For listing: the process's handle, opened on first use and kept.
std::mutex g_listMutex;
snd_seq_t* g_listSeq = nullptr;
std::string g_listError;

snd_seq_t* openSeq(int streams, int mode, std::string& error) {
    const Alsa* as = alsa();
    if (!as) {
        error = kNoAlsa;
        return nullptr;
    }
    snd_seq_t* seq = nullptr;
    if (const int r = as->snd_seq_open(&seq, "default", streams, mode); r < 0) {
        error = std::string("cannot open the ALSA sequencer (") + as->snd_strerror(r) + ")";
        return nullptr;
    }
    as->snd_seq_set_client_name(seq, kClientName);
    return seq;
}

snd_seq_t* listSeq(std::string& error) {
    if (!g_listSeq && g_listError.empty()) g_listSeq = openSeq(SND_SEQ_OPEN_DUPLEX, 0, g_listError);
    error = g_listError;
    return g_listSeq;
}

int64_t steadyNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

// The queue the ports' messages are stamped on: one for the process, on the listing handle,
// running while a port is open. Its clock is an hrtimer tick (kTicksPerSecond) plus CLOCK_MONOTONIC
// since the tick, but it does not keep to CLOCK_MONOTONIC: it was seen to step back a millisecond
// and to run 250 ms ahead in 15 minutes. So only the time since a message was stamped is taken
// from it.
struct StampClock {
    int queue = -1;
    int users = 0;
};
StampClock g_clock;  // under g_listMutex
constexpr unsigned kTicksPerSecond = 6250;  // the most the sequencer allows (seq_timer.c)

int64_t queueNs(snd_seq_t* seq, int queue, snd_seq_queue_status_t* status) {
    const Alsa* as = alsa();
    if (as->snd_seq_get_queue_status(seq, queue, status) < 0) return -1;
    const snd_seq_real_time_t* t = as->snd_seq_queue_status_get_real_time(status);
    return (int64_t)t->tv_sec * 1000000000 + t->tv_nsec;
}

// Caller holds g_listMutex.
bool acquireStampClock(std::string& error) {
    snd_seq_t* seq = listSeq(error);
    if (!seq) return false;
    const Alsa* as = alsa();
    if (g_clock.users > 0) {
        ++g_clock.users;
        return true;
    }
    const int queue = as->snd_seq_alloc_named_queue(seq, kClientName);
    if (queue < 0) {
        error = std::string("cannot make an ALSA sequencer queue (") + as->snd_strerror(queue) + ")";
        return false;
    }
    const auto timer = opaque<snd_seq_queue_timer_t>(as->snd_seq_queue_timer_sizeof);
    as->snd_seq_get_queue_timer(seq, queue, timer.get());
    as->snd_seq_queue_timer_set_resolution(timer.get(), kTicksPerSecond);
    as->snd_seq_set_queue_timer(seq, queue, timer.get());
    as->snd_seq_control_queue(seq, queue, SND_SEQ_EVENT_START, 0, nullptr);
    as->snd_seq_drain_output(seq);
    g_clock = {queue, 1};
    return true;
}

void releaseStampClock() {
    std::lock_guard lock(g_listMutex);
    if (--g_clock.users > 0) return;
    const Alsa* as = alsa();
    as->snd_seq_control_queue(g_listSeq, g_clock.queue, SND_SEQ_EVENT_STOP, 0, nullptr);
    as->snd_seq_drain_output(g_listSeq);
    as->snd_seq_free_queue(g_listSeq, g_clock.queue);
    g_clock = {};
}

std::string trimmed(const char* s) {
    std::string t = s ? s : "";
    while (!t.empty() && t.back() == ' ') t.pop_back();
    return t;
}

// Other clients' ports that send MIDI: a type that says MIDI (as RtMidi chooses, which leaves out
// the system's timer and sound servers' own ports), readable by subscribers, not hidden. Named by
// the port, and by its client too where that tells apart ports of one name: not by the client
// alone, which for an unnamed program is "Client-" and a number that changes with every start.
// Ports that still share a name (two interfaces of a kind) are "Name (2)" and on, in client order.
// Brack's own ports are left out. Caller holds g_listMutex.
std::vector<Source> sources(snd_seq_t* seq) {
    struct Port {
        snd_seq_addr_t address;
        std::string name, client;
    };
    const Alsa* as = alsa();
    std::vector<Port> ports;
    const auto clientInfo = opaque<snd_seq_client_info_t>(as->snd_seq_client_info_sizeof);
    const auto portInfo = opaque<snd_seq_port_info_t>(as->snd_seq_port_info_sizeof);
    snd_seq_client_info_t* client = clientInfo.get();
    snd_seq_port_info_t* port = portInfo.get();
    const unsigned midiTypes =
        SND_SEQ_PORT_TYPE_MIDI_GENERIC | SND_SEQ_PORT_TYPE_SYNTH | SND_SEQ_PORT_TYPE_APPLICATION;
    const unsigned readable = SND_SEQ_PORT_CAP_READ | SND_SEQ_PORT_CAP_SUBS_READ;
    as->snd_seq_client_info_set_client(client, -1);
    while (as->snd_seq_query_next_client(seq, client) >= 0) {
        const int number = as->snd_seq_client_info_get_client(client);
        const std::string clientName = trimmed(as->snd_seq_client_info_get_name(client));
        if (clientName == kClientName && as->snd_seq_client_info_get_pid(client) == getpid()) continue;
        as->snd_seq_port_info_set_client(port, number);
        as->snd_seq_port_info_set_port(port, -1);
        while (as->snd_seq_query_next_port(seq, port) >= 0) {
            const unsigned caps = as->snd_seq_port_info_get_capability(port);
            if ((caps & readable) != readable || (caps & SND_SEQ_PORT_CAP_NO_EXPORT)) continue;
            if (!(as->snd_seq_port_info_get_type(port) & midiTypes)) continue;
            const snd_seq_addr_t address = *as->snd_seq_port_info_get_addr(port);
            std::string name = trimmed(as->snd_seq_port_info_get_name(port));
            if (name.empty()) name = std::to_string(address.client) + ":" + std::to_string(address.port);
            ports.push_back({address, name, clientName});
        }
    }
    std::vector<Source> result;
    std::map<std::string, int> named;
    for (const Port& p : ports) {
        bool clash = false;
        for (const Port& o : ports) clash = clash || (o.name == p.name && o.client != p.client);
        const std::string withClient = p.name + " (" + p.client + ")";
        Source s{p.address, clash ? withClient : p.name, clash ? p.name : withClient};
        if (const int same = ++named[s.name]; same > 1) {
            const std::string suffix = " (" + std::to_string(same) + ")";
            s.name += suffix;
            s.other += suffix;
        }
        result.push_back(std::move(s));
    }
    return result;
}

class AlsaInput final : public MidiInputPort {
public:
    // A port named `name`; connected from `from`, or (null) open for others to connect to.
    static std::unique_ptr<AlsaInput> open(const std::string& name, const snd_seq_addr_t* from, MidiReceiveFn fn,
                                           std::string& error) {
        snd_seq_t* seq = openSeq(SND_SEQ_OPEN_INPUT, SND_SEQ_NONBLOCK, error);
        if (!seq) return nullptr;
        const Alsa* as = alsa();
        auto in = std::unique_ptr<AlsaInput>(new AlsaInput(*as, seq, std::move(fn)));
        {
            std::lock_guard lock(g_listMutex);
            if (!acquireStampClock(error)) return nullptr;
            in->queue_ = g_clock.queue;
        }
        // A hardware input's port only receives from its source: no one else is offered it.
        const unsigned caps = SND_SEQ_PORT_CAP_WRITE | SND_SEQ_PORT_CAP_SUBS_WRITE | (from ? SND_SEQ_PORT_CAP_NO_EXPORT : 0);
        const auto portInfo = opaque<snd_seq_port_info_t>(as->snd_seq_port_info_sizeof);
        snd_seq_port_info_t* info = portInfo.get();
        as->snd_seq_port_info_set_name(info, name.c_str());
        as->snd_seq_port_info_set_capability(info, caps);
        as->snd_seq_port_info_set_type(info, SND_SEQ_PORT_TYPE_MIDI_GENERIC | SND_SEQ_PORT_TYPE_APPLICATION);
        as->snd_seq_port_info_set_timestamping(info, 1);
        as->snd_seq_port_info_set_timestamp_real(info, 1);
        as->snd_seq_port_info_set_timestamp_queue(info, in->queue_);
        if (const int r = as->snd_seq_create_port(seq, info); r < 0) {
            error = std::string("cannot make an ALSA sequencer port (") + as->snd_strerror(r) + ")";
            return nullptr;
        }
        in->port_ = as->snd_seq_port_info_get_port(info);
        if (from) {
            in->from_ = *from;
            in->connected_ = true;
            if (const int r = as->snd_seq_connect_from(seq, in->port_, from->client, from->port); r < 0) {
                error = std::string("cannot connect to the MIDI input (") + as->snd_strerror(r) + ")";
                return nullptr;
            }
        }
        if (as->snd_midi_event_new(16, &in->decoder_) < 0 || pipe(in->wake_) != 0) {
            error = "cannot set up the MIDI input";
            return nullptr;
        }
        as->snd_midi_event_no_status(in->decoder_, 1);  // every message with its status byte
        for (int fd : in->wake_) fcntl(fd, F_SETFD, FD_CLOEXEC);
        in->thread_ = std::thread([p = in.get()] { p->run(); });
        return in;
    }

    ~AlsaInput() override {
        if (thread_.joinable()) {
            stop_ = true;
            const char c = 0;
            while (write(wake_[1], &c, 1) < 0 && errno == EINTR) {
            }
            thread_.join();
        }
        for (int fd : wake_)
            if (fd >= 0) close(fd);
        if (decoder_) as_.snd_midi_event_free(decoder_);
        as_.snd_seq_close(seq_);  // takes its port and connections with it
        if (queue_ >= 0) releaseStampClock();
    }

    bool lost() const override { return lost_.load(); }

private:
    AlsaInput(const Alsa& as, snd_seq_t* seq, MidiReceiveFn fn)
        : as_(as),
          seq_(seq),
          fn_(std::move(fn)),
          status_(opaque<snd_seq_queue_status_t>(as.snd_seq_queue_status_sizeof)) {}

    void run() {
        std::vector<pollfd> fds((size_t)std::max(as_.snd_seq_poll_descriptors_count(seq_, POLLIN), 1) + 1);
        const int n = as_.snd_seq_poll_descriptors(seq_, fds.data(), (unsigned)fds.size() - 1, POLLIN);
        fds[(size_t)n] = {wake_[0], POLLIN, 0};
        while (!stop_) {
            if (poll(fds.data(), (nfds_t)n + 1, -1) < 0 && errno != EINTR) break;
            for (;;) {
                snd_seq_event_t* ev = nullptr;
                const int r = as_.snd_seq_event_input(seq_, &ev);
                if (r == -ENOSPC) {  // the port's queue overflowed: events were lost
                    inSysex_ = false;  // a SysEx missing pieces is not joined to a later end
                    continue;
                }
                if (r < 0 || !ev) break;     // nothing more for now
                handle(*ev);
            }
        }
    }

    // When the sequencer delivered `ev`, on Brack's clock: as long before now as the queue has run
    // since it stamped `ev`. 0 when it is not stamped in real time.
    int64_t stampNs(const snd_seq_event_t& ev) const {
        if ((ev.flags & SND_SEQ_TIME_STAMP_MASK) != SND_SEQ_TIME_STAMP_REAL) return 0;
        const int64_t now = steadyNs(), queueNow = queueNs(seq_, queue_, status_.get());
        if (queueNow < 0) return 0;
        const int64_t stamped = (int64_t)ev.time.time.tv_sec * 1000000000 + ev.time.time.tv_nsec;
        return now - std::max<int64_t>(0, queueNow - stamped);
    }

    void handle(const snd_seq_event_t& ev) {
        switch (ev.type) {
            case SND_SEQ_EVENT_SYSEX:
                sysex(static_cast<const uint8_t*>(ev.data.ext.ptr), ev.data.ext.len, stampNs(ev));
                return;
            // The connection from the source gone: the source's port or program went away, or
            // someone disconnected it (aconnect -d).
            case SND_SEQ_EVENT_PORT_UNSUBSCRIBED:
                if (connected_ && ev.data.connect.sender.client == from_.client &&
                    ev.data.connect.sender.port == from_.port && ev.data.connect.dest.port == port_)
                    lost_ = true;
                return;
            default: {
                uint8_t bytes[16];
                // Negative for what is not MIDI: the announcements of ports and connections.
                const long size = as_.snd_midi_event_decode(decoder_, bytes, sizeof bytes, &ev);
                if (size > 0) fn_(bytes, (size_t)size, stampNs(ev));
                return;
            }
        }
    }

    // The sequencer delivers a SysEx in pieces (256 bytes from a hardware port): joined up to F7,
    // timed by its first piece.
    void sysex(const uint8_t* data, size_t size, int64_t whenNs) {
        for (size_t i = 0; i < size; ++i) {
            const uint8_t b = data[i];
            if (b == 0xF0) {
                inSysex_ = true;  // one not finished is dropped, as on Windows
                sysex_.clear();
                sysexWhenNs_ = whenNs;
            }
            if (!inSysex_) continue;
            if (sysex_.size() == kMaxSysex) {
                inSysex_ = false;
                continue;
            }
            sysex_.push_back(b);
            if (b == 0xF7) {
                inSysex_ = false;
                fn_(sysex_.data(), sysex_.size(), sysexWhenNs_);
            }
        }
    }

    const Alsa& as_;
    snd_seq_t* seq_;
    MidiReceiveFn fn_;
    Opaque<snd_seq_queue_status_t> status_;  // the reading thread's
    snd_midi_event_t* decoder_ = nullptr;
    int port_ = -1;
    snd_seq_addr_t from_{};
    bool connected_ = false;
    int wake_[2] = {-1, -1};
    std::atomic<bool> stop_{false};
    std::atomic<bool> lost_{false};
    std::thread thread_;
    int queue_ = -1;  // g_clock's, which this port holds
    std::vector<uint8_t> sysex_;  // the reading thread's
    bool inSysex_ = false;
    int64_t sysexWhenNs_ = 0;
};

}  // namespace

std::vector<std::string> listHardwareMidiInputs() {
    std::lock_guard lock(g_listMutex);
    std::string error;
    snd_seq_t* seq = listSeq(error);
    std::vector<std::string> names;
    if (seq)
        for (const Source& s : sources(seq)) names.push_back(s.name);
    return names;
}

std::unique_ptr<MidiInputPort> openHardwareMidiInput(const std::string& name, MidiReceiveFn fn, std::string& error) {
    snd_seq_addr_t from{};
    bool found = false;
    {
        std::lock_guard lock(g_listMutex);
        snd_seq_t* seq = listSeq(error);
        if (!seq) return nullptr;
        // A name stored when another port's name clashed with it, or no longer does, still finds it.
        const std::vector<Source> all = sources(seq);
        for (const Source& s : all)
            if (!found && s.name == name) from = s.address, found = true;
        for (const Source& s : all)
            if (!found && s.other == name) from = s.address, found = true;
    }
    if (!found) {
        error = "MIDI input not found: " + name;
        return nullptr;
    }
    return AlsaInput::open("Brack in: " + name, &from, std::move(fn), error);
}

bool virtualMidiAvailable(std::string& reason) {
    std::lock_guard lock(g_listMutex);
    return listSeq(reason) != nullptr;
}

bool virtualMidiRemovalHangsService() { return false; }

std::unique_ptr<MidiInputPort> createVirtualMidiInput(const std::string& name, MidiReceiveFn fn, std::string& error) {
    return AlsaInput::open(name, nullptr, std::move(fn), error);
}

}  // namespace brack
