// macOS and Linux: plugin crashes are signals. A guarded call sets a jump point of its thread's
// own; a fault on that thread while it runs (SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGTRAP) jumps back
// there from the handler, which runs on an alternate stack so that a stack overflow is caught too.
// A fault anywhere else goes on to the handler that was there before (a plugin host's, which
// records it and ends the process). abort() is left alone, as Windows leaves __fastfail: glibc's
// abort holds a lock that jumping out of it would never release. On macOS the kernel turns the
// Mach exceptions into these signals when no debugger has taken them first.
#include <dlfcn.h>
#include <setjmp.h>
#include <signal.h>
#ifdef __APPLE__
#include <sys/ucontext.h>
#else
#include <ucontext.h>
#endif

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>

#include "plugin/library.h"
#include "util/common.h"

namespace brack {

namespace {

constexpr uint32_t kUncaughtCppException = 0xE06D7363;  // Windows' code for it, named alike
constexpr int kFaults[] = {SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGTRAP};
constexpr size_t kAltStackSize = 64 * 1024;

// A guarded call this thread is in; `outer` is the one it was made from, if any.
struct Guard {
    sigjmp_buf jump;
    GuardFault* fault;
    Guard* outer;
};
thread_local Guard* t_guard = nullptr;

struct sigaction g_previous[NSIG];

void onFault(int sig, siginfo_t* info, void* context) {
    if (Guard* g = t_guard) {
        g->fault->code = (uint32_t)sig;
        g->fault->address = signalFaultAddress(info, context);
        siglongjmp(g->jump, 1);
    }
    // Not in a guarded call: as though this handler were not here.
    const struct sigaction& previous = g_previous[sig];
    if (previous.sa_flags & SA_SIGINFO) {
        if (previous.sa_sigaction) previous.sa_sigaction(sig, info, context);
    } else if (previous.sa_handler != SIG_DFL && previous.sa_handler != SIG_IGN) {
        previous.sa_handler(sig);
    } else {
        signal(sig, SIG_DFL);  // the fault comes again on return, to the default action
    }
}

void installHandlers() {
    static std::once_flag once;
    std::call_once(once, [] {
        struct sigaction sa{};
        sa.sa_sigaction = &onFault;
        sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
        sigemptyset(&sa.sa_mask);
        for (int sig : kFaults) sigaction(sig, &sa, &g_previous[sig]);
    });
}

// The handler's stack, unless the thread has one already (a plugin host's threads do).
void ensureAltStack() {
    thread_local bool checked = false;
    thread_local std::unique_ptr<char[]> stack;
    if (checked) return;
    checked = true;
    stack_t current{};
    sigaltstack(nullptr, &current);
    if (!(current.ss_flags & SS_DISABLE)) return;
    stack.reset(new char[kAltStackSize]);
    stack_t ss{};
    ss.ss_sp = stack.get();
    ss.ss_size = kAltStackSize;
    sigaltstack(&ss, nullptr);
}

}  // namespace

bool runGuarded(void (*fn)(void*), void* ctx, GuardFault* fault) {
    GuardFault local;
    if (!fault) fault = &local;
    installHandlers();
    ensureAltStack();
    Guard guard;
    guard.fault = fault;
    guard.outer = t_guard;
    if (sigsetjmp(guard.jump, 1)) {  // back from onFault, the signal mask as it was
        t_guard = guard.outer;
        return false;
    }
    t_guard = &guard;
    // A C++ exception out of fn is caught where the plugin and Brack share their C++ runtime. Where
    // each has its own, linked statically as plugins and Brack's programs are, unwinding from one
    // unwinder into the other aborts before it gets here.
    try {
        fn(ctx);
    } catch (...) {
        t_guard = guard.outer;
        guard.fault->code = kUncaughtCppException;  // guard's, not the argument: kept across sigsetjmp
        guard.fault->address = 0;
        return false;
    }
    t_guard = guard.outer;
    return true;
}

uint64_t signalFaultAddress(const void* info, const void* context) {
#if defined(__linux__) && defined(__x86_64__)
    (void)info;
    return (uint64_t) static_cast<const ucontext_t*>(context)->uc_mcontext.gregs[REG_RIP];
#elif defined(__linux__) && defined(__i386__)
    (void)info;
    return (uint32_t) static_cast<const ucontext_t*>(context)->uc_mcontext.gregs[REG_EIP];  // greg_t is signed
#elif defined(__linux__) && defined(__aarch64__)
    (void)info;
    return (uint64_t) static_cast<const ucontext_t*>(context)->uc_mcontext.pc;
#elif defined(__APPLE__) && defined(__x86_64__)
    (void)info;
    return (uint64_t) static_cast<const ucontext_t*>(context)->uc_mcontext->__ss.__rip;
#elif defined(__APPLE__) && defined(__aarch64__)
    (void)info;
    return (uint64_t)__darwin_arm_thread_state64_get_pc(static_cast<const ucontext_t*>(context)->uc_mcontext->__ss);
#else
    (void)context;
    return (uint64_t)(uintptr_t) static_cast<const siginfo_t*>(info)->si_addr;
#endif
}

// GuardFault::code is the signal (a guarded call's, or a plugin host's crash: brack_link_posix.cpp).
const char* faultName(uint32_t code) {
    switch (code) {
        case SIGSEGV: return "segmentation fault";
        case SIGBUS: return "bus error";
        case SIGILL: return "illegal instruction";
        case SIGFPE: return "arithmetic exception";
        case SIGABRT: return "abort";
        case SIGTRAP: return "trap";
        case kUncaughtCppException: return "uncaught C++ exception";
        default: return "";
    }
}

std::string faultLocation(uint64_t address) {
    char buf[64];
    Dl_info info{};
    if (dladdr(reinterpret_cast<void*>((uintptr_t)address), &info) && info.dli_fname && info.dli_fbase) {
        const char* slash = std::strrchr(info.dli_fname, '/');
        std::snprintf(buf, sizeof buf, "+0x%llx", (unsigned long long)(address - (uint64_t)(uintptr_t)info.dli_fbase));
        return std::string(slash ? slash + 1 : info.dli_fname) + buf;
    }
    std::snprintf(buf, sizeof buf, "0x%llx", (unsigned long long)address);
    return buf;
}

}  // namespace brack
