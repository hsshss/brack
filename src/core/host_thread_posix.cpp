// Linux: the host thread as a loop that waits in poll() for queued work (a pipe wakes
// it), the next timer, and the file descriptors watched on it: X11 connections, and plugins' own
// (CLAP posix-fd-support, VST3 IRunLoop).
#include "host_thread.h"

#include <fcntl.h>
#include <poll.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <deque>
#include <exception>
#include <future>
#include <map>
#include <mutex>
#include <thread>
#include <vector>

namespace brack {

using Clock = std::chrono::steady_clock;

namespace {
thread_local HostThread* t_current = nullptr;
}  // namespace

struct HostThread::Impl {
    struct Timer {
        std::chrono::milliseconds period;
        Clock::time_point next;
        std::function<void()> fn;
    };
    struct Watch {
        int fd;
        uint32_t events;
        std::function<void(uint32_t)> fn;
    };

    std::mutex mutex;  // the queue and quit; timers and watches are the host thread's
    std::deque<std::function<void()>> queue;
    bool quit = false;
    int wakeRead = -1, wakeWrite = -1;
    std::map<uint32_t, Timer> timers;
    uint32_t nextTimerId = 1;
    std::map<uint32_t, Watch> watches;
    uint32_t nextWatchId = 1;
    std::thread thread;
    std::thread::id id;

    Impl() {
        int fds[2];
        if (pipe(fds) == 0) {
            for (int fd : fds) {
                fcntl(fd, F_SETFD, FD_CLOEXEC);
                fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
            }
            wakeRead = fds[0];
            wakeWrite = fds[1];
        }
    }
    ~Impl() {
        for (int fd : {wakeRead, wakeWrite})
            if (fd >= 0) close(fd);
    }

    void wake() {
        const char c = 0;
        while (write(wakeWrite, &c, 1) < 0 && errno == EINTR) {
        }
    }

    // Runs everything queued so far. False once told to quit.
    bool drainQueue() {
        std::unique_lock lock(mutex);
        while (!queue.empty()) {
            auto fn = std::move(queue.front());
            queue.pop_front();
            lock.unlock();
            fn();
            lock.lock();
        }
        return !quit;
    }

    // Runs the most overdue timer, if one is due (one at a time, so that a slow timer cannot
    // starve the others), and says how long until the next.
    int runTimer() {
        const auto now = Clock::now();
        auto due = timers.end();
        for (auto it = timers.begin(); it != timers.end(); ++it)
            if (it->second.next <= now && (due == timers.end() || it->second.next < due->second.next)) due = it;
        if (due != timers.end()) {
            due->second.next = now + due->second.period;
            auto fn = due->second.fn;  // a copy: fn may remove its own timer
            fn();
            return 0;
        }
        Clock::time_point soonest = now + std::chrono::hours(1);
        for (auto& [timerId, t] : timers) soonest = std::min(soonest, t.next);
        return (int)std::chrono::ceil<std::chrono::milliseconds>(soonest - now).count();
    }

    static short pollEvents(uint32_t events) {
        return (short)(((events & FdRead) ? POLLIN : 0) | ((events & FdWrite) ? POLLOUT : 0));
    }

    void run() {
        std::vector<pollfd> fds;
        std::vector<uint32_t> ids;
        while (drainQueue()) {
            const int timeout = runTimer();
            if (timeout == 0) continue;
            fds.assign(1, pollfd{wakeRead, POLLIN, 0});
            ids.assign(1, 0);
            for (auto& [watchId, w] : watches) {
                fds.push_back({w.fd, pollEvents(w.events), 0});
                ids.push_back(watchId);
            }
            if (poll(fds.data(), (nfds_t)fds.size(), timeout) <= 0) continue;
            if (fds[0].revents) {
                char buf[64];
                while (read(wakeRead, buf, sizeof buf) > 0) {
                }
            }
            for (size_t i = 1; i < fds.size(); ++i) {
                const short r = fds[i].revents;
                if (!r) continue;
                auto it = watches.find(ids[i]);  // an earlier callback may have removed it
                if (it == watches.end()) continue;
                uint32_t happened = 0;
                if (r & POLLIN) happened |= FdRead;
                if (r & POLLOUT) happened |= FdWrite;
                if (r & (POLLERR | POLLHUP | POLLNVAL)) happened |= FdError;
                auto fn = it->second.fn;  // a copy: fn may stop watching
                fn(happened);
            }
        }
        drainQueue();  // work queued before the end still runs, as on Windows
    }
};

HostThread::HostThread() : impl_(std::make_unique<Impl>()) {
    std::promise<void> ready;
    impl_->thread = std::thread([this, &ready] {
        impl_->id = std::this_thread::get_id();
        t_current = this;
        ready.set_value();
        impl_->run();
    });
    ready.get_future().wait();
}

HostThread::~HostThread() {
    {
        std::lock_guard lock(impl_->mutex);
        impl_->quit = true;
    }
    impl_->wake();
    impl_->thread.join();
}

bool HostThread::isCurrent() const { return std::this_thread::get_id() == impl_->id; }

HostThread* HostThread::current() { return t_current; }

void HostThread::post(std::function<void()> fn) {
    {
        std::lock_guard lock(impl_->mutex);
        impl_->queue.push_back(std::move(fn));
    }
    impl_->wake();
}

void HostThread::invoke(const std::function<void()>& fn) {
    if (isCurrent()) {
        fn();
        return;
    }
    std::promise<void> done;
    post([&] {
        try {
            fn();
            done.set_value();
        } catch (...) {
            done.set_exception(std::current_exception());
        }
    });
    done.get_future().get();
}

uint32_t HostThread::addTimer(uint32_t periodMs, std::function<void()> fn) {
    uint32_t id = 0;
    invoke([&] {
        id = impl_->nextTimerId++;
        impl_->timers[id] = {std::chrono::milliseconds(periodMs), Clock::now() + std::chrono::milliseconds(periodMs),
                             std::move(fn)};
    });
    return id;
}

void HostThread::removeTimer(uint32_t id) {
    invoke([&] { impl_->timers.erase(id); });
}

uint32_t HostThread::watchFd(int fd, uint32_t events, std::function<void(uint32_t)> fn) {
    uint32_t id = 0;
    invoke([&] {
        id = impl_->nextWatchId++;
        impl_->watches[id] = {fd, events, std::move(fn)};
    });
    return id;
}

void HostThread::changeFd(uint32_t id, uint32_t events) {
    invoke([&] {
        if (auto it = impl_->watches.find(id); it != impl_->watches.end()) it->second.events = events;
    });
}

void HostThread::unwatchFd(uint32_t id) {
    invoke([&] { impl_->watches.erase(id); });
}

}  // namespace brack
