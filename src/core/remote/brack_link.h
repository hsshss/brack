#pragma once
// The plugin host's end of its link to the Brack process that started it (remote/host_protocol.h),
// per OS: brack_link_win32.cpp, brack_link_posix.cpp. The plugin host itself (plugin_host.cpp)
// is the same everywhere.

#include <memory>
#include <string>
#include <vector>

#include "remote/host_protocol.h"

namespace brack {

class BrackLink {
public:
    // From the plugin host's command line (after the program name). Null, with the exit code to
    // end with, when it is not one Brack of this build gave.
    static std::unique_ptr<BrackLink> open(const std::vector<std::string>& args, int& exitCode);
    ~BrackLink();
    BrackLink(const BrackLink&) = delete;
    BrackLink& operator=(const BrackLink&) = delete;

    // The next request; false once Brack is gone.
    bool receive(remote::Message& m);
    // Any thread.
    bool send(const remote::Message& m);

    remote::ControlBlock& control();
    // Audio thread: waits for Brack's "process"; false if it cannot. Should Brack go away instead,
    // this process ends with it.
    bool waitForBlock();
    // Audio thread: "done".
    void blockDone();
    void raiseAudioThreadPriority(double sampleRate, uint32_t maxFrames);

    // New shared memory for `bytes` of outputs, replacing the last one, which Brack lets go of
    // when it maps this. What tells Brack where it is (a handle, a descriptor, a name) goes into `ref`.
    // Null if it cannot be made.
    float* createOutputs(uint64_t bytes, remote::Message& ref);

    // From now on, a crash nothing catches (on a thread of the plugin's own, in its window
    // procedures) is recorded in the control block before the process ends, without any error
    // dialog to wait for.
    void recordUncaughtCrashes();
    // On each other thread that runs plugin code, first: what recording a crash there needs
    // (POSIX: a stack for the handler, should the crash be the thread's stack overflowing). Also
    // on the threads the plugin starts (Linux).
    static void protectThisThread();
    // Ends this process at once after a crash, leaving `report` for Brack. Nothing of the plugin
    // is called again, not even to unload it. Any thread.
    [[noreturn]] void endAfterCrash(const std::string& report);
    // Ends this process at once, without unloading the plugin: whatever it would do on the way
    // out cannot matter any more, and might not return.
    [[noreturn]] void end();

    struct Impl;

private:
    explicit BrackLink(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

}  // namespace brack
