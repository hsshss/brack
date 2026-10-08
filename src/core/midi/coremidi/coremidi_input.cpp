// macOS: MIDI input ports on CoreMIDI. Sources (hardware, the IAC driver, other programs' virtual
// sources) are opened by their display name; a virtual port is a destination others send to.
// CoreMIDI hands both over as Universal MIDI Packets of the MIDI 1.0 protocol, which say every
// MIDI 1.0 message whole: they go back to the bytes Brack passes on, a SysEx gathered from its
// packets. The client is made on a thread of its own whose run loop gets CoreMIDI's notifications
// (a source going away), whatever the application does with its main thread.
#include <CoreFoundation/CoreFoundation.h>
#include <CoreMIDI/CoreMIDI.h>
#include <mach/mach_time.h>

#include <algorithm>
#include <atomic>
#include <future>
#include <map>
#include <mutex>
#include <thread>
#include <vector>

#include "midi/midi_input.h"

namespace brack {

namespace {

constexpr size_t kMaxSysex = 1 << 20;

std::string cfString(CFStringRef s) {
    if (!s) return {};
    char buf[1024];
    std::string out = CFStringGetCString(s, buf, sizeof buf, kCFStringEncodingUTF8) ? buf : "";
    CFRelease(s);
    return out;
}

// A CoreMIDI error by its MIDIServices.h name where it has one, and its number.
std::string describe(OSStatus status) {
    static const char* const kNames[] = {
        "kMIDIInvalidClient",    "kMIDIInvalidPort",     "kMIDIWrongEndpointType", "kMIDINoConnection",
        "kMIDIUnknownEndpoint",  "kMIDIUnknownProperty", "kMIDIWrongPropertyType", "kMIDINoCurrentSetup",
        "kMIDIMessageSendErr",   "kMIDIServerStartErr",  "kMIDISetupFormatErr",    "kMIDIWrongThread",
        "kMIDIObjectNotFound",   "kMIDIIDNotUnique",     "kMIDINotPermitted",      "kMIDIUnknownError",
    };
    const std::string number = std::to_string(status);
    if (status <= -10830 && status > -10830 - (OSStatus)std::size(kNames))
        return std::string(kNames[-10830 - status]) + ", " + number;
    return number;
}

CFStringRef toCf(const std::string& s) { return CFStringCreateWithCString(nullptr, s.c_str(), kCFStringEncodingUTF8); }

std::string displayName(MIDIObjectRef o) {
    CFStringRef name = nullptr;
    MIDIObjectGetStringProperty(o, kMIDIPropertyDisplayName, &name);
    return cfString(name);
}

// A packet's time stamp, host time, on Brack's clock: steady_clock, which counts the time asleep
// that host time leaves out. 0 (no time stamp) stays 0.
int64_t clockNs(MIDITimeStamp host) {
    if (!host) return 0;
    static const mach_timebase_info_data_t timebase = [] {
        mach_timebase_info_data_t t;
        mach_timebase_info(&t);
        return t;
    }();
    const int64_t asleep = (int64_t)(mach_continuous_time() - mach_absolute_time());
    return (int64_t)(((__int128)host + asleep) * timebase.numer / timebase.denom);
}

class UmpToBytes {
public:
    explicit UmpToBytes(MidiReceiveFn fn) : fn_(std::move(fn)) {}

    void receive(const MIDIEventList* list) {
        const MIDIEventPacket* packet = &list->packet[0];
        for (UInt32 i = 0; i < list->numPackets; ++i) {
            const int64_t when = clockNs(packet->timeStamp);
            for (UInt32 at = 0; at < packet->wordCount;) {
                const UInt32 type = packet->words[at] >> 28;
                const UInt32 size = kWords[type];
                if (at + size > packet->wordCount) break;
                message(type, &packet->words[at], when);
                at += size;
            }
            packet = MIDIEventPacketNext(packet);
        }
    }

private:
    static constexpr UInt32 kWords[16] = {1, 1, 1, 2, 2, 4, 1, 1, 2, 2, 2, 3, 3, 4, 4, 4};

