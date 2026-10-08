// macOS and Linux: Brack's end of a plugin host process. A socket for the messages, POSIX shared
// memory for the control block and the outputs, and a futex on the control block for the audio
// thread's "process" and "done". The host is started directly, or through the emulator that runs its
// architecture here (architectureLauncher). See remote/host_process.h.
#include <dlfcn.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <future>
#include <mutex>
#include <thread>

#include "remote/futex_posix.h"
#include "remote/host_process.h"
#include "remote/plugin_host.h"
#include "util/common.h"
#ifdef __linux__
#include "util/realtime_linux.h"
#else
#include "util/realtime_mac.h"
#endif

extern char** environ;

namespace brack {

using remote::BlockTurn;
using remote::ControlBlock;
using remote::Message;
using Clock = std::chrono::steady_clock;

// Beside the module (a build's bin, or an application that keeps libbrack.so with its plugin
// hosts), else an installation's <prefix>/libexec/brack, beside its bin and lib.
std::filesystem::path pluginHostExecutable(const std::string& architecture) {
    std::filesystem::path module;
    Dl_info info{};
    if (dladdr(reinterpret_cast<void*>(&pluginHostExecutable), &info) && info.dli_fname) {
        char resolved[4096];
        if (realpath(info.dli_fname, resolved)) module = resolved;
    }
    std::error_code ec;
#ifdef __linux__
    // For the main program dladdr gives argv[0], which may be relative to a directory left since.
    if (module.empty() || (info.dli_fname && std::strcmp(info.dli_fname, program_invocation_name) == 0))
        module = std::filesystem::read_symlink("/proc/self/exe", ec);
#endif
    const std::string name = "brack-host-" + architecture;
    const std::filesystem::path beside = module.parent_path() / name;
    const std::filesystem::path installed = module.parent_path().parent_path() / "libexec" / "brack" / name;
    return !std::filesystem::exists(beside, ec) && std::filesystem::exists(installed, ec) ? installed : beside;
}

namespace {

// Runs `fn` on a thread that lasts as long as the process. Linux sends a plugin host its
// parent-death signal (BrackLink::open()) when the thread that started it ends, not the process,
// and plugins are made on threads that end (Engine::loadSessionJson(), any caller of
// Engine::addPlugin()): every host starts from this one. It blocks all signals, leaving the
// process's to its other threads.
int onLastingThread(std::function<int()> fn) {
#ifdef __linux__
    struct Queue {
        std::mutex mutex;
        std::condition_variable cv;
        std::deque<std::packaged_task<int()>> tasks;
    };
    static Queue* queue = [] {
        auto* q = new Queue;  // never destroyed: its thread runs until the process ends
        sigset_t all, before;
        sigfillset(&all);
        pthread_sigmask(SIG_SETMASK, &all, &before);
        std::thread([q] {
            for (;;) {
                std::unique_lock lock(q->mutex);
                q->cv.wait(lock, [q] { return !q->tasks.empty(); });
                std::packaged_task<int()> task = std::move(q->tasks.front());
                q->tasks.pop_front();
                lock.unlock();
                task();
            }
        }).detach();
        pthread_sigmask(SIG_SETMASK, &before, nullptr);
        return q;
    }();
    std::packaged_task<int()> task(std::move(fn));
    std::future<int> result = task.get_future();
    {
        std::lock_guard lock(queue->mutex);
        queue->tasks.push_back(std::move(task));
    }
    queue->cv.notify_one();
    return result.get();
#else
    return fn();
#endif
}

#ifdef MSG_NOSIGNAL
constexpr int kNoSignal = MSG_NOSIGNAL;  // a host that is gone is an error, not SIGPIPE
#else
constexpr int kNoSignal = 0;
#endif

bool closeOnExec(int fd) { return fcntl(fd, F_SETFD, FD_CLOEXEC) == 0; }

// `fd` moved to 5 or above (close-on-exec), for a host to get as 3 or 4: dup2 onto 3 and 4 would
// otherwise overwrite one source with the other first (a control block at 3 became the socket's
// copy, now and then when hosts started at once), or leave one already at its place close-on-exec.
int aboveHostDescriptors(int fd) {
    if (fd > 4) return fd;
    const int moved = fcntl(fd, F_DUPFD_CLOEXEC, 5);
    close(fd);
    return moved;
}

void cpuRelax() {
#if defined(__x86_64__) || defined(__i386__)
    __builtin_ia32_pause();
#elif defined(__aarch64__)
    asm volatile("yield");
#endif
}

class PosixHostProcess final : public HostProcess {
public:
    static std::unique_ptr<HostProcess> create(const std::string& architecture, std::string& error) {
        std::vector<std::string> launcher;
        if (!architectureLauncher(architecture, launcher)) {
            error = "this computer does not run " + architecture + " programs";
            return nullptr;
        }
        const std::filesystem::path exe = pluginHostExecutable(architecture);
        if (access(exe.c_str(), X_OK) != 0) {
            error = "no plugin host for " + architecture + " plugins (" + pathToUtf8(exe.filename()) +
                    " is not installed beside Brack)";
            return nullptr;
        }
        auto p = std::unique_ptr<PosixHostProcess>(new PosixHostProcess);
        if (!p->launch(launcher, exe, error)) return nullptr;
        return p;
    }

