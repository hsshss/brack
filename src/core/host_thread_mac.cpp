// macOS: the host thread is the process's main thread, through the main dispatch queue. Plugins
// there take their "main thread" to be AppKit's: their editors, timers and messages run on its run
// loop, which the process keeps running (util/main_thread.h). Timers and watched file descriptors
// are dispatch sources on the same queue.
#include "host_thread.h"

#include <dispatch/dispatch.h>
#include <poll.h>
#include <pthread.h>
#include <sys/ioctl.h>

#include <atomic>
#include <chrono>
#include <deque>
#include <exception>
#include <future>
#include <map>
#include <mutex>
#include <vector>

#include "util/common.h"

namespace brack {

struct HostThread::Impl {
    struct State {
        std::mutex mutex;
        std::deque<std::function<void()>> queue;
        bool drainScheduled = false;
        bool quit = false;

        void drain() {
            std::unique_lock lock(mutex);
            drainScheduled = false;
            while (!queue.empty()) {
                auto fn = std::move(queue.front());
                queue.pop_front();
                lock.unlock();
                fn();
                lock.lock();
            }
        }
    };
    struct Source {
        std::shared_ptr<State> state;
        std::function<void()> fn;
        int fd = -1;
        uint32_t events = 0;
        std::function<void(uint32_t)> onFd;
    };

    std::shared_ptr<State> state = std::make_shared<State>();
    std::map<uint32_t, dispatch_source_t> timers;
    struct Watch {
        int fd;
        std::function<void(uint32_t)> fn;
        std::vector<dispatch_source_t> sources;
    };
    std::map<uint32_t, Watch> watches;
    uint32_t nextId = 1;

    static void drainOnMain(void* context) {
        auto* held = static_cast<std::shared_ptr<State>*>(context);
        (*held)->drain();
        delete held;
    }

    void schedule() {
        dispatch_async_f(dispatch_get_main_queue(), new std::shared_ptr<State>(state), &drainOnMain);
    }

    // A handler may remove its own source: libdispatch runs the cancel handler, which frees it, after.
    static void fire(void* context) {
        auto* s = static_cast<Source*>(context);
        s->fn();
    }

    static void fireFd(void* context) {
        auto* s = static_cast<Source*>(context);
        pollfd p{s->fd, (short)(((s->events & FdRead) ? POLLIN : 0) | ((s->events & FdWrite) ? POLLOUT : 0)), 0};
        if (poll(&p, 1, 0) <= 0) return;
        // At the end, with nothing left to read, macOS has POLLIN with POLLHUP; Linux only POLLHUP.
        int unread = 0;
        if ((p.revents & (POLLIN | POLLHUP)) == (POLLIN | POLLHUP) && ioctl(s->fd, FIONREAD, &unread) == 0 && unread == 0)
            p.revents &= ~POLLIN;
        uint32_t happened = 0;
        if (p.revents & POLLIN) happened |= FdRead;
        if (p.revents & POLLOUT) happened |= FdWrite;
        if (p.revents & (POLLERR | POLLHUP | POLLNVAL)) happened |= FdError;
        if (!happened) return;
        s->onFd(happened);
    }

    static void release(void* context) { delete static_cast<Source*>(context); }

    static dispatch_source_t start(dispatch_source_type_t type, uintptr_t handle, Source* s, dispatch_function_t fn) {
        dispatch_source_t src = dispatch_source_create(type, handle, 0, dispatch_get_main_queue());
        dispatch_set_context(src, s);
        dispatch_source_set_event_handler_f(src, fn);
        dispatch_source_set_cancel_handler_f(src, &release);
        return src;
    }

    void watch(Watch& w, uint32_t events) {
        for (dispatch_source_t src : w.sources) stop(src);
        w.sources.clear();
        for (uint32_t event : {(uint32_t)FdRead, (uint32_t)FdWrite}) {
            if (!(events & event)) continue;
            auto* s = new Source{state, {}, w.fd, events, w.fn};
            dispatch_source_t src = start(event == FdRead ? DISPATCH_SOURCE_TYPE_READ : DISPATCH_SOURCE_TYPE_WRITE,
                                          (uintptr_t)w.fd, s, &fireFd);
            dispatch_activate(src);
            w.sources.push_back(src);
        }
    }

