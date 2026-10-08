#include "util/realtime_linux.h"

#include <sched.h>
#include <signal.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <map>
#include <mutex>
#include <string>

#include "util/common.h"
#include "util/dbus_linux.h"

namespace brack::realtime {

namespace {

// Below the sound server's own threads, which have to be able to interrupt ours (PipeWire's run
// at 88).
constexpr int kPriority = 80;

bool isRealtime(pid_t tid) {
    const int policy = sched_getscheduler(tid) & ~SCHED_RESET_ON_FORK;
    return policy == SCHED_FIFO || policy == SCHED_RR;
}

// Threads the audio thread starts are not real-time (SCHED_RESET_ON_FORK).
bool direct(pid_t pid, pid_t tid, int& priority, std::string& error) {
    rlimit lim{};
    if (prlimit(pid, RLIMIT_RTPRIO, nullptr, &lim) != 0) lim.rlim_cur = 0;
    const int allowed = (int)std::min<rlim_t>(lim.rlim_cur, kPriority);
    for (int p : {kPriority, allowed}) {  // the first for CAP_SYS_NICE
        if (p <= 0) continue;
        sched_param param{};
        param.sched_priority = p;
        if (sched_setscheduler(tid, SCHED_FIFO | SCHED_RESET_ON_FORK, &param) == 0) {
            priority = p;
            return true;
        }
    }
    error = "SCHED_FIFO: " + std::string(allowed > 0 ? std::strerror(errno) : "no rtprio limit");
    return false;
}

// ---- RealtimeKit, through libdbus (only Brack calls it, never a plugin host)

// A connection of our own to the system bus, for a few calls to RealtimeKit.
class RealtimeKit {
public:
    explicit RealtimeKit(const dbus::Functions& d) : d_(d) {
        dbus::Error e;
        d_.errorInit(&e);
        bus_ = d_.busGetPrivate(dbus::kBusSystem, &e);
        if (bus_)
            d_.setExitOnDisconnect(bus_, 0);  // libdbus would end the process
        else
            take(e);
    }
    ~RealtimeKit() {
        if (!bus_) return;
        d_.connectionClose(bus_);
        d_.connectionUnref(bus_);
    }
    RealtimeKit(const RealtimeKit&) = delete;
    RealtimeKit& operator=(const RealtimeKit&) = delete;

    bool property(const char* name, long long& value) {
        void* m = message("org.freedesktop.DBus.Properties", "Get");
        const char* iface = kName;
        if (!m || !d_.appendArgs(m, dbus::kTypeString, &iface, dbus::kTypeString, &name, dbus::kTypeInvalid))
            return fail(m);
        void* reply = send(m);
        if (!reply) return false;
        dbus::Iter outer{}, inner{};
        bool ok = false;
        if (d_.iterInit(reply, &outer) && d_.iterArgType(&outer) == dbus::kTypeVariant) {
            d_.iterRecurse(&outer, &inner);
            if (d_.iterArgType(&inner) == dbus::kTypeInt32) {
                int32_t v = 0;
                d_.iterGetBasic(&inner, &v);
                value = v;
                ok = true;
            } else if (d_.iterArgType(&inner) == dbus::kTypeInt64) {
                int64_t v = 0;
                d_.iterGetBasic(&inner, &v);
                value = v;
                ok = true;
            }
        }
        d_.messageUnref(reply);
        if (!ok) error = std::string(name) + " is not a number";
        return ok;
    }

    bool makeThreadRealtime(pid_t pid, pid_t tid, uint32_t priority) {
        void* m = message(kName, "MakeThreadRealtimeWithPID");
        uint64_t p = (uint64_t)pid, t = (uint64_t)tid;
        if (!m || !d_.appendArgs(m, dbus::kTypeUint64, &p, dbus::kTypeUint64, &t, dbus::kTypeUint32, &priority,
                                 dbus::kTypeInvalid))
            return fail(m);
        void* reply = send(m);
        if (reply) d_.messageUnref(reply);
        return reply != nullptr;
    }

    std::string error;

private:
    static constexpr const char* kName = "org.freedesktop.RealtimeKit1";

