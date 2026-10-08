// The host thread: work run on it, timers, and (macOS and Linux) file descriptors watched on it.
#include <atomic>
#include <chrono>
#include <cstdio>
#include <stdexcept>
#include <thread>

#ifndef _WIN32
#include <unistd.h>
#endif

#include "host_thread.h"
#include "util/main_thread.h"

using namespace brack;

namespace {

int g_failures = 0;
#define CHECK(cond)                                                                \
    do {                                                                           \
        if (!(cond)) {                                                             \
            std::fprintf(stderr, "CHECK failed: %s (line %d)\n", #cond, __LINE__); \
            ++g_failures;                                                          \
        }                                                                          \
    } while (0)

template <typename F>
bool waitFor(F done, int ms = 2000) {
    for (int i = 0; i < ms / 5 && !done(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    return done();
}

void invokes(HostThread& h) {
    bool onIt = false;
    h.invoke([&] { onIt = h.isCurrent(); });
    CHECK(onIt);
    CHECK(!h.isCurrent());
    bool thrown = false;
    try {
        h.invoke([] { throw std::runtime_error("x"); });
    } catch (const std::runtime_error&) {
        thrown = true;
    }
    CHECK(thrown);
    std::atomic<int> posted{0};
    for (int i = 0; i < 100; ++i) h.post([&] { ++posted; });
    CHECK(waitFor([&] { return posted == 100; }));
}

void timers(HostThread& h) {
    std::atomic<int> fast{0}, slow{0};
    const uint32_t a = h.addTimer(5, [&] { ++fast; });
    const uint32_t b = h.addTimer(50, [&] { ++slow; });
    CHECK(waitFor([&] { return fast >= 10 && slow >= 1; }));
    h.removeTimer(a);
    h.removeTimer(b);
    const int stopped = fast;
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    CHECK(fast == stopped);
}

#ifndef _WIN32
// What a plugin's X11 connection is to the loop: data to read, then the other end gone.
void watchesFds(HostThread& h) {
    int fds[2];
    CHECK(pipe(fds) == 0);
    std::atomic<int> reads{0}, errors{0};
    std::atomic<bool> onIt{true};
    uint32_t id = 0;
    id = h.watchFd(fds[0], HostThread::FdRead, [&](uint32_t events) {
        onIt = onIt && h.isCurrent();
        if (events & HostThread::FdRead) {
            char c;
            if (read(fds[0], &c, 1) == 1) ++reads;
        }
        if ((events & HostThread::FdError) && !(events & HostThread::FdRead)) {
            ++errors;
            h.unwatchFd(id);  // from its own callback
        }
    });
    CHECK(write(fds[1], "ab", 2) == 2);
    CHECK(waitFor([&] { return reads == 2; }));
    CHECK(onIt);

    // Not asked for: nothing to read is reported, so it is left alone.
    h.changeFd(id, 0);
    CHECK(write(fds[1], "c", 1) == 1);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    CHECK(reads == 2);
    h.changeFd(id, HostThread::FdRead);
    CHECK(waitFor([&] { return reads == 3; }));

    close(fds[1]);
    CHECK(waitFor([&] { return errors == 1; }));
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    CHECK(errors == 1);  // unwatched: no more
    close(fds[0]);
}
#endif

}  // namespace

int run() {
    std::atomic<bool> ran{false};
    {
        HostThread h;
        invokes(h);
        timers(h);
#ifndef _WIN32
        watchesFds(h);
#endif
        h.post([&] { ran = true; });
    }
    CHECK(ran);  // work queued before the end still runs
    if (g_failures) {
        std::fprintf(stderr, "%d check(s) failed\n", g_failures);
        return 1;
    }
    std::puts("all checks passed");
    return 0;
}

int main() { return brack::runWithMainLoop([] { return run(); }); }