    static void stop(dispatch_source_t src) {
        dispatch_source_cancel(src);
        dispatch_release(src);
    }
};

HostThread::HostThread() : impl_(std::make_unique<Impl>()) {}

HostThread::~HostThread() {
    invoke([] {});
    std::lock_guard lock(impl_->state->mutex);
    impl_->state->quit = true;
    for (auto& [id, src] : impl_->timers) Impl::stop(src);
    for (auto& [id, w] : impl_->watches)
        for (dispatch_source_t src : w.sources) Impl::stop(src);
}

bool HostThread::isCurrent() const { return pthread_main_np() != 0; }

void HostThread::post(std::function<void()> fn) {
    std::lock_guard lock(impl_->state->mutex);
    if (impl_->state->quit) return;
    impl_->state->queue.push_back(std::move(fn));
    if (!impl_->state->drainScheduled) {
        impl_->state->drainScheduled = true;
        impl_->schedule();
    }
}

void HostThread::invoke(const std::function<void()>& fn) {
    if (isCurrent()) {
        fn();
        return;
    }
    std::promise<void> done;
    std::atomic<bool> started{false};
    post([&] {
        started = true;
        try {
            fn();
            done.set_value();
        } catch (...) {
            done.set_exception(std::current_exception());
        }
    });
    std::future<void> result = done.get_future();
    // Not even started after a while: the application's main thread is not running its run loop,
    // which nothing else here can tell it.
    if (result.wait_for(std::chrono::seconds(10)) == std::future_status::timeout && !started) {
        static std::atomic<bool> warned{false};
        if (!warned.exchange(true))
            logWarn("waiting for the main thread: on macOS Brack works on it, and its run loop has not run for 10 s "
                    "(the application keeps it running: brack.h)");
    }
    result.get();
}

uint32_t HostThread::addTimer(uint32_t periodMs, std::function<void()> fn) {
    std::lock_guard lock(impl_->state->mutex);
    const uint32_t id = impl_->nextId++;
    auto* s = new Impl::Source{impl_->state, std::move(fn), -1, 0, {}};
    dispatch_source_t src = Impl::start(DISPATCH_SOURCE_TYPE_TIMER, 0, s, &Impl::fire);
    const uint64_t period = (uint64_t)periodMs * NSEC_PER_MSEC;
    dispatch_source_set_timer(src, dispatch_time(DISPATCH_TIME_NOW, (int64_t)period), period, period / 10);
    dispatch_activate(src);
    impl_->timers[id] = src;
    return id;
}

void HostThread::removeTimer(uint32_t id) {
    std::lock_guard lock(impl_->state->mutex);
    if (auto it = impl_->timers.find(id); it != impl_->timers.end()) {
        Impl::stop(it->second);
        impl_->timers.erase(it);
    }
}

uint32_t HostThread::watchFd(int fd, uint32_t events, std::function<void(uint32_t)> fn) {
    std::lock_guard lock(impl_->state->mutex);
    const uint32_t id = impl_->nextId++;
    impl_->watch(impl_->watches[id] = {fd, std::move(fn), {}}, events);
    return id;
}

void HostThread::changeFd(uint32_t id, uint32_t events) {
    std::lock_guard lock(impl_->state->mutex);
    if (auto it = impl_->watches.find(id); it != impl_->watches.end()) impl_->watch(it->second, events);
}

void HostThread::unwatchFd(uint32_t id) {
    std::lock_guard lock(impl_->state->mutex);
    if (auto it = impl_->watches.find(id); it != impl_->watches.end()) {
        impl_->watch(it->second, 0);
        impl_->watches.erase(it);
    }
}

}  // namespace brack
