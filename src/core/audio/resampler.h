#pragma once
// Multichannel streaming sample rate converter (r8brain-free-src, linear phase).
// Constructed off the audio thread; process() is real-time safe.

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace r8b {
class CDSPResampler;
}

namespace brack {

// All presets are linear phase and flat up to (at least) 20 kHz; they differ
// in stop-band attenuation and where the -3 dB point sits (wider transition
// band = shorter filter = less latency).
enum class ResamplerQuality {
    Standard,  // ~136 dB stop band, -3 dB at 20.5 kHz (16-bit transparent)
    High,      // ~180 dB stop band, -3 dB at 21 kHz (24-bit / float transparent)
    Ultra,     // ~207 dB stop band, -3 dB at 21.5 kHz
};

const char* resamplerQualityName(ResamplerQuality q);
bool parseResamplerQuality(const std::string& s, ResamplerQuality& out);

class MultiChannelResampler {
public:
    MultiChannelResampler(double srcRate, double dstRate, uint32_t channels, uint32_t maxInFrames,
                          ResamplerQuality quality);
    ~MultiChannelResampler();

    // Converts `frames` input frames of every channel and appends the produced
    // frames to the internal output FIFO.
    void push(const float* const* input, uint32_t frames);
    uint32_t available() const { return fifoCount_; }
    // Pops `frames` (<= available()) frames into `out` (channel pointers), starting at offset.
    void pop(float* const* out, uint32_t offset, uint32_t frames);

    // Upper bound on frames produced by one push() of maxInFrames.
    uint32_t maxOutPerPush() const { return maxOutPerPush_; }
    double latencyInputFrames() const;

private:
    uint32_t channels_;
    uint32_t maxIn_;
    uint32_t maxOutPerPush_;
    std::vector<std::unique_ptr<r8b::CDSPResampler>> rs_;
    std::vector<double> inBuf_;
    std::vector<std::vector<float>> fifo_;  // per channel ring
    uint32_t fifoCap_ = 0, fifoRead_ = 0, fifoCount_ = 0;
};

}  // namespace brack
