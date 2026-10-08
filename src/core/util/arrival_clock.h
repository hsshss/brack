#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>

namespace brack {

// Turns the moment a MIDI message is for (its arrival, or a time it was sent for) into the render
// position it is due at, a fixed way after that moment, so where in a device period it falls is
// kept. Messages are taken only at period starts and would otherwise all land there.
//
// The device's clock is the highest reading (render position less steady-clock frames) at render
// starts over this window and the last: a late callback reads low, and windows follow drift from
// the steady clock. A reading further below than a stall (CallbackCadence) means the device
// stopped: the clock is found afresh. Readings more than 1 ms low for as long as a stall mean the
// device lost less time than a stall (a Bluetooth output loses 11 ms soon after it starts, whatever its
// period), as late callbacks would have caught up: the clock falls to their best at once, not
// when the windows age out up to 8 s on.
//
// When the clock falls, a message for a moment no earlier than the last one placed is still
// placed no earlier than it, so order is kept (a note-off before its note-on would hang the note).
// Reasons and measurements: design notes, ch. 12. Audio thread only, but for reset() and
// forgetOrder().
class ArrivalClock {
public:
    static constexpr uint64_t kUnknown = std::numeric_limits<uint64_t>::max();

    // While nothing renders. `latencyFrames`: how far after its arrival a message is due, at
    // least a period, for it to arrive before that period is rendered. The order kept stays: a
    // device opened again renders on from where the last one stopped, and messages placed for
    // the last one may still be waiting.
    void reset(uint32_t rate, uint32_t latencyFrames, int64_t nowNs) {
        framesPerNs_ = rate / 1e9;
        latency_ = latencyFrames;
        originNs_ = windowStartNs_ = nowNs;
        current_ = previous_ = -std::numeric_limits<double>::infinity();
        lowSinceNs_ = kNotLow;
    }

    // When render positions begin again (from 0): the order kept so far means nothing then.
    void forgetOrder() {
        lastWhenNs_ = std::numeric_limits<int64_t>::min();
        lastDue_ = 0;
    }

    // At the start of each render, which begins at `position`, with how long a silence means
    // the device stopped.
    void rendering(uint64_t position, int64_t nowNs, int64_t stallNs) {
        const double reading = (double)position - (nowNs - originNs_) * framesPerNs_;
        if (reading < clock() - stallNs * framesPerNs_) {
            current_ = previous_ = -std::numeric_limits<double>::infinity();
            windowStartNs_ = nowNs;
            lowSinceNs_ = kNotLow;
        } else if (reading < clock() - kFallNs * framesPerNs_) {
            if (lowSinceNs_ == kNotLow) {
                lowSinceNs_ = nowNs;
                lowBest_ = reading;
            }
            lowBest_ = std::max(lowBest_, reading);
            if (nowNs - lowSinceNs_ >= stallNs) {
                current_ = lowBest_;
                previous_ = -std::numeric_limits<double>::infinity();
                windowStartNs_ = nowNs;
                lowSinceNs_ = kNotLow;
            }
        } else {
            lowSinceNs_ = kNotLow;
        }
        if (nowNs - windowStartNs_ >= kWindowNs) {
            previous_ = current_;
            current_ = reading;
            windowStartNs_ = nowNs;
        } else {
            current_ = std::max(current_, reading);
        }
    }

    // kUnknown before the first render: as soon as possible.
    uint64_t due(int64_t whenNs) {
        const double c = clock();
        if (c == -std::numeric_limits<double>::infinity()) return kUnknown;
        uint64_t due = (uint64_t)std::max(0.0, std::ceil(c + (whenNs - originNs_) * framesPerNs_ + latency_));
        if (whenNs >= lastWhenNs_) {
            due = std::max(due, lastDue_);
            lastWhenNs_ = whenNs;
            lastDue_ = due;
        }
        return due;
    }

private:
    static constexpr int64_t kWindowNs = 4'000'000'000;
    static constexpr int64_t kFallNs = 1'000'000;
    static constexpr int64_t kNotLow = std::numeric_limits<int64_t>::min();

    double clock() const { return std::max(current_, previous_); }

    double framesPerNs_ = 0;
    uint32_t latency_ = 0;
    int64_t originNs_ = 0, windowStartNs_ = 0;
    double current_ = -std::numeric_limits<double>::infinity(), previous_ = current_;
    int64_t lowSinceNs_ = kNotLow;  // since when the readings have stayed well below the clock
    double lowBest_ = 0;            // and the best of them
    int64_t lastWhenNs_ = std::numeric_limits<int64_t>::min();  // the latest moment placed, and where
    uint64_t lastDue_ = 0;
};

}  // namespace brack