    void* message(const char* iface, const char* method) {
        return bus_ ? d_.newMethodCall(kName, "/org/freedesktop/RealtimeKit1", iface, method) : nullptr;
    }
    bool fail(void* m) {
        if (m) d_.messageUnref(m);
        if (error.empty()) error = "out of memory";
        return false;
    }
    // Takes the message.
    void* send(void* m) {
        dbus::Error e;
        d_.errorInit(&e);
        void* reply = d_.sendWithReplyAndBlock(bus_, m, 1000, &e);
        d_.messageUnref(m);
        if (!reply) take(e);
        return reply;
    }
    void take(dbus::Error& e) {
        error = e.message ? e.message : e.name ? e.name : "failed";
        d_.errorFree(&e);
    }

    const dbus::Functions& d_;
    void* bus_ = nullptr;
};

// RealtimeKit wants a limit on how long the process's real-time threads may run without sleeping
// (RLIMIT_RTTIME). A thread past half of it gets SIGXCPU (fallBackOnOverrun()); past all of it,
// SIGKILL.
bool viaRealtimeKit(pid_t pid, pid_t tid, int& priority, std::string& error) {
    const dbus::Functions* d = dbus::functions();
    if (!d) {
        error = "RealtimeKit: no libdbus-1.so.3";
        return false;
    }
    RealtimeKit kit(*d);
    long long maxPriority = 0, maxRunUs = 0;
    rlimit lim{};
    bool ok = kit.property("MaxRealtimePriority", maxPriority) && kit.property("RTTimeUSecMax", maxRunUs);
    if (ok && prlimit(pid, RLIMIT_RTTIME, nullptr, &lim) == 0) {
        lim.rlim_max = std::min<rlim_t>(lim.rlim_max, (rlim_t)maxRunUs);
        lim.rlim_cur = std::min<rlim_t>(lim.rlim_cur, lim.rlim_max / 2);
        if (prlimit(pid, RLIMIT_RTTIME, &lim, nullptr) != 0) {
            kit.error = std::string("cannot limit the run time: ") + std::strerror(errno);
            ok = false;
        }
    }
    priority = (int)std::min<long long>(maxPriority, kPriority);
    ok = ok && priority > 0 && kit.makeThreadRealtime(pid, tid, (uint32_t)priority);
    if (!ok) error = "RealtimeKit: " + kit.error;
    return ok;
}

void onOverrun(int) {
    // The thread that overran gets the signal. SCHED_RESET_ON_FORK stays: it was set with the
    // real-time policy (here and by RealtimeKit), and only a privileged thread may clear it, so
    // leaving it out fails with EPERM and the thread runs on, real-time, to its SIGKILL.
    sched_param param{};
    sched_setscheduler(0, SCHED_OTHER | SCHED_RESET_ON_FORK, &param);
}

}  // namespace

pid_t threadId() { return (pid_t)syscall(SYS_gettid); }

void makeRealtime(pid_t pid, pid_t tid, const char* what) {
    if (tid <= 0 || isRealtime(tid)) return;  // already: a sound server's thread, say
    std::string outcome, why;
    int priority = 0;
    LogLevel level = LogLevel::Info;
    if (direct(pid, tid, priority, why)) {
        outcome = std::string(what) + ": real-time (SCHED_FIFO " + std::to_string(priority) + ")";
    } else if (std::string kitWhy; viaRealtimeKit(pid, tid, priority, kitWhy)) {
        outcome = std::string(what) + ": real-time (SCHED_RR " + std::to_string(priority) + ", through RealtimeKit)";
    } else {
        outcome = std::string(what) + ": normal priority (" + why + "; " + kitWhy + ")";
        level = LogLevel::Warning;
    }
    static std::mutex mutex;
    static std::map<std::string, std::string> last;  // what -> outcome
    std::lock_guard lock(mutex);
    std::string& before = last[what];
    if (before == outcome) return;
    before = outcome;
    log(level, outcome);
}

void fallBackOnOverrun() {
    static std::once_flag once;
    std::call_once(once, [] {
        struct sigaction old{};
        if (sigaction(SIGXCPU, nullptr, &old) != 0 || (old.sa_flags & SA_SIGINFO) || old.sa_handler != SIG_DFL) return;
        struct sigaction sa{};
        sa.sa_handler = &onOverrun;
        sa.sa_flags = SA_RESTART;
        sigemptyset(&sa.sa_mask);
        sigaction(SIGXCPU, &sa, nullptr);
    });
}

}  // namespace brack::realtime
