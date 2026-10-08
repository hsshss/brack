// Linux: MIDI input ports on the ALSA sequencer (midi/alsa/). A client of this test's sends to
// them as another program would: listed under its port's name (with the client's where two
// ports' names clash), messages arrive as sent, a SysEx sent in pieces arrives whole, a virtual
// port receives from whoever sends to it, an input notices its source going away, and each message
// comes with when the sequencer delivered it, on Brack's clock. Not tried where there is no
// sequencer.
#include <alsa/asoundlib.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "midi/midi_input.h"

using namespace brack;

namespace {

int g_failures = 0;
#define CHECK(cond)                                                                \
    do {                                                                           \
        if (!(cond)) {                                                             \
            std::fprintf(stderr, "CHECK failed: %s (line %d)\n", #cond, __LINE__); \
            ++g_failures;                                                          \
        }                                                                          \
    } while (0)

using Bytes = std::vector<uint8_t>;

template <typename F>
bool waitFor(F done, int ms = 2000) {
    for (int i = 0; i < ms / 5 && !done(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    return done();
}

int64_t steadyNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

// What a port received, from its reading thread, each with when it was delivered.
struct Received {
    std::mutex mutex;
    std::vector<Bytes> messages;
    std::vector<int64_t> whens;
    MidiReceiveFn fn() {
        return [this](const uint8_t* d, size_t n, int64_t whenNs) {
            std::lock_guard lock(mutex);
            messages.emplace_back(d, d + n);
            whens.push_back(whenNs);
        };
    }
    size_t count() {
        std::lock_guard lock(mutex);
        return messages.size();
    }
};

// Another program: a client with one port that sends.
struct Sender {
    snd_seq_t* seq = nullptr;
    int port = -1;
    snd_midi_event_t* encoder = nullptr;

    Sender(const char* client, const char* portName) {
        if (snd_seq_open(&seq, "default", SND_SEQ_OPEN_DUPLEX, 0) < 0) return;
        snd_seq_set_client_name(seq, client);
        port = snd_seq_create_simple_port(seq, portName, SND_SEQ_PORT_CAP_READ | SND_SEQ_PORT_CAP_SUBS_READ,
                                          SND_SEQ_PORT_TYPE_MIDI_GENERIC | SND_SEQ_PORT_TYPE_APPLICATION);
        snd_midi_event_new(16, &encoder);
    }
    ~Sender() {
        if (encoder) snd_midi_event_free(encoder);
        if (seq) snd_seq_close(seq);
    }

    // To its subscribers, or to `dest`. A SysEx, or (`piece`) a piece of one.
    void send(const Bytes& message, const snd_seq_addr_t* dest = nullptr, bool piece = false) {
        snd_seq_event_t ev;
        snd_seq_ev_clear(&ev);
        if (piece || message[0] == 0xF0) {
            snd_seq_ev_set_sysex(&ev, (unsigned)message.size(), const_cast<uint8_t*>(message.data()));
        } else {
            snd_midi_event_reset_encode(encoder);
            snd_midi_event_encode(encoder, message.data(), (long)message.size(), &ev);
        }
        snd_seq_ev_set_source(&ev, port);
        if (dest) snd_seq_ev_set_dest(&ev, dest->client, dest->port);
        else snd_seq_ev_set_subs(&ev);
        snd_seq_ev_set_direct(&ev);
        snd_seq_event_output_direct(seq, &ev);
    }

    // The port named `name` of the client named `client`.
    bool find(const char* client, const char* name, snd_seq_addr_t& out) {
        snd_seq_client_info_t* c;
        snd_seq_port_info_t* p;
        snd_seq_client_info_alloca(&c);
        snd_seq_port_info_alloca(&p);
        snd_seq_client_info_set_client(c, -1);
        while (snd_seq_query_next_client(seq, c) >= 0) {
            if (std::string(snd_seq_client_info_get_name(c)) != client) continue;
            snd_seq_port_info_set_client(p, snd_seq_client_info_get_client(c));
            snd_seq_port_info_set_port(p, -1);
            while (snd_seq_query_next_port(seq, p) >= 0)
                if (std::string(snd_seq_port_info_get_name(p)) == name) {
                    out = *snd_seq_port_info_get_addr(p);
                    return true;
                }
        }
        return false;
    }
};

bool listed(const std::string& name) {
    const auto names = listHardwareMidiInputs();
    return std::find(names.begin(), names.end(), name) != names.end();
}

void hardwareInput() {
    Sender sender("brack test source", "brack test out");
    CHECK(sender.port >= 0);
    CHECK(listed("brack test out"));
    Received received;
    std::string err;
    auto in = openHardwareMidiInput("brack test out", received.fn(), err);
    CHECK(in);
    if (!in) {
        std::fprintf(stderr, "open: %s\n", err.c_str());
        return;
    }
    Bytes sysex(1007);
    sysex.front() = 0xF0;
    sysex.back() = 0xF7;
    for (size_t i = 1; i + 1 < sysex.size(); ++i) sysex[i] = (uint8_t)(i & 0x7F);
    const std::vector<Bytes> sent = {{0x90, 0x3C, 0x64}, {0xB0, 0x07, 0x64}, {0xC0, 0x05}, {0xE0, 0x00, 0x40},
                                     {0x80, 0x3C, 0x00}, sysex, {0xF8}};
    for (const Bytes& m : sent) {
        if (m == sysex) {
            // In three pieces, as a hardware port delivers a long one.
            sender.send(Bytes(sysex.begin(), sysex.begin() + 256), nullptr, true);
            sender.send(Bytes(sysex.begin() + 256, sysex.begin() + 512), nullptr, true);
            sender.send(Bytes(sysex.begin() + 512, sysex.end()), nullptr, true);
        } else {
            sender.send(m);
        }
    }
    CHECK(waitFor([&] { return received.count() >= sent.size(); }));
    {
        std::lock_guard lock(received.mutex);
        CHECK(received.messages == sent);
        std::printf("received %zu messages, a SysEx of %zu bytes among them\n", received.messages.size(),
                    received.messages.size() > 5 ? received.messages[5].size() : 0);
    }
    CHECK(!in->lost());

    // The source going away, and coming back.
    snd_seq_delete_simple_port(sender.seq, sender.port);
    CHECK(waitFor([&] { return in->lost(); }));
    CHECK(!listed("brack test out"));
    sender.port = snd_seq_create_simple_port(sender.seq, "brack test out", SND_SEQ_PORT_CAP_READ | SND_SEQ_PORT_CAP_SUBS_READ,
                                             SND_SEQ_PORT_TYPE_MIDI_GENERIC | SND_SEQ_PORT_TYPE_APPLICATION);
    CHECK(listed("brack test out"));

    // The source's program ending.
    in = openHardwareMidiInput("brack test out", received.fn(), err);
    CHECK(in && !in->lost());
    snd_seq_close(sender.seq);
    sender.seq = nullptr;
    CHECK(in && waitFor([&] { return in->lost(); }));
}

// Two programs' ports of one name are told apart by their clients' names; the plain name still opens one.
void clashingNames() {
    Sender a("brack test source", "brack test out"), b("brack test other", "brack test out");
    CHECK(!listed("brack test out"));
    CHECK(listed("brack test out (brack test source)"));
    CHECK(listed("brack test out (brack test other)"));
    Received received;
    std::string err;
    auto in = openHardwareMidiInput("brack test out (brack test other)", received.fn(), err);
    CHECK(in);
    b.send({0x91, 0x40, 0x7F});
    a.send({0x92, 0x41, 0x7F});
    CHECK(waitFor([&] { return received.count() >= 1; }));
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    {
        std::lock_guard lock(received.mutex);
        CHECK((received.messages == std::vector<Bytes>{{0x91, 0x40, 0x7F}}));
    }
    CHECK(openHardwareMidiInput("brack test out", Received().fn(), err));
}

void virtualPort() {
    Received received;
    std::string err;
    auto in = createVirtualMidiInput("brack test virtual", received.fn(), err);
    CHECK(in);
    CHECK(!listed("brack test virtual"));  // Brack's own ports are not offered as inputs
    Sender sender("brack test source", "brack test out");
    snd_seq_addr_t dest{};
    CHECK(sender.find("Brack", "brack test virtual", dest));
    sender.send({0x93, 0x42, 0x10}, &dest);
    CHECK(waitFor([&] { return received.count() >= 1; }));
    std::lock_guard lock(received.mutex);
    CHECK((received.messages == std::vector<Bytes>{{0x93, 0x42, 0x10}}));
}

// Each message comes with when the sequencer delivered it, on Brack's clock (steady_clock): within
// a tick or two of when it was sent, early on and 3 seconds later alike, however late the reading
// thread reads it, for a hardware input and a virtual port, and a SysEx by its first piece.
void deliveryTimes() {
    Sender sender("brack test source", "brack test out");
    Received received;
    std::string err;
    auto in = openHardwareMidiInput("brack test out", received.fn(), err);
    CHECK(in);
    if (!in) return;
    std::vector<int64_t> sent;
    for (int i = 0; i < 31; ++i) {
        if (i == 30) std::this_thread::sleep_for(std::chrono::seconds(3));
        sent.push_back(steadyNs());
        sender.send({0x90, (uint8_t)i, 0x40});
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    CHECK(waitFor([&] { return received.count() >= sent.size(); }));
    std::vector<int64_t> late;
    {
        std::lock_guard lock(received.mutex);
        for (size_t i = 0; i < received.whens.size() && i < sent.size(); ++i) late.push_back(received.whens[i] - sent[i]);
    }
    CHECK(late.size() == sent.size());
    const auto [least, most] = std::minmax_element(late.begin(), late.end());
    std::printf("delivered after sending: %.3f to %.3f ms (%.3f ms 3 s later)\n", (double)*least / 1e6,
                (double)*most / 1e6, late.empty() ? 0.0 : (double)late.back() / 1e6);
    constexpr int64_t kTolerance = 2'000'000;  // a tick of the queue's timer comes late by less
    for (int64_t l : late) CHECK(l > -kTolerance && l < kTolerance);

    // When it was delivered, not when it was read: a port whose reading thread the first message
    // holds up for 50 ms, the second sent 10 ms after it.
    Received held;
    auto slow = openHardwareMidiInput("brack test out", [&](const uint8_t* d, size_t n, int64_t whenNs) {
        held.fn()(d, n, whenNs);
        if (held.count() == 1) std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }, err);
    CHECK(slow);
    sender.send({0x90, 0x01, 0x40});
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    const int64_t second = steadyNs();
    sender.send({0x90, 0x02, 0x40});
    CHECK(waitFor([&] { return held.count() >= 2; }));
    if (held.count() >= 2) {
        std::lock_guard lock(held.mutex);
        std::printf("read 40 ms late, delivered after sending: %.3f ms\n", (double)(held.whens[1] - second) / 1e6);
        CHECK(held.whens[1] - second > -kTolerance && held.whens[1] - second < kTolerance);
    }

    Received onVirtual;
    auto port = createVirtualMidiInput("brack test virtual", onVirtual.fn(), err);
    CHECK(port);
    snd_seq_addr_t dest{};
    CHECK(sender.find("Brack", "brack test virtual", dest));
    const int64_t before = steadyNs();
    sender.send({0x90, 0x40, 0x40}, &dest);
    sender.send({0xF0, 0x7D, 0x01}, &dest, true);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    const int64_t between = steadyNs();
    sender.send({0x02, 0xF7}, &dest, true);
    CHECK(waitFor([&] { return onVirtual.count() >= 2; }));
    std::lock_guard lock(onVirtual.mutex);
    CHECK(onVirtual.whens.size() == 2);
    if (onVirtual.whens.size() == 2) {
        CHECK(onVirtual.whens[0] > before - kTolerance && onVirtual.whens[0] < between);
        CHECK(onVirtual.whens[1] > before - kTolerance && onVirtual.whens[1] < between);  // the SysEx's first piece
    }
}

}  // namespace

int main() {
    std::string reason;
    if (!virtualMidiAvailable(reason)) {
        std::printf("no ALSA sequencer, not tried: %s\n", reason.c_str());
        return 0;
    }
    hardwareInput();
    clashingNames();
    virtualPort();
    deliveryTimes();
    if (g_failures) {
        std::fprintf(stderr, "%d check(s) failed\n", g_failures);
        return 1;
    }
    std::puts("all checks passed");
    return 0;
}