    // A SysEx is timed by the packet it starts in.
    void message(UInt32 type, const UInt32* w, int64_t when) {
        const uint8_t status = (uint8_t)(w[0] >> 16), d1 = (uint8_t)(w[0] >> 8) & 0x7F, d2 = (uint8_t)w[0] & 0x7F;
        if (type == 0x1) {
            const uint8_t bytes[3] = {status, d1, d2};
            const size_t n = status == 0xF2 ? 3 : (status == 0xF1 || status == 0xF3) ? 2 : 1;
            fn_(bytes, n, when);
        } else if (type == 0x2) {
            const uint8_t bytes[3] = {status, d1, d2};
            const uint8_t kind = status & 0xF0;
            fn_(bytes, kind == 0xC0 || kind == 0xD0 ? 2 : 3, when);
        } else if (type == 0x3) {
            const UInt32 part = (w[0] >> 20) & 0xF, n = std::min<UInt32>((w[0] >> 16) & 0xF, 6);
            const uint8_t data[6] = {(uint8_t)(w[0] >> 8), (uint8_t)w[0], (uint8_t)(w[1] >> 24),
                                     (uint8_t)(w[1] >> 16), (uint8_t)(w[1] >> 8), (uint8_t)w[1]};
            if (part == 0 || part == 1) {
                sysex_.assign(1, 0xF0);
                sysexWhen_ = when;
            } else if (sysex_.empty()) {
                return;
            }
            if (sysex_.size() + n > kMaxSysex) {
                sysex_.clear();
                return;
            }
            sysex_.insert(sysex_.end(), data, data + n);
            if (part == 0 || part == 3) {
                sysex_.push_back(0xF7);
                fn_(sysex_.data(), sysex_.size(), sysexWhen_);
                sysex_.clear();
            }
        }
    }

    MidiReceiveFn fn_;
    std::vector<uint8_t> sysex_;
    int64_t sysexWhen_ = 0;
};

class CoreMidiInput;

struct Client {
    MIDIClientRef ref = 0;
    OSStatus status = 0;
    std::mutex mutex;
    std::vector<CoreMidiInput*> inputs;
};

Client& client();

class CoreMidiInput final : public MidiInputPort {
public:
    CoreMidiInput(MidiReceiveFn fn, MIDIEndpointRef source)
        : receiver_(std::make_shared<Receiver>(std::move(fn))), source_(source) {
        if (source_) MIDIObjectGetIntegerProperty(source_, kMIDIPropertyUniqueID, &sourceId_);
    }

    ~CoreMidiInput() override {
        {
            std::lock_guard lock(client().mutex);
            std::erase(client().inputs, this);
        }
        {
            std::lock_guard lock(receiver_->mutex);
            receiver_->open = false;
        }
        if (port_) MIDIPortDispose(port_);
        if (destination_) MIDIEndpointDispose(destination_);
    }

    bool open(const std::string& name, std::string& error) {
        Client& c = client();
        if (!c.ref) {
            error = "cannot open CoreMIDI (" + describe(c.status) + ")";
            return false;
        }
        CFStringRef cfName = toCf(name);
        std::shared_ptr<Receiver> receiver = receiver_;
        auto receive = ^(const MIDIEventList* list, void*) {
            std::lock_guard lock(receiver->mutex);
            if (receiver->open) receiver->decoder.receive(list);
        };
        OSStatus r = source_ ? MIDIInputPortCreateWithProtocol(c.ref, cfName, kMIDIProtocol_1_0, &port_, receive)
                             : MIDIDestinationCreateWithProtocol(c.ref, cfName, kMIDIProtocol_1_0, &destination_, receive);
        CFRelease(cfName);
        if (r == noErr && source_) r = MIDIPortConnectSource(port_, source_, nullptr);
        if (r != noErr) {
            error = std::string(source_ ? "cannot connect to the MIDI source" : "cannot create the virtual MIDI port") +
                    " (" + describe(r) + ")";
            return false;
        }
        std::lock_guard lock(c.mutex);
        c.inputs.push_back(this);
        return true;
    }

    bool lost() const override { return lost_.load(); }