    ~PosixHostProcess() override {
        unmapOutputs();
        for (int fd : received_) close(fd);
        if (messages_ >= 0) close(messages_);  // the host sees its link close and ends
        if (pid_ > 0 && !reaped_) {
            const auto until = Clock::now() + std::chrono::seconds(2);
            while (!reap(false) && Clock::now() < until) std::this_thread::sleep_for(std::chrono::milliseconds(5));
            if (!reaped_) {
                kill(pid_, SIGKILL);
                reap(true);
            }
        }
        if (control_) munmap(control_, sizeof(ControlBlock));
    }

    Outcome call(const Message& request, Message& reply, std::chrono::milliseconds timeout) override {
        const auto until = Clock::now() + timeout;
        const std::vector<uint8_t> frame = remote::encodeMessage(request);
        Outcome o = io(true, const_cast<uint8_t*>(frame.data()), frame.size(), until);
        while (o == Outcome::Ok) {
            Message m;
            o = readMessage(m, until);
            if (o != Outcome::Ok) break;
            if (m.contains("reply")) {
                reply = std::move(m);
                return Outcome::Ok;
            }
            note(m);
        }
        if (o == Outcome::TimedOut) terminate();
        return o;
    }

    void pollNotes() override {
        for (;;) {
            pollfd p{messages_, POLLIN, 0};
            if (poll(&p, 1, 0) <= 0 || !(p.revents & POLLIN)) return;
            Message m;
            if (readMessage(m, Clock::now() + std::chrono::seconds(2)) != Outcome::Ok) {
                terminate();  // a note that does not arrive whole leaves the stream out of step
                return;
            }
            note(m);
        }
    }

    bool running() override { return !reap(false); }
    void terminate() override {
        if (!reaped_) kill(pid_, SIGKILL);
    }

    std::string endReport() override {
        const auto until = Clock::now() + std::chrono::seconds(1);
        while (!reap(false) && Clock::now() < until) std::this_thread::sleep_for(std::chrono::milliseconds(5));
        if (!reaped_) return "stopped answering";
        if (control_->crashed) {
            if (control_->crashReport[0])
                return std::string(control_->crashReport, strnlen(control_->crashReport, sizeof control_->crashReport));
            return faultReport("the plugin", {control_->crashCode, 0});
        }
        char buf[128];
        if (WIFSIGNALED(status_)) {
            const int sig = WTERMSIG(status_);
            std::snprintf(buf, sizeof buf, "crashed: %s (signal %d)", strsignal(sig), sig);
        } else if (WIFEXITED(status_) && WEXITSTATUS(status_) == remote::kExitWrongVersion) {
            return "the plugin host is from another build of Brack (build every architecture again)";
        } else {
            std::snprintf(buf, sizeof buf, "ended unexpectedly (exit code %d)", WIFEXITED(status_) ? WEXITSTATUS(status_) : -1);
        }
        return buf;
    }

