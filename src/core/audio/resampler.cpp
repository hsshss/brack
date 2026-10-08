#include "audio/resampler.h"

#include <CDSPResampler.h>

#include <algorithm>
#include <cmath>

namespace brack {

const char* resamplerQualityName(ResamplerQuality q) {
    switch (q) {
        case ResamplerQuality::Standard: return "standard";
        case ResamplerQuality::High: return "high";
        case ResamplerQuality::Ultra: return "ultra";
    }
    return "high";
}

bool parseResamplerQuality(const std::string& s, ResamplerQuality& out) {
    if (s == "standard") out = ResamplerQuality::Standard;
    else if (s == "high") out = ResamplerQuality::High;
    else if (s == "ultra") out = ResamplerQuality::Ultra;
    else return false;
    return true;
}

MultiChannelResampler::MultiChannelResampler(double srcRate, double dstRate, uint32_t channels,
                                             uint32_t maxInFrames, ResamplerQuality quality)
    : channels_(channels), maxIn_(maxInFrames) {
    double atten = 180.15, edge = 21000.0;
    switch (quality) {
        case ResamplerQuality::Standard: atten = 136.45; edge = 20500.0; break;
        case ResamplerQuality::High: atten = 180.15; edge = 21000.0; break;
        case ResamplerQuality::Ultra: atten = 206.91; edge = 21500.0; break;
    }
    // Transition band: r8brain measures it from the -3 dB point to the Nyquist
    // frequency of the lower rate. Keeping the audible band flat but no more is
    // what keeps a linear-phase filter short: a fixed 1-2 % band costs 40-70 ms
    // of latency at 44.1/48 kHz, which is unplayable for a live instrument host.
    const double nyq = std::min(srcRate, dstRate) * 0.5;
    edge = std::min(edge, nyq * 0.96);
    const double transBand = std::clamp((nyq - edge) / nyq * 100.0, 0.5, 45.0);
    for (uint32_t c = 0; c < channels_; ++c)
        rs_.push_back(std::make_unique<r8b::CDSPResampler>(srcRate, dstRate, (int)maxIn_, transBand, atten,
                                                           r8b::fprLinearPhase));
    inBuf_.resize(maxIn_);
    maxOutPerPush_ = rs_.empty() ? 0 : (uint32_t)rs_[0]->getMaxOutLen((int)maxIn_);
    // Room for a few pushes plus a full device period worth of backlog.
    fifoCap_ = std::max<uint32_t>(maxOutPerPush_ * 4, (uint32_t)std::ceil(maxIn_ * dstRate / srcRate) * 4 + 8192);
    fifo_.assign(channels_, std::vector<float>(fifoCap_, 0.0f));
}

MultiChannelResampler::~MultiChannelResampler() = default;

double MultiChannelResampler::latencyInputFrames() const {
    // r8brain pre-compensates its filter delay: it shows up as input consumed
    // before the first output sample rather than in getLatency().
    return rs_.empty() ? 0.0 : rs_[0]->getInLenBeforeOutPos(0) + rs_[0]->getLatencyFrac();
}

void MultiChannelResampler::push(const float* const* input, uint32_t frames) {
    frames = std::min(frames, maxIn_);
    uint32_t produced = 0;
    for (uint32_t c = 0; c < channels_; ++c) {
        for (uint32_t i = 0; i < frames; ++i) inBuf_[i] = input[c][i];
        double* op = nullptr;
        int n = rs_[c]->process(inBuf_.data(), (int)frames, op);
        // All channels share the same configuration, so they produce the same count.
        uint32_t room = fifoCap_ - fifoCount_;
        uint32_t count = std::min<uint32_t>((uint32_t)n, room);
        uint32_t w = (fifoRead_ + fifoCount_) % fifoCap_;
        float* dst = fifo_[c].data();
        for (uint32_t i = 0; i < count; ++i) {
            dst[w] = (float)op[i];
            if (++w == fifoCap_) w = 0;
        }
        produced = count;
    }
    fifoCount_ += produced;
}

void MultiChannelResampler::pop(float* const* out, uint32_t offset, uint32_t frames) {
    frames = std::min(frames, fifoCount_);
    for (uint32_t c = 0; c < channels_; ++c) {
        uint32_t r = fifoRead_;
        const float* src = fifo_[c].data();
        float* dst = out[c] + offset;
        for (uint32_t i = 0; i < frames; ++i) {
            dst[i] = src[r];
            if (++r == fifoCap_) r = 0;
        }
    }
    fifoRead_ = (fifoRead_ + frames) % fifoCap_;
    fifoCount_ -= frames;
}

}  // namespace brack