    void checkSource() {
        if (!source_) return;
        MIDIObjectRef found = 0;
        MIDIObjectType type{};
        if (MIDIObjectFindByUniqueID(sourceId_, &found, &type) != noErr) lost_ = true;
    }

private:
    // The receiving block's, which CoreMIDI may still run while the port is disposed of: nothing
    // reaches `fn` once this port is gone.
    struct Receiver {
        explicit Receiver(MidiReceiveFn fn) : decoder(std::move(fn)) {}
        std::mutex mutex;
        bool open = true;
        UmpToBytes decoder;
    };

    std::shared_ptr<Receiver> receiver_;
    MIDIEndpointRef source_ = 0;
    SInt32 sourceId_ = 0;
    MIDIPortRef port_ = 0;
    MIDIEndpointRef destination_ = 0;
    std::atomic<bool> lost_{false};
};

Client& client() {
    static Client* c = [] {
        auto* made = new Client;
        std::promise<void> ready;
        std::thread([made, &ready] {
            made->status = MIDIClientCreateWithBlock(CFSTR("Brack"), &made->ref, ^(const MIDINotification* n) {
                if (n->messageID != kMIDIMsgObjectRemoved && n->messageID != kMIDIMsgSetupChanged) return;
                std::lock_guard lock(made->mutex);
                for (CoreMidiInput* in : made->inputs) in->checkSource();
            });
            ready.set_value();
            if (made->status == noErr) CFRunLoopRun();
        }).detach();
        ready.get_future().wait();
        return made;
    }();
    return *c;
}

}  // namespace

namespace {
// The sources by the names Brack lists them under: their display names, and where several share
// one (two keyboards of a kind), "Name (2)" and on for the others, in the order of their unique ids,
// which CoreMIDI keeps for a device across sessions.
std::vector<std::pair<std::string, MIDIEndpointRef>> namedSources() {
    client();  // sources show up in the listing only once the process has a client
    struct Source {
        std::string name;
        SInt32 id;
        MIDIEndpointRef ref;
    };
    std::vector<Source> all;
    for (ItemCount i = 0, n = MIDIGetNumberOfSources(); i < n; ++i) {
        const MIDIEndpointRef ref = MIDIGetSource(i);
        SInt32 id = 0;
        MIDIObjectGetIntegerProperty(ref, kMIDIPropertyUniqueID, &id);
        if (std::string name = displayName(ref); !name.empty()) all.push_back({std::move(name), id, ref});
    }
    std::stable_sort(all.begin(), all.end(), [](const Source& a, const Source& b) { return a.id < b.id; });
    std::map<std::string, int> seen;
    std::vector<std::pair<std::string, MIDIEndpointRef>> named;
    for (const Source& src : all) {
        const int n = ++seen[src.name];
        named.emplace_back(n == 1 ? src.name : src.name + " (" + std::to_string(n) + ")", src.ref);
    }
    return named;
}
}  // namespace

std::vector<std::string> listHardwareMidiInputs() {
    std::vector<std::string> names;
    for (auto& [name, ref] : namedSources()) names.push_back(name);
    return names;
}

std::unique_ptr<MidiInputPort> openHardwareMidiInput(const std::string& name, MidiReceiveFn fn, std::string& error) {
    MIDIEndpointRef source = 0;
    for (auto& [listed, ref] : namedSources())
        if (listed == name) source = ref;
    if (!source) {
        error = "no MIDI input named " + name;
        return nullptr;
    }
    auto in = std::make_unique<CoreMidiInput>(std::move(fn), source);
    if (!in->open("Brack", error)) return nullptr;
    return in;
}

bool virtualMidiAvailable(std::string& reason) {
    if (client().ref) return true;
    reason = "cannot open CoreMIDI (" + describe(client().status) + ")";
    return false;
}

bool virtualMidiRemovalHangsService() { return false; }

std::unique_ptr<MidiInputPort> createVirtualMidiInput(const std::string& name, MidiReceiveFn fn, std::string& error) {
    auto in = std::make_unique<CoreMidiInput>(std::move(fn), 0);
    if (!in->open(name, error)) return nullptr;
    return in;
}

}  // namespace brack