    ControlBlock& control() override { return *control_; }

    void signalBlock() override {
        std::atomic_ref(control_->turn).store((uint32_t)BlockTurn::Host, std::memory_order_release);
        futex::wakeAll(&control_->turn);
    }

    // Spins a little first: a plugin is often done that soon, and sleeping and being woken costs
    // about 12 µs on WSL2. A host that ends wakes nobody, so the sleep after that is cut into
    // slices, between which we look whether it is still there.
    Outcome waitBlock(uint32_t timeoutMs) override {
        constexpr auto kSpin = std::chrono::microseconds(20);
        constexpr auto kSlice = std::chrono::milliseconds(10);
        std::atomic_ref turn(control_->turn);
        auto ours = [&] { return turn.load(std::memory_order_acquire) == (uint32_t)BlockTurn::Brack; };
        const auto start = Clock::now();
        while (Clock::now() - start < kSpin) {
            if (ours()) return Outcome::Ok;
            cpuRelax();
        }
        const auto until = start + std::chrono::milliseconds(timeoutMs);
        for (bool waited = false;; waited = true) {
            if (ours()) return Outcome::Ok;
            const auto left = until - Clock::now();
            if (left <= Clock::duration::zero()) return Outcome::TimedOut;
            if (waited && ended()) return Outcome::Ended;
            const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(std::min<Clock::duration>(left, kSlice));
            const timespec slice{0, (long)ns.count()};
            futex::wait(&control_->turn, (uint32_t)BlockTurn::Host, &slice);
        }
    }

