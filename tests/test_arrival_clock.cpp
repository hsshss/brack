// Messages without a time are due a fixed way after they arrived, wherever in a period that was.
#include <algorithm>
#include <cstdio>
#include <vector>

#include "util/arrival_clock.h"
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
constexpr uint32_t kRate = 48000, kPeriod = 480;  // 10 ms periods

// A device whose period k starts at k * 10 ms (its clock `scale` times the steady clock's),
// called back up to 0.5 ms late.
struct Device {
    double scale = 1;
    int64_t stalledFrom = INT64_MAX, stalledFor = 0;  // callbacks stop, then go on where they were
    int64_t lateFrom = INT64_MAX, lateUntil = 0, lateBy = 0;  // callbacks late, then on time again

    int64_t callbackNs(int64_t k) const {
        int64_t ns = (int64_t)(k * 10 * kMs * scale) + (k * 37 % 11) * kMs / 20;
        if (ns >= lateFrom && ns < lateUntil) ns += lateBy;
        return ns < stalledFrom ? ns : ns + stalledFor;
    }
};

struct Landing {
    int64_t arrived;
    uint64_t due, nextPeriod;  // the position of the first period rendered after it arrived
};

// Messages every 7.3 ms, out of step with the periods, each placed as it is taken.
std::vector<Landing> play(ArrivalClock& clock, const Device& device, int64_t untilNs) {
    std::vector<Landing> out;
    int64_t k = 0;
    for (int64_t t = 0; t < untilNs; t += 7300 * 1000) {
        for (; device.callbackNs(k) <= t; ++k) clock.rendering((uint64_t)k * kPeriod, device.callbackNs(k), 40 * kMs);
        if (k > 0) out.push_back({t, clock.due(t), (uint64_t)k * kPeriod});
    }
    return out;
}

// How far after its arrival each message is due, in frames of the device's clock.
std::vector<double> leads(const std::vector<Landing>& landings, int64_t fromNs, double scale = 1) {
    std::vector<double> out;
    for (auto& l : landings)
        if (l.arrived >= fromNs) out.push_back((double)l.due - l.arrived / scale * kRate / 1e9);
    return out;
}

bool noneLate(const std::vector<Landing>& landings, int64_t fromNs) {
    return std::all_of(landings.begin(), landings.end(),
                       [&](const Landing& l) { return l.arrived < fromNs || l.due >= l.nextPeriod; });
}

double spread(const std::vector<double>& v) { return *std::max_element(v.begin(), v.end()) - *std::min_element(v.begin(), v.end()); }

}  // namespace

