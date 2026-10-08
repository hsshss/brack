#include <CoreMIDI/CoreMIDI.h>
#include <mach/mach_time.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "engine.h"
#include "midi/midi_input.h"
#include "util/main_thread.h"

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

struct Received {
    std::mutex mutex;
    std::vector<Bytes> messages;
    std::vector<int64_t> whens;
    MidiReceiveFn fn() {
        return [this](const uint8_t* d, size_t n, int64_t when) {
            std::lock_guard lock(mutex);
            messages.emplace_back(d, d + n);
            whens.push_back(when);
        };
    }
    size_t count() {
        std::lock_guard lock(mutex);
        return messages.size();
    }
};

const std::vector<Bytes> kSent = [] {
    Bytes sysex = {0xF0, 0x7D};
    for (int i = 0; i < 300; ++i) sysex.push_back((uint8_t)(i & 0x7F));
    sysex.push_back(0xF7);
    return std::vector<Bytes>{{0x90, 0x3C, 0x64}, {0xB0, 0x07, 0x20}, {0xC2, 0x05}, {0xF8},
                              {0xE0, 0x00, 0x40}, {0xF2, 0x10, 0x20}, sysex, {0x80, 0x3C, 0x00}};
}();

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
void sendBytes(MIDIEndpointRef source, MIDIPortRef out, MIDIEndpointRef destination, const std::vector<Bytes>& messages,
               MIDITimeStamp timeStamp = 0) {
    Bytes stream;
    for (auto& m : messages) stream.insert(stream.end(), m.begin(), m.end());
    std::vector<Byte> buffer(stream.size() + 1024);
    auto* list = reinterpret_cast<MIDIPacketList*>(buffer.data());
    MIDIPacket* packet = MIDIPacketListInit(list);
    packet = MIDIPacketListAdd(list, buffer.size(), packet, timeStamp, stream.size(), stream.data());
    CHECK(packet != nullptr);
    if (source) CHECK(MIDIReceived(source, list) == noErr);
    else CHECK(MIDISend(out, destination, list) == noErr);
}
#pragma clang diagnostic pop

MIDIClientRef testClient() {
    static MIDIClientRef c = [] {
        MIDIClientRef made = 0;
        CHECK(MIDIClientCreateWithBlock(CFSTR("brack test"), &made, nullptr) == noErr);
        return made;
    }();
    return c;
}

bool listed(const std::string& name) {
    auto names = listHardwareMidiInputs();
    return std::find(names.begin(), names.end(), name) != names.end();
}

void hardwareInput() {
    MIDIEndpointRef source = 0;
    CHECK(MIDISourceCreateWithProtocol(testClient(), CFSTR("brack test out"), kMIDIProtocol_1_0, &source) == noErr);
    CHECK(waitFor([] { return listed("brack test out"); }));
    Received received;
    std::string err;
    auto in = openHardwareMidiInput("brack test out", received.fn(), err);
    CHECK(in);
    if (!in) {
        std::printf("open: %s\n", err.c_str());
        return;
    }
    sendBytes(source, 0, 0, kSent);
    CHECK(waitFor([&] { return received.count() >= kSent.size(); }));
    {
        std::lock_guard lock(received.mutex);
        std::printf("source: %zu of %zu messages as sent\n", received.messages == kSent ? kSent.size() : 0, kSent.size());
        CHECK(received.messages == kSent);
    }
    CHECK(!in->lost());
    MIDIEndpointDispose(source);
    CHECK(waitFor([&] { return in->lost(); }));
    std::printf("source gone: %s\n", in->lost() ? "noticed" : "not noticed");
    CHECK(!listed("brack test out"));
    CHECK(!openHardwareMidiInput("brack test out", Received().fn(), err));
}

void virtualPort() {
    Received received;
    std::string err;
    auto in = createVirtualMidiInput("brack test virtual", received.fn(), err);
    CHECK(in);
    if (!in) {
        std::printf("virtual: %s\n", err.c_str());
        return;
    }
    CHECK(!listed("brack test virtual"));
    MIDIEndpointRef destination = 0;
    CHECK(waitFor([&] {
        for (ItemCount i = 0, n = MIDIGetNumberOfDestinations(); i < n; ++i) {
            CFStringRef name = nullptr;
            MIDIObjectGetStringProperty(MIDIGetDestination(i), kMIDIPropertyDisplayName, &name);
            if (name && CFStringCompare(name, CFSTR("brack test virtual"), 0) == kCFCompareEqualTo)
                destination = MIDIGetDestination(i);
            if (name) CFRelease(name);
        }
        return destination != 0;
    }));
    MIDIPortRef out = 0;
    CHECK(MIDIOutputPortCreate(testClient(), CFSTR("brack test sender"), &out) == noErr);
    sendBytes(0, out, destination, kSent);
    CHECK(waitFor([&] { return received.count() >= kSent.size(); }));
    std::lock_guard lock(received.mutex);
    std::printf("virtual port: %zu of %zu messages as sent\n", received.messages == kSent ? kSent.size() : 0, kSent.size());
    CHECK(received.messages == kSent);
}

int64_t steadyNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

