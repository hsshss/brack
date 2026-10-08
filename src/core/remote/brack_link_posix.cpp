// macOS and Linux: the plugin host's end of its link to Brack. Descriptors 3 (messages) and 4 (the
// control block, with the futex for the audio thread's "process" and "done") come from Brack; the
// outputs are POSIX shared memory made here (createOutputs()).
#include <dlfcn.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <unistd.h>
#ifdef __linux__
#include <sys/prctl.h>
#else
#include <mach/mach.h>
#include <sys/event.h>
#include <sys/sysctl.h>
#endif

#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <thread>

#include "plugin/library.h"
#include "remote/brack_link.h"
#include "remote/futex_posix.h"
#include "util/common.h"
#ifdef __linux__
#include "util/realtime_linux.h"
#else
#include "util/realtime_mac.h"
#endif

namespace brack {

using remote::BlockTurn;
using remote::ControlBlock;
using remote::Message;

namespace {

#ifdef MSG_NOSIGNAL
constexpr int kNoSignal = MSG_NOSIGNAL;  // Brack gone is the end of the loop, not SIGPIPE
#else
constexpr int kNoSignal = 0;
#endif

ControlBlock* g_control = nullptr;  // for the crash record

void writeCrashRecord(uint32_t code, const char* report) {
    ControlBlock& c = *g_control;
    c.crashCode = code;
    c.crashed = 1;
    std::snprintf(c.crashReport, sizeof c.crashReport, "%s", report);
}

// Each thread that runs plugin code gets a stack of its own for the handler, so that a stack
// overflow there is recorded too. Mapped, not thread_local storage, which would be taken out of
// every thread's stack, even one a plugin starts with a small stack.
class AltStack {
public:
    AltStack() {
        void* memory = mmap(nullptr, kSize, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (memory == MAP_FAILED) return;
        stack_t ss{};
        ss.ss_sp = memory;
        ss.ss_size = kSize;
        if (sigaltstack(&ss, nullptr) == 0) memory_ = memory;
        else munmap(memory, kSize);
    }
    ~AltStack() {
        if (!memory_) return;
        stack_t ss{};
        ss.ss_flags = SS_DISABLE;
        sigaltstack(&ss, nullptr);
        munmap(memory_, kSize);
    }
    AltStack(const AltStack&) = delete;
    AltStack& operator=(const AltStack&) = delete;

private:
    static constexpr size_t kSize = 64 * 1024;
    void* memory_ = nullptr;
};

#ifdef __APPLE__
// Threads with a stack of their own for the handler (altStackForThisThread()): a crash there is
// the signal handler's. Others are the plugin's, which nothing can give one on arm64.
constexpr int kMaxProtected = 64;
std::atomic<mach_port_t> g_protected[kMaxProtected];

void markProtected() {
    const mach_port_t self = mach_thread_self();  // a name kept for the thread's life
    for (auto& slot : g_protected) {
        mach_port_t none = MACH_PORT_NULL;
        if (slot.load() == self || slot.compare_exchange_strong(none, self)) return;
    }
}

bool isProtected(mach_port_t thread) {
    for (auto& slot : g_protected)
        if (slot.load() == thread) return true;
    return false;
}
#endif

void altStackForThisThread() {
    thread_local AltStack stack;
#ifdef __APPLE__
    markProtected();
#endif
}

// A crash outside a guarded call (Brack's calls are guarded: plugin/library_posix.cpp, which
// passes the rest on here): on a plugin thread, abort(), or the host's own. Records it, then takes
// the signal's default action, which ends the process. Nothing here allocates: a crash inside the
// allocator (heap corruption caught by glibc, say) holds its lock.
void onCrashSignal(int sig, siginfo_t* info, void* context) {
    ControlBlock& c = *g_control;
    if (c.crashed && c.crashReport[0]) {  // recorded already (macOS: a stack overflow, from its Mach exception)
        signal(sig, SIG_DFL);
        raise(sig);
        return;
    }
    c.crashCode = (uint32_t)sig;
    c.crashed = 1;  // enough for a report, should what follows fail in a broken process
    const uint64_t at = signalFaultAddress(info, context);
    const char* name = faultName((uint32_t)sig);
    Dl_info module{};
    if (dladdr(reinterpret_cast<void*>((uintptr_t)at), &module) && module.dli_fname && module.dli_fbase) {
        const char* slash = std::strrchr(module.dli_fname, '/');
        std::snprintf(c.crashReport, sizeof c.crashReport, "crashed in the plugin: %s (0x%08X) at %s+0x%llx", name,
                      (unsigned)sig, slash ? slash + 1 : module.dli_fname,
                      (unsigned long long)(at - (uint64_t)(uintptr_t)module.dli_fbase));
    } else {
        std::snprintf(c.crashReport, sizeof c.crashReport, "crashed in the plugin: %s (0x%08X) at 0x%llx", name,
                      (unsigned)sig, (unsigned long long)at);
    }
    signal(sig, SIG_DFL);
    raise(sig);
}

#ifdef __APPLE__
// A stack overflow on a thread the plugin started (no stack for the signal handler) is recorded
// from a thread of our own, which sees it first as a Mach exception: arm64 cannot run the handler
// on the overflowed stack, and would end the process with SIGILL alone. Every exception is then
// declined, so that the kernel goes on to deliver the signal as before.
#pragma pack(push, 4)  // as Mach messages are laid out
struct ExceptionRequest {
    mach_msg_header_t head;
    mach_msg_body_t body;
    mach_msg_port_descriptor_t thread;
    mach_msg_port_descriptor_t task;
    NDR_record_t ndr;
    exception_type_t exception;
    mach_msg_type_number_t codeCount;
    int64_t code[2];
    char trailer[128];
};
struct ExceptionReply {
    mach_msg_header_t head;
    NDR_record_t ndr;
    kern_return_t result;
};
#pragma pack(pop)

// The thread's stack and instruction pointers.
bool threadPointers(mach_port_t thread, uint64_t& sp, uint64_t& pc) {
#if defined(__aarch64__)
    arm_thread_state64_t state;
    mach_msg_type_number_t count = ARM_THREAD_STATE64_COUNT;
    if (thread_get_state(thread, ARM_THREAD_STATE64, (thread_state_t)&state, &count) != KERN_SUCCESS) return false;
    sp = arm_thread_state64_get_sp(state);
    pc = arm_thread_state64_get_pc(state);
#else
    x86_thread_state64_t state;
    mach_msg_type_number_t count = x86_THREAD_STATE64_COUNT;
    if (thread_get_state(thread, x86_THREAD_STATE64, (thread_state_t)&state, &count) != KERN_SUCCESS) return false;
    sp = state.__rsp;
    pc = state.__rip;
#endif
    return true;
}

// Not under a debugger, which takes the exceptions from the same port.
bool beingDebugged() {
    kinfo_proc info{};
    size_t size = sizeof info;
    int name[] = {CTL_KERN, KERN_PROC, KERN_PROC_PID, getpid()};
    return sysctl(name, 4, &info, &size, nullptr, 0) == 0 && (info.kp_proc.p_flag & P_TRACED);
}

void watchStackOverflows() {
    if (beingDebugged()) return;
    mach_port_t port = MACH_PORT_NULL;
    if (mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE, &port) != KERN_SUCCESS) return;
    mach_port_insert_right(mach_task_self(), port, port, MACH_MSG_TYPE_MAKE_SEND);
    if (task_set_exception_ports(mach_task_self(), EXC_MASK_BAD_ACCESS, port, EXCEPTION_DEFAULT | MACH_EXCEPTION_CODES,
                                 THREAD_STATE_NONE) != KERN_SUCCESS)
        return;
    std::thread([port] {
        for (;;) {
            ExceptionRequest request{};
            if (mach_msg(&request.head, MACH_RCV_MSG, 0, sizeof request, port, MACH_MSG_TIMEOUT_NONE, MACH_PORT_NULL) !=
                KERN_SUCCESS)
                continue;
            const mach_port_t thread = request.thread.name;
            uint64_t sp = 0, pc = 0;
            const uint64_t fault = (uint64_t)request.code[1];
            if (!isProtected(thread) && threadPointers(thread, sp, pc) && fault < sp + 4096 && fault + (1u << 20) > sp) {
                const int sig = request.code[0] == KERN_PROTECTION_FAILURE ? SIGBUS : SIGSEGV;
                ControlBlock& c = *g_control;
                c.crashCode = (uint32_t)sig;
                c.crashed = 1;
                Dl_info module{};
                if (dladdr(reinterpret_cast<void*>((uintptr_t)pc), &module) && module.dli_fname && module.dli_fbase) {
                    const char* slash = std::strrchr(module.dli_fname, '/');
                    std::snprintf(c.crashReport, sizeof c.crashReport, "crashed in the plugin: stack overflow, %s (0x%08X) at %s+0x%llx",
                                  faultName((uint32_t)sig), (unsigned)sig, slash ? slash + 1 : module.dli_fname,
                                  (unsigned long long)(pc - (uint64_t)(uintptr_t)module.dli_fbase));
                } else {
                    std::snprintf(c.crashReport, sizeof c.crashReport, "crashed in the plugin: stack overflow, %s (0x%08X)",
                                  faultName((uint32_t)sig), (unsigned)sig);
                }
            }
            mach_port_deallocate(mach_task_self(), request.thread.name);
            mach_port_deallocate(mach_task_self(), request.task.name);
            ExceptionReply reply{};
            reply.head.msgh_bits = MACH_MSGH_BITS(MACH_MSGH_BITS_REMOTE(request.head.msgh_bits), 0);
            reply.head.msgh_remote_port = request.head.msgh_remote_port;
            reply.head.msgh_size = sizeof reply;
            reply.head.msgh_id = request.head.msgh_id + 100;
            reply.ndr = NDR_record;
            reply.result = KERN_FAILURE;  // on to the signal
            mach_msg(&reply.head, MACH_SEND_MSG, sizeof reply, 0, MACH_PORT_NULL, MACH_MSG_TIMEOUT_NONE, MACH_PORT_NULL);
        }
    }).detach();
}
#endif

int fdArg(const std::string& s) { return (int)std::strtol(s.c_str(), nullptr, 10); }

}  // namespace

struct BrackLink::Impl {
    int messages = -1;
    std::mutex writeMutex;
    void* outputs = nullptr;
    size_t outputsBytes = 0;
    uint32_t outputsCount = 0;
    int outputsFd = -1;  // Linux: kept open for Brack to open through /proc; macOS: until sent