    // `ref`: on Linux, the host's descriptor of the shared memory it made (already unlinked), opened
    // here through /proc. On macOS the descriptor itself, which came with a message (received_).
    float* mapOutputs(const Message& ref, uint64_t bytes) override {
        unmapOutputs();
        if (!bytes) return nullptr;
#ifdef __linux__
        if (!ref.is_number_integer()) return nullptr;
        const std::string path = "/proc/" + std::to_string(pid_) + "/fd/" + std::to_string(ref.get<int>());
        const int fd = open(path.c_str(), O_RDWR | O_CLOEXEC);
        if (fd < 0) return nullptr;
#else
        if (ref != "sent" || received_.empty()) return nullptr;
        const int fd = received_.front();
        received_.pop_front();
#endif
        void* view = mmap(nullptr, (size_t)bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        close(fd);
        if (view == MAP_FAILED) return nullptr;
        outputs_ = view;
        outputsBytes_ = (size_t)bytes;
        return static_cast<float*>(view);
    }

    void unmapOutputs() override {
        if (outputs_) munmap(outputs_, outputsBytes_);
        outputs_ = nullptr;
    }

    void allowForeground() override {}

private:
    PosixHostProcess() = default;

    bool launch(const std::vector<std::string>& launcher, const std::filesystem::path& exe, std::string& error) {
        auto fail = [&](const char* what) {
            error = std::string("cannot start the plugin host: ") + what + " failed (" + std::strerror(errno) + ")";
            return false;
        };
        int messages[2] = {-1, -1};
        if (socketpair(AF_UNIX, SOCK_STREAM, 0, messages) != 0) return fail("socketpair");
        messages_ = messages[0];
        for (int fd : messages) closeOnExec(fd);
        messages[1] = aboveHostDescriptors(messages[1]);
        if (messages[1] < 0) return fail("moving the socket");
#ifdef SO_NOSIGPIPE
        const int on = 1;
        setsockopt(messages_, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof on);  // no MSG_NOSIGNAL there
#endif

        // The control block, unlinked as soon as it is open: the host gets the descriptor.
        static std::atomic<uint32_t> counter{0};
        const std::string name = "/brack-" + std::to_string(getpid()) + "-" + std::to_string(++counter);
        int control = shm_open(name.c_str(), O_RDWR | O_CREAT | O_EXCL, 0600);
        if (control < 0) {
            close(messages[1]);
            return fail("shm_open");
        }
        shm_unlink(name.c_str());
        closeOnExec(control);
        control = aboveHostDescriptors(control);
        if (control < 0) {
            close(messages[1]);
            return fail("moving the control block");
        }
        bool mapped = ftruncate(control, sizeof(ControlBlock)) == 0;
        void* view = mapped ? mmap(nullptr, sizeof(ControlBlock), PROT_READ | PROT_WRITE, MAP_SHARED, control, 0) : MAP_FAILED;
        if (view == MAP_FAILED) {
            close(control);
            close(messages[1]);
            return fail("sharing the control block");
        }
        control_ = static_cast<ControlBlock*>(view);

        // The host gets its end of the socket and the control block as descriptors 3 and 4 (dup2
        // leaves them open across exec).
        posix_spawn_file_actions_t actions;
        posix_spawn_file_actions_init(&actions);
        posix_spawn_file_actions_adddup2(&actions, messages[1], 3);
        posix_spawn_file_actions_adddup2(&actions, control, 4);
        // Nothing else of Brack's: not its standard input and output (a plugin's printf must not
        // land in brack-cli's), and not an application's descriptors. Errors still go to stderr.
        posix_spawn_file_actions_addopen(&actions, 0, "/dev/null", O_RDONLY, 0);
        posix_spawn_file_actions_addopen(&actions, 1, "/dev/null", O_WRONLY, 0);
#if defined(__GLIBC__) && (__GLIBC__ > 2 || __GLIBC_MINOR__ >= 34)
        posix_spawn_file_actions_addclosefrom_np(&actions, 5);
#endif
#ifdef __APPLE__
        posix_spawn_file_actions_addinherit_np(&actions, 2);  // the rest close (POSIX_SPAWN_CLOEXEC_DEFAULT)
#endif
        std::vector<std::string> args = launcher;
        args.push_back(exe.string());
        for (const std::string& a : {std::string("--brack-host"), std::to_string(remote::kProtocolVersion),
                                     std::string("3"), std::string("4")})
            args.push_back(a);
        std::vector<char*> argv;
        for (auto& a : args) argv.push_back(a.data());
        argv.push_back(nullptr);
        // No signals blocked, whichever thread started it (onLastingThread() blocks them all).
        posix_spawnattr_t attr;
        posix_spawnattr_init(&attr);
        sigset_t none;
        sigemptyset(&none);
        posix_spawnattr_setsigmask(&attr, &none);
#ifdef __APPLE__
        posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_CLOEXEC_DEFAULT);
#else
        posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETSIGMASK);
#endif
        const int spawned =
            onLastingThread([&] { return posix_spawn(&pid_, argv[0], &actions, &attr, argv.data(), environ); });
        posix_spawnattr_destroy(&attr);
        posix_spawn_file_actions_destroy(&actions);
        close(messages[1]);  // the host holds the only other end now: it closes when the host ends
        close(control);
        if (spawned != 0) {
            errno = spawned;
            pid_ = -1;
            return fail("posix_spawn");
        }
        return true;
    }

    void note(const Message& m) {
        if (m.value("note", "") == "audioThread") {
#ifdef __linux__
            realtime::makeRealtime(pid_, m.value("thread", (pid_t)0), "plugin hosts' audio threads");
#else
            realtime::report("plugin hosts' audio threads", m.value("error", ""));
#endif
            return;
        }
        if (onNote) onNote(m);
    }

