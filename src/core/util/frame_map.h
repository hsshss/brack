#pragma once
#include <cstdint>

namespace brack {

// Render positions (output frames, the clock MIDI times refer to) to process frames. The
// output's rate changes while the engine runs when it moves to a device at another rate; the
// positions from then on count at the new rate, from where the process frames had got to.
class FrameMap {
public:
    void setRates(uint64_t renderPos, uint32_t outRate, uint32_t procRate) {
        processBase_ = processFrame(renderPos);
        renderBase_ = renderPos;
        outRate_ = outRate;
        procRate_ = procRate;
    }

    // A position from before the last change maps to where the change began: already due.
    uint64_t processFrame(uint64_t renderPos) const {
        if (renderPos <= renderBase_) return processBase_;
        const uint64_t d = renderPos - renderBase_;
        return processBase_ + (outRate_ == procRate_ ? d : (uint64_t)((long double)d * procRate_ / outRate_));
    }

private:
    uint64_t renderBase_ = 0, processBase_ = 0;
    uint32_t outRate_ = 0, procRate_ = 0;
};

}  // namespace brack