    // macOS: the outputs' descriptor goes along with the next message's first byte (SCM_RIGHTS).
    ssize_t sendFirst(uint8_t* data, size_t n) {
        iovec iov{data, n};
        alignas(cmsghdr) char control[CMSG_SPACE(sizeof(int))] = {};
        msghdr msg{};
        msg.msg_iov = &iov;
        msg.msg_iovlen = 1;
        msg.msg_control = control;
        msg.msg_controllen = sizeof control;
        cmsghdr* c = CMSG_FIRSTHDR(&msg);
        c->cmsg_level = SOL_SOCKET;
        c->cmsg_type = SCM_RIGHTS;
        c->cmsg_len = CMSG_LEN(sizeof(int));
        std::memcpy(CMSG_DATA(c), &outputsFd, sizeof(int));
        const ssize_t done = sendmsg(messages, &msg, kNoSignal);
        if (done > 0) {
            close(outputsFd);
            outputsFd = -1;
        }
        return done;
    }

    bool io(bool write, uint8_t* data, size_t n) {
        while (n > 0) {
#ifdef __APPLE__
            const bool withFd = write && outputsFd >= 0;
#else
            const bool withFd = false;
#endif
            const ssize_t done = withFd ? sendFirst(data, n) : write ? ::send(messages, data, n, kNoSignal) : recv(messages, data, n, 0);
            if (done < 0 && errno == EINTR) continue;
            if (done <= 0) return false;
            data += done;
            n -= (size_t)done;
        }
        return true;
    }
};

std::unique_ptr<BrackLink> BrackLink::open(const std::vector<std::string>& args, int& exitCode) {
    // --brack-host <version> <messages> <control block>
    exitCode = remote::kExitBadArguments;
    if (args.size() != 4 || args[0] != "--brack-host") return nullptr;
    if (std::strtoul(args[1].c_str(), nullptr, 10) != remote::kProtocolVersion) {
        exitCode = remote::kExitWrongVersion;
        return nullptr;
    }
#ifdef __linux__
    prctl(PR_SET_PDEATHSIG, SIGKILL);
#else
    const pid_t brack = getppid();
    const int queue = kqueue();
    struct kevent watch;
    EV_SET(&watch, brack, EVFILT_PROC, EV_ADD, NOTE_EXIT, 0, nullptr);
    if (queue < 0 || kevent(queue, &watch, 1, nullptr, 0, nullptr) != 0 || getppid() != brack) _exit(0);
    realtime::prepareWorkInterval();
    std::thread([queue] {
        struct kevent happened;
        while (kevent(queue, nullptr, 0, &happened, 1, nullptr) != 1) {
        }
        _exit(0);
    }).detach();
#endif
    const int control = fdArg(args[3]);
    void* view = mmap(nullptr, sizeof(ControlBlock), PROT_READ | PROT_WRITE, MAP_SHARED, control, 0);
    close(control);
    if (view == MAP_FAILED) return nullptr;
    g_control = static_cast<ControlBlock*>(view);
    auto impl = std::make_unique<Impl>();
    impl->messages = fdArg(args[2]);
    fcntl(impl->messages, F_SETFD, FD_CLOEXEC);
#ifdef SO_NOSIGPIPE
    const int on = 1;
    setsockopt(impl->messages, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof on);  // no MSG_NOSIGNAL there
#endif
    return std::unique_ptr<BrackLink>(new BrackLink(std::move(impl)));
}

BrackLink::BrackLink(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
BrackLink::~BrackLink() = default;

bool BrackLink::receive(Message& m) {
    uint8_t len[4];
    if (!impl_->io(false, len, 4)) return false;
    const uint32_t n = len[0] | len[1] << 8 | len[2] << 16 | (uint32_t)len[3] << 24;
    if (n > remote::kMaxMessageBytes) return false;
    std::vector<uint8_t> body(n);
    return impl_->io(false, body.data(), body.size()) && remote::decodeMessage(body, m);
}

bool BrackLink::send(const Message& m) {
    const std::vector<uint8_t> frame = remote::encodeMessage(m);
    std::lock_guard lock(impl_->writeMutex);
    return impl_->io(true, const_cast<uint8_t*>(frame.data()), frame.size());
}

ControlBlock& BrackLink::control() { return *g_control; }

#ifdef __APPLE__
namespace {
double g_blockSeconds = 0;  // the audio thread's, for its work interval
}  // namespace
#endif

// Brack going away wakes nobody here: the process ends with it (open()).
bool BrackLink::waitForBlock() {
    while (std::atomic_ref(g_control->turn).load(std::memory_order_acquire) != (uint32_t)BlockTurn::Host)
        futex::wait(&g_control->turn, (uint32_t)BlockTurn::Brack, nullptr);
#ifdef __APPLE__
    realtime::beginBlock(g_blockSeconds);
#endif
    return true;
}

void BrackLink::blockDone() {
#ifdef __APPLE__
    realtime::endBlock();
#endif
    std::atomic_ref(g_control->turn).store((uint32_t)BlockTurn::Brack, std::memory_order_release);
    futex::wakeAll(&g_control->turn);
}

// Linux: Brack makes this thread real-time (util/realtime_linux.h). It may have to ask RealtimeKit,
// over D-Bus, and a plugin host of another architecture would have no libdbus of its own for that.
void BrackLink::raiseAudioThreadPriority(double sampleRate, uint32_t maxFrames) {
#ifdef __linux__
    thread_local bool raised = false;
    if (raised) return;
    raised = true;
    realtime::fallBackOnOverrun();
    send({{"note", "audioThread"}, {"thread", realtime::threadId()}});
#else
    g_blockSeconds = maxFrames / sampleRate;
    std::string error = realtime::makeThisThreadRealtime(sampleRate, maxFrames);
    if (error.empty()) error = realtime::joinWorkInterval();
    send({{"note", "audioThread"}, {"error", error}});
#endif
}

// Unlinked at once, so that no name is left behind if either process ends in between. Linux keeps
// the descriptor open for Brack to open through /proc/<pid>/fd; macOS sends it to Brack.
float* BrackLink::createOutputs(uint64_t bytes, Message& ref) {
    if (impl_->outputs) munmap(impl_->outputs, impl_->outputsBytes);
    if (impl_->outputsFd >= 0) close(impl_->outputsFd);
    impl_->outputs = nullptr;
    impl_->outputsFd = -1;
    const std::string name =
        "/brack-host-" + std::to_string(getpid()) + "-" + std::to_string(++impl_->outputsCount);
    const int fd = shm_open(name.c_str(), O_RDWR | O_CREAT | O_EXCL, 0600);
    if (fd < 0) return nullptr;
    shm_unlink(name.c_str());
    void* view = ftruncate(fd, (off_t)bytes) == 0
                     ? mmap(nullptr, (size_t)bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0)
                     : MAP_FAILED;
    if (view == MAP_FAILED) {
        close(fd);
        return nullptr;
    }
    impl_->outputs = view;
    impl_->outputsBytes = (size_t)bytes;
    impl_->outputsFd = fd;
#ifdef __linux__
    ref = fd;
#else
    ref = "sent";  // with the next message (Impl::sendFirst())
#endif
    return static_cast<float*>(view);
}

void BrackLink::protectThisThread() { altStackForThisThread(); }

// Linux: the threads a plugin starts get one too (src/host/plugin_threads_linux.cpp). macOS cannot
// give them one (plugins bind pthread_create to libSystem): their stack overflows are recorded from
// the Mach exception instead (watchStackOverflows()).
void BrackLink::recordUncaughtCrashes() {
    altStackForThisThread();
#ifdef __APPLE__
    watchStackOverflows();
#endif
    struct sigaction sa{};
    sa.sa_sigaction = &onCrashSignal;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK | SA_RESETHAND;
    sigemptyset(&sa.sa_mask);
    for (int sig : {SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT, SIGTRAP}) sigaction(sig, &sa, nullptr);
}

void BrackLink::endAfterCrash(const std::string& report) {
    writeCrashRecord(0, report.c_str());
    _exit(1);
}

void BrackLink::end() { _exit(0); }

}  // namespace brack
