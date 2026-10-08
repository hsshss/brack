#pragma once
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace brack {

// One call = one complete MIDI 1.0 message exactly as received (status byte
// included, SysEx as one F0..F7 block), with when the driver says it was received on
// Brack's clock (steady_clock nanoseconds), or 0 for when it is passed on. Called on a
// driver thread.
using MidiReceiveFn = std::function<void(const uint8_t* data, size_t size, int64_t whenNs)>;

class MidiInputPort {
public:
    virtual ~MidiInputPort() = default;
    // The driver closed the connection (device gone, service dropped it): nothing more
    // will arrive. Any thread.
    virtual bool lost() const { return false; }
};

// Hardware (or other applications') MIDI input ports.
std::vector<std::string> listHardwareMidiInputs();
std::unique_ptr<MidiInputPort> openHardwareMidiInput(const std::string& name, MidiReceiveFn fn, std::string& error);

// A virtual MIDI port published by this process. Other applications see it as
// a MIDI output they can send to.
bool virtualMidiAvailable(std::string& reason);
// True when this system's MIDI service is known to wedge when a virtual port is
// removed (Windows MIDI Services before the late-2026 fix, microsoft/MIDI#1047).
bool virtualMidiRemovalHangsService();
std::unique_ptr<MidiInputPort> createVirtualMidiInput(const std::string& name, MidiReceiveFn fn, std::string& error);

}  // namespace brack
