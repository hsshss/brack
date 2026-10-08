#pragma once
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
namespace brack::win {
// One call = one complete MIDI 1.0 byte-stream message (status byte included; SysEx delivered as complete F0..F7).
// Called on a service/driver thread. Must not be called after the port object is destroyed.
// whenNs: when it arrived on Brack's clock, or 0 for now (brack::MidiReceiveFn).
using MidiReceiveFn = std::function<void(const uint8_t* data, size_t size, int64_t whenNs)>;
class MidiSrvVirtualPort {
public:
    // name: UTF-8, shown to other apps (WinMM output port name should equal it). Returns nullptr and fills error on failure.
    static std::unique_ptr<MidiSrvVirtualPort> create(const std::string& name, MidiReceiveFn onMessage, std::string& error);
    static bool isAvailable(std::string& reason);   // service present & virtual transport enabled; cheap-ish
    // True on Windows MIDI Services builds without the late-2026 fix for microsoft/MIDI#1047:
    // removing a virtual device (including at process exit) deadlocks MidiSrv until reboot.
    static bool removalHangsService();
    virtual ~MidiSrvVirtualPort() = default;        // destroying must cleanly tear down the device, connection and session
    virtual const std::string& name() const = 0;
};
}
