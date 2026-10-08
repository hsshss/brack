#pragma once
#include <algorithm>
#include <cstdint>

namespace brack {

// How long a device goes between callbacks, and so how long a silence means it has stopped. A
// sound server may call back several times in a row and then not for the rest of its cycle
// (PipeWire with a large quantum, PulseAudio in a VM): a few of the device's periods are then no
// sign of a stop. The longest gap of this window and the last (4 s each) is the device's cadence;
// a gap of kStopNs or more is a stop, not cadence, and is left out, so that a device that comes
// back after one is not taken to call back that seldom. Audio thread only, but for reset().
class CallbackCadence {
public:
    static constexpr int64_t kStopNs = 250'000'000;

    void reset() { lastNs_ = current_ = previous_ = 0; }

    // At the start of each device callback.
    void callback(int64_t nowNs) {
        if (lastNs_ != 0) {
            const int64_t gap = nowNs - lastNs_;
            if (nowNs - windowStartNs_ >= kWindowNs) {
                previous_ = current_;
                current_ = 0;
                windowStartNs_ = nowNs;
            }
            if (gap < kStopNs) current_ = std::max(current_, gap);
        } else {
            windowStartNs_ = nowNs;
        }
        lastNs_ = nowNs;
    }

    int64_t longestGapNs() const { return std::max(current_, previous_); }

    // How long without a callback means the device stopped, for a device asked for `periodNs`
    // periods that has called back at most `longestGapNs` apart lately.
    static int64_t stallNs(int64_t periodNs, int64_t longestGapNs) {
        return std::max({4 * periodNs, kMinStallNs, 2 * longestGapNs});
    }

private:
    static constexpr int64_t kWindowNs = 4'000'000'000;
    static constexpr int64_t kMinStallNs = 40'000'000;

    int64_t lastNs_ = 0, windowStartNs_ = 0;
    int64_t current_ = 0, previous_ = 0;
};

}  // namespace brack
