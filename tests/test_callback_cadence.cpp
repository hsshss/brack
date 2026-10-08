// How long a silence means a device stopped follows how it calls back.
#include <cstdio>

#include "util/callback_cadence.h"

using namespace brack;

namespace {

int g_failures = 0;
#define CHECK(cond)                                                       \
    do {                                                                  \
        if (!(cond)) {                                                    \
            std::fprintf(stderr, "CHECK failed: %s (line %d)\n", #cond, __LINE__); \
            ++g_failures;                                                 \
        }                                                                 \
    } while (0)

constexpr int64_t kMs = 1'000'000;
constexpr int64_t kPeriodNs = 512 * 1'000'000'000ll / 48000;  // 10.7 ms

// Calls back every 10.7 ms from `fromNs` for `forNs`; returns when it stopped.
int64_t regular(CallbackCadence& c, int64_t fromNs, int64_t forNs) {
    int64_t t = fromNs;
    for (; t < fromNs + forNs; t += kPeriodNs) c.callback(t);
    return t;
}

// Calls back 8 times in a row, then not for the rest of an 85 ms cycle.
int64_t bursts(CallbackCadence& c, int64_t fromNs, int64_t forNs) {
    int64_t t = fromNs;
    for (; t < fromNs + forNs; t += 8 * kPeriodNs)
        for (int i = 0; i < 8; ++i) c.callback(t + i * 50'000);
    return t;
}

int64_t stall(const CallbackCadence& c) { return CallbackCadence::stallNs(kPeriodNs, c.longestGapNs()); }

}  // namespace

int main() {
    {
        // Steady callbacks: a few periods, and no less than 40 ms.
        CallbackCadence c;
        regular(c, 0, 2000 * kMs);
        CHECK(c.longestGapNs() == kPeriodNs);
        CHECK(stall(c) == 4 * kPeriodNs);
    }
    {
        // Callbacks 85 ms apart are cadence, not stops.
        CallbackCadence c;
        bursts(c, 0, 2000 * kMs);
        CHECK(c.longestGapNs() > 85 * kMs - 8 * 50'000 && stall(c) > 2 * 80 * kMs);
    }
    {
        // A device that stops for 2 s and comes back is not taken to call back that seldom.
        CallbackCadence c;
        int64_t t = regular(c, 0, 1000 * kMs);
        regular(c, t + 2000 * kMs, 1000 * kMs);
        CHECK(c.longestGapNs() == kPeriodNs);
    }
    {
        // Long gaps are forgotten two windows (8 s) after they stop.
        CallbackCadence c;
        int64_t t = bursts(c, 0, 2000 * kMs);
        t = regular(c, t, 4000 * kMs);
        CHECK(stall(c) > 2 * 80 * kMs);  // still in the last window
        regular(c, t, 8000 * kMs);
        CHECK(stall(c) == 4 * kPeriodNs);
    }
    {
        // Starting over (another device) forgets the cadence.
        CallbackCadence c;
        bursts(c, 0, 2000 * kMs);
        c.reset();
        regular(c, 10'000 * kMs, 1000 * kMs);
        CHECK(c.longestGapNs() == kPeriodNs);
    }
    if (g_failures) std::fprintf(stderr, "%d failure(s)\n", g_failures);
    else std::printf("callback cadence: ok\n");
    return g_failures ? 1 : 0;
}
