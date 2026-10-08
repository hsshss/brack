// Linux without the ALSA development files (midi/alsa/): no MIDI ports; sources fed through the
// API work as everywhere.
#include "midi/midi_input.h"

namespace brack {

namespace {
const char* kUnavailable = "MIDI ports are not available on this system yet";
}

std::vector<std::string> listHardwareMidiInputs() { return {}; }

std::unique_ptr<MidiInputPort> openHardwareMidiInput(const std::string&, MidiReceiveFn, std::string& error) {
    error = kUnavailable;
    return nullptr;
}

bool virtualMidiAvailable(std::string& reason) {
    reason = kUnavailable;
    return false;
}

bool virtualMidiRemovalHangsService() { return false; }

std::unique_ptr<MidiInputPort> createVirtualMidiInput(const std::string&, MidiReceiveFn, std::string& error) {
    error = kUnavailable;
    return nullptr;
}

}  // namespace brack
