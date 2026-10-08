#pragma once
// The "main thread" in CLAP terms. The engine owns a dedicated thread running a
// native event loop; every engine mutation, every plugin main-thread call and
// all plugin GUI windows live on it. Public engine methods marshal onto it, so
// the engine can be driven from any thread (GUI, CLI, DLL clients).
//
// On macOS the host thread is the process's main thread, as plugins there expect (host_thread_mac.cpp).

#include <cstdint>
#include <functional>
#include <memory>

namespace brack {

class HostThread {
public:
    HostThread();
    ~HostThread();
    HostThread(const HostThread&) = delete;
    HostThread& operator=(const HostThread&) = delete;

    // Runs fn on the host thread and waits for it. Runs inline when already on
    // the host thread. Exceptions propagate to the caller.
    void invoke(const std::function<void()>& fn);
    // Queues fn for asynchronous execution on the host thread.
    void post(std::function<void()> fn);
    bool isCurrent() const;

    // Periodic timer on the host thread. Returns a non-zero id.
    uint32_t addTimer(uint32_t periodMs, std::function<void()> fn);
    void removeTimer(uint32_t id);

#ifdef __linux__
    // The host thread the caller is on, if any (for what is made on it without being told, like
    // an editor's window).
    static HostThread* current();
#endif
#ifndef _WIN32

    // A file descriptor watched on the host thread (an X11 connection; plugins' own, through CLAP
    // posix-fd-support and VST3 IRunLoop): fn gets the FdEvents that happened, of those asked
    // for, and FdError whenever (on macOS, only while some event is asked for). Returns a non-zero
    // id. Callable from any thread, as the timers.
    enum FdEvents : uint32_t { FdRead = 1, FdWrite = 2, FdError = 4 };
    uint32_t watchFd(int fd, uint32_t events, std::function<void(uint32_t events)> fn);
    void changeFd(uint32_t id, uint32_t events);
    void unwatchFd(uint32_t id);
#endif

    struct Impl;

private:
    std::unique_ptr<Impl> impl_;
};

}  // namespace brack