    // Whether the process has ended, collecting its status once (`block`: wait for it).
    bool reap(bool block) {
        if (reaped_ || pid_ <= 0) return true;
        pid_t r;
        do r = waitpid(pid_, &status_, block ? 0 : WNOHANG);
        while (r < 0 && errno == EINTR);
        if (r == pid_ || (r < 0 && errno == ECHILD)) reaped_ = true;
        return reaped_;
    }

    // Whether the process has ended, leaving it to reap() to collect: any thread.
    bool ended() const {
        if (reaped_ || pid_ <= 0) return true;
        siginfo_t info{};
        if (waitid(P_PID, (id_t)pid_, &info, WEXITED | WNOHANG | WNOWAIT) != 0) return errno == ECHILD;
        return info.si_pid == pid_;
    }

    Outcome readMessage(Message& m, Clock::time_point until) {
        uint8_t len[4];
        Outcome o = io(false, len, 4, until);
        if (o != Outcome::Ok) return o;
        const uint32_t n = len[0] | len[1] << 8 | len[2] << 16 | (uint32_t)len[3] << 24;
        if (n > remote::kMaxMessageBytes) {
            terminate();  // not something a host of this version would send
            return Outcome::Ended;
        }
        std::vector<uint8_t> body(n);
        o = io(false, body.data(), body.size(), until);
        if (o != Outcome::Ok) return o;
        if (!remote::decodeMessage(body, m)) {
            terminate();
            return Outcome::Ended;
        }
        return Outcome::Ok;
    }

    Outcome io(bool write, uint8_t* data, size_t n, Clock::time_point until) {
        while (n > 0) {
            const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(until - Clock::now()).count();
            pollfd p{messages_, (short)(write ? POLLOUT : POLLIN), 0};
            const int ready = poll(&p, 1, (int)std::max<long long>(left, 0));
            if (ready == 0) return Outcome::TimedOut;
            if (ready < 0) {
                if (errno == EINTR) continue;
                return Outcome::Ended;
            }
            const ssize_t done = write ? send(messages_, data, n, kNoSignal) : receive(data, n);
            if (done < 0 && errno == EINTR) continue;
            if (done <= 0) return Outcome::Ended;
            data += done;
            n -= (size_t)done;
        }
        return Outcome::Ok;
    }

    // Reads, keeping the descriptors that come along (macOS: the outputs, SCM_RIGHTS).
    ssize_t receive(uint8_t* data, size_t n) {
#ifdef __APPLE__
        iovec iov{data, n};
        alignas(cmsghdr) char control[CMSG_SPACE(4 * sizeof(int))];
        msghdr msg{};
        msg.msg_iov = &iov;
        msg.msg_iovlen = 1;
        msg.msg_control = control;
        msg.msg_controllen = sizeof control;
        const ssize_t done = recvmsg(messages_, &msg, 0);
        for (cmsghdr* c = done > 0 ? CMSG_FIRSTHDR(&msg) : nullptr; c; c = CMSG_NXTHDR(&msg, c)) {
            if (c->cmsg_level != SOL_SOCKET || c->cmsg_type != SCM_RIGHTS) continue;
            const size_t count = (c->cmsg_len - CMSG_LEN(0)) / sizeof(int);
            for (size_t i = 0; i < count; ++i) {
                int fd;
                std::memcpy(&fd, CMSG_DATA(c) + i * sizeof(int), sizeof fd);
                closeOnExec(fd);
                received_.push_back(fd);
            }
        }
        return done;
#else
        return recv(messages_, data, n, 0);
#endif
    }

    std::deque<int> received_;  // macOS: descriptors from the host, oldest first
    pid_t pid_ = -1;
    int status_ = 0;
    std::atomic<bool> reaped_{false};  // host thread writes; terminate() reads it on the audio thread
    int messages_ = -1;
    ControlBlock* control_ = nullptr;
    void* outputs_ = nullptr;
    size_t outputsBytes_ = 0;
};

}  // namespace

std::unique_ptr<HostProcess> HostProcess::start(const std::string& architecture, std::string& error) {
    return PosixHostProcess::create(architecture, error);
}

}  // namespace brack
