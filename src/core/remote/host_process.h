#pragma once
// Brack's end of one plugin host process (remote/host_protocol.h), per OS: host_process_win32.cpp,
// host_process_posix.cpp. RemotePlugin and RemoteDescriber (remote_plugin.cpp) drive it the same
// way everywhere.

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include "remote/host_protocol.h"

namespace brack {

class HostProcess {
public:
    enum class Outcome { Ok, Ended, TimedOut };

    // brack-host-<architecture>, started. Null (error: why) when this computer does not run that
    // architecture, the host is not installed, or it would not start.
    static std::unique_ptr<HostProcess> start(const std::string& architecture, std::string& error);
    // Ends the process: it sees its link close and ends; one that does not, soon, is killed.
    virtual ~HostProcess() = default;

    // Sends a request and waits for its reply, handing notes that arrive meanwhile to onNote.
    // A host that does not answer within `timeout` is ended.
    virtual Outcome call(const remote::Message& request, remote::Message& reply, std::chrono::milliseconds timeout) = 0;
    // Notes that arrived since the last call (host thread, between calls).
    virtual void pollNotes() = 0;
    virtual bool running() = 0;
    virtual void terminate() = 0;  // any thread
    // Why the process ended: the crash it recorded, else how it exited.
    virtual std::string endReport() = 0;

    virtual remote::ControlBlock& control() = 0;
    // Audio thread, without allocating: "process", then waiting for "done" (Ended if the host
    // is gone, TimedOut after `timeoutMs`).
    virtual void signalBlock() = 0;
    virtual Outcome waitBlock(uint32_t timeoutMs) = 0;

    // Maps the outputs the host made at its last activation; `ref` is where, as the host said.
    virtual float* mapOutputs(const remote::Message& ref, uint64_t bytes) = 0;
    virtual void unmapOutputs() = 0;
    // Before an editor opens: lets the host bring its window to the front.
    virtual void allowForeground() = 0;

    std::function<void(const remote::Message&)> onNote;
};

}  // namespace brack