// A packet's time stamp, host time, comes through on Brack's clock (steady_clock, which counts the
// time asleep that host time leaves out), one for now and one 50 ms ahead.
void timeStamps() {
    MIDIEndpointRef source = 0;
    CHECK(MIDISourceCreateWithProtocol(testClient(), CFSTR("brack test stamps"), kMIDIProtocol_1_0, &source) == noErr);
    CHECK(waitFor([] { return listed("brack test stamps"); }));
    Received received;
    std::string err;
    auto in = openHardwareMidiInput("brack test stamps", received.fn(), err);
    CHECK(in);
    mach_timebase_info_data_t timebase;
    mach_timebase_info(&timebase);
    const int64_t before = steadyNs();
    const MIDITimeStamp now = mach_absolute_time();
    const int64_t after = steadyNs();
    const MIDITimeStamp ahead = now + 50'000'000ull * timebase.denom / timebase.numer;
    sendBytes(source, 0, 0, {{0x90, 0x3C, 0x64}}, now);
    sendBytes(source, 0, 0, {{0x80, 0x3C, 0x00}}, ahead);
    CHECK(waitFor([&] { return received.count() >= 2; }));
    std::lock_guard lock(received.mutex);
    CHECK(received.whens.size() == 2);
    if (received.whens.size() == 2) {
        std::printf("time stamps: now %+lld ns, 50 ms ahead %+lld ns off\n", (long long)(received.whens[0] - before),
                    (long long)(received.whens[1] - before - 50'000'000));
        // Host time counts ticks (41.67 ns on Apple silicon); turning it into nanoseconds, here
        // and in steady_clock, rounds by up to one.
        const int64_t tick = (timebase.numer + timebase.denom - 1) / timebase.denom;
        const int64_t aheadNs = (int64_t)((ahead - now) * timebase.numer / timebase.denom);
        CHECK(received.whens[0] >= before - tick && received.whens[0] <= after + tick);
        CHECK(received.whens[1] >= before + aheadNs - tick && received.whens[1] <= after + aheadNs + tick);
    }
    MIDIEndpointDispose(source);
}

// Two sources of one name (two keyboards of a kind) are both listed, and each opens on its own.
void sameNames() {
    MIDIEndpointRef first = 0, second = 0;
    CHECK(MIDISourceCreateWithProtocol(testClient(), CFSTR("brack test twin"), kMIDIProtocol_1_0, &first) == noErr);
    CHECK(MIDISourceCreateWithProtocol(testClient(), CFSTR("brack test twin"), kMIDIProtocol_1_0, &second) == noErr);
    CHECK(waitFor([] { return listed("brack test twin") && listed("brack test twin (2)"); }));
    SInt32 firstId = 0, secondId = 0;
    MIDIObjectGetIntegerProperty(first, kMIDIPropertyUniqueID, &firstId);
    MIDIObjectGetIntegerProperty(second, kMIDIPropertyUniqueID, &secondId);
    const MIDIEndpointRef later = firstId < secondId ? second : first;  // "(2)": the larger unique id
    Received received;
    std::string err;
    auto in = openHardwareMidiInput("brack test twin (2)", received.fn(), err);
    CHECK(in);
    sendBytes(later == first ? second : first, 0, 0, {{0x90, 0x10, 0x10}});
    sendBytes(later, 0, 0, {{0x90, 0x20, 0x20}});
    CHECK(waitFor([&] { return received.count() >= 1; }));
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    std::lock_guard lock(received.mutex);
    std::printf("same names: \"(2)\" received %zu message(s), from %s\n", received.messages.size(),
                received.messages == std::vector<Bytes>{{0x90, 0x20, 0x20}} ? "its own source only" : "the wrong source");
    CHECK((received.messages == std::vector<Bytes>{{0x90, 0x20, 0x20}}));
    MIDIEndpointDispose(first);
    MIDIEndpointDispose(second);
}

// The engine notices its source going away, and opens it again once it is back.
void engineReconnects() {
    MIDIEndpointRef source = 0;
    CHECK(MIDISourceCreateWithProtocol(testClient(), CFSTR("brack test comeback"), kMIDIProtocol_1_0, &source) == noErr);
    CHECK(waitFor([] { return listed("brack test comeback"); }));
    Engine e;
    std::string err;
    CHECK(!e.addMidiSource({"in", MidiSourceKind::Hardware, "brack test comeback"}, err).empty());
    auto waitEvent = [&](EngineEvent::Type type) {
        bool seen = false;  // waitFor asks again once done: polling must not be what answers
        return waitFor([&] {
            for (EngineEvent ev; !seen && e.pollEvent(ev);) seen = ev.type == type && ev.id == "in";
            return seen;
        }, 5000);
    };
    MIDIEndpointDispose(source);
    const bool lost = waitEvent(EngineEvent::Type::MidiSourceLost);
    CHECK(MIDISourceCreateWithProtocol(testClient(), CFSTR("brack test comeback"), kMIDIProtocol_1_0, &source) == noErr);
    const bool back = waitEvent(EngineEvent::Type::MidiSourceReconnected);
    std::printf("engine: source gone %s, back %s\n", lost ? "noticed" : "NOT noticed", back ? "reconnected" : "NOT reconnected");
    CHECK(lost && back);
    MIDIEndpointDispose(source);
}

int run() {
    std::string reason;
    CHECK(virtualMidiAvailable(reason));
    hardwareInput();
    virtualPort();
    timeStamps();
    sameNames();
    engineReconnects();
    if (g_failures) {
        std::fprintf(stderr, "%d check(s) failed\n", g_failures);
        return 1;
    }
    std::puts("all checks passed");
    return 0;
}

}  // namespace

int main() { return runWithMainLoop(run); }
