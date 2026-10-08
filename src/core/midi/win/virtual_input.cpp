// Virtual MIDI ports on Windows via the in-box Windows MIDI Services (MidiSrv).
#include "midi/midi_input.h"
#include "midi/win/midisrv_virtual_port.h"

namespace brack {

namespace {
class VirtualInput final : public MidiInputPort {
public:
    explicit VirtualInput(std::unique_ptr<win::MidiSrvVirtualPort> p) : port_(std::move(p)) {}

private:
    std::unique_ptr<win::MidiSrvVirtualPort> port_;
};
}  // namespace

bool virtualMidiAvailable(std::string& reason) { return win::MidiSrvVirtualPort::isAvailable(reason); }
bool virtualMidiRemovalHangsService() { return win::MidiSrvVirtualPort::removalHangsService(); }

std::unique_ptr<MidiInputPort> createVirtualMidiInput(const std::string& name, MidiReceiveFn fn, std::string& error) {
    auto port = win::MidiSrvVirtualPort::create(name, std::move(fn), error);
    if (!port) return nullptr;
    return std::make_unique<VirtualInput>(std::move(port));
}

}  // namespace brack