int main() {
    {
        ArrivalClock clock;
        clock.reset(kRate, kPeriod, 0);
        CHECK(clock.due(5 * kMs) == ArrivalClock::kUnknown);  // nothing rendered yet: at once
    }
    {
        // Wherever in a period a message arrives, it is due a period after, to the frame, and
        // never in a period already rendered when it arrived.
        ArrivalClock clock;
        clock.reset(kRate, kPeriod, 0);
        auto landed = play(clock, Device{}, 10'000 * kMs);
        auto lead = leads(landed, 500 * kMs);
        CHECK(spread(lead) <= 1);
        CHECK(*std::min_element(lead.begin(), lead.end()) >= kPeriod);
        CHECK(noneLate(landed, 0));
    }
    for (double scale : {1 - 100e-6, 1 + 100e-6}) {
        // A device clock 100 ppm off: followed within what two windows drift, and never late.
        ArrivalClock clock;
        clock.reset(kRate, kPeriod, 0);
        auto landed = play(clock, Device{scale}, 60'000 * kMs);
        auto lead = leads(landed, 500 * kMs, scale);
        CHECK(spread(lead) <= 1 + 8 * kRate * 100e-6);
        CHECK(noneLate(landed, 0));
    }
    {
        // A device that stops for half a second and goes on where it was is found afresh.
        ArrivalClock clock;
        clock.reset(kRate, kPeriod, 0);
        Device stalling;
        stalling.stalledFrom = 1000 * kMs;
        stalling.stalledFor = 500 * kMs;
        auto landed = play(clock, stalling, 4000 * kMs);
        auto lead = leads(landed, 2000 * kMs);
        for (double& l : lead) l += 0.5 * kRate;  // half a second behind the steady clock now
        CHECK(spread(lead) <= 1);
        CHECK(*std::min_element(lead.begin(), lead.end()) >= kPeriod);
        CHECK(noneLate(landed, 1600 * kMs));
    }
    for (int64_t lost : {11'500'000ll, 3'000'000ll}) {
        // A device that loses some time and goes on (a Bluetooth output loses 11.5 ms soon after
        // it starts): less than a stall, but the clock follows within one, not when the windows
        // age out. Also when that is less than half a period.
        ArrivalClock clock;
        clock.reset(kRate, kPeriod, 0);
        Device skipping;
        skipping.stalledFrom = 574 * kMs;
        skipping.stalledFor = lost;
        auto landed = play(clock, skipping, 4000 * kMs);
        auto lead = leads(landed, 700 * kMs);
        for (double& l : lead) l += lost * kRate / 1e9;  // that much behind the steady clock now
        CHECK(*std::min_element(lead.begin(), lead.end()) >= kPeriod);
        CHECK(*std::max_element(lead.begin(), lead.end()) <= kPeriod + 1);
        CHECK(noneLate(landed, 0));
    }
    {
        // Callbacks 8 ms late for 30 ms, then on time again: no fall, so the clock stays.
        ArrivalClock clock;
        clock.reset(kRate, kPeriod, 0);
        Device late;
        late.lateFrom = 1000 * kMs;
        late.lateUntil = 1030 * kMs;
        late.lateBy = 8 * kMs;
        auto lead = leads(play(clock, late, 3000 * kMs), 500 * kMs);
        CHECK(spread(lead) <= 1);
    }
    {
        // A new device at another rate, the position going on from 10 s at 48 kHz.
        ArrivalClock clock;
        clock.reset(kRate, kPeriod, 0);
        play(clock, Device{}, 10'000 * kMs);
        const uint64_t from = 10ull * kRate;
        const int64_t start = 10'100 * kMs;
        clock.reset(192000, 1920, start);
        std::vector<double> lead;
        for (int64_t k = 0, t = start; t < start + 3000 * kMs; t += 7300 * 1000) {
            for (; start + k * 10 * kMs <= t; ++k) clock.rendering(from + (uint64_t)k * 1920, start + k * 10 * kMs, 40 * kMs);
            lead.push_back((double)clock.due(t) - from - (t - start) * 192000 / 1e9);
        }
        CHECK(spread(lead) <= 1);
        CHECK(*std::min_element(lead.begin(), lead.end()) >= 1920);
    }
    {
        // A sound server that calls back 8 times in a row for 512 frames each, then not for the
        // rest of its 4096-frame cycle (85 ms): messages keep their place in the cycle, and
        // none lands in a period already rendered when it arrived.
        ArrivalClock clock;
        clock.reset(kRate, 512, 0);
        CallbackCadence cadence;
        auto callbackNs = [](int64_t k) { return (k / 8) * 4096 * 1'000'000'000 / kRate + (k % 8) * 50'000 + (k / 8 * 37 % 11) * kMs / 20; };
        std::vector<Landing> landed;
        int64_t k = 0;
        for (int64_t t = 0; t < 10'000 * kMs; t += 7300 * 1000) {
            for (; callbackNs(k) <= t; ++k) {
                cadence.callback(callbackNs(k));
                clock.rendering((uint64_t)k * 512, callbackNs(k),
                                CallbackCadence::stallNs(512 * 1'000'000'000ll / kRate, cadence.longestGapNs()));
            }
            if (k > 0) landed.push_back({t, clock.due(t), (uint64_t)k * 512});
        }
        auto lead = leads(landed, 500 * kMs);
        CHECK(spread(lead) <= 512 + 1);  // a burst's 512-frame steps, seen from between them
        CHECK(noneLate(landed, 500 * kMs));
    }
    {
        // The device stops for half a second and comes back where it was, so the clock is found
        // afresh, lower. Messages keep their order across that: a note-off placed before its
        // note-on would leave the note sounding.
        ArrivalClock clock;
        clock.reset(kRate, kPeriod, 0);
        Device stalling;
        stalling.stalledFrom = 1000 * kMs;
        stalling.stalledFor = 500 * kMs;
        auto landed = play(clock, stalling, 4000 * kMs);
        bool inOrder = true;
        for (size_t i = 1; i < landed.size(); ++i) inOrder = inOrder && landed[i].due >= landed[i - 1].due;
        CHECK(inOrder);
    }
    {
        // The device is opened again (it stopped, and Brack follows the default): render
        // positions go on, and the new device's first callback reads its clock lower. A message
        // placed before is still waiting; the next one is not placed before it.
        ArrivalClock clock;
        clock.reset(kRate, kPeriod, 0);
        play(clock, Device{}, 1000 * kMs);
        const uint64_t before = clock.due(1000 * kMs);
        clock.reset(kRate, kPeriod, 1010 * kMs);
        clock.rendering(48000, 1050 * kMs, 40 * kMs);
        CHECK(clock.due(1011 * kMs) >= before);
    }
    {
        // A message for an earlier moment than the last (sent ahead for one, say) is not held
        // back by it, and rendering begun again from 0 has no order to keep.
        ArrivalClock clock;
        clock.reset(kRate, kPeriod, 0);
        play(clock, Device{}, 1000 * kMs);
        const uint64_t ahead = clock.due(1500 * kMs);
        const uint64_t now = clock.due(1000 * kMs);
        CHECK(now < ahead);
        clock.reset(kRate, kPeriod, 2000 * kMs);
        clock.forgetOrder();
        clock.rendering(0, 2000 * kMs, 40 * kMs);
        CHECK(clock.due(2000 * kMs) < ahead);
    }
    if (g_failures) std::fprintf(stderr, "%d failure(s)\n", g_failures);
    else std::printf("arrival clock: ok\n");
    return g_failures ? 1 : 0;
}
