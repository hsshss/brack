// Measures the sample rate converter: SNR of a converted sine and stop-band rejection.
#include <cmath>
#include <cstdio>
#include <vector>

#include "audio/resampler.h"

using namespace brack;

namespace {

constexpr double kPi = 3.14159265358979323846;

std::vector<float> convert(double src, double dst, ResamplerQuality q, double freq, double seconds) {
    const uint32_t block = 256;
    MultiChannelResampler rs(src, dst, 1, block, q);
    std::vector<float> in(block), out;
    std::vector<float> tmp(rs.maxOutPerPush() + 1);
    float* outPtr[1] = {tmp.data()};
    const float* inPtr[1] = {in.data()};
    uint64_t n = 0, total = (uint64_t)(src * seconds);
    // Vary the block length to exercise the FIFO with uneven pushes.
    uint32_t sizes[] = {256, 17, 128, 255, 1, 64};
    size_t k = 0;
    while (n < total) {
        uint32_t len = sizes[k++ % 6];
        for (uint32_t i = 0; i < len; ++i, ++n) in[i] = (float)(0.5 * std::sin(2 * kPi * freq * n / src));
        rs.push(inPtr, len);
        uint32_t avail = rs.available();
        rs.pop(outPtr, 0, avail);
        out.insert(out.end(), tmp.begin(), tmp.begin() + avail);
    }
    return out;
}

// Least-squares fit of a*sin + b*cos + c at a known frequency; returns SNR in dB.
double snrDb(const std::vector<float>& x, size_t from, size_t to, double freq, double rate) {
    double ss = 0, sc = 0, cc = 0, xs = 0, xc = 0;
    for (size_t i = from; i < to; ++i) {
        double s = std::sin(2 * kPi * freq * i / rate), c = std::cos(2 * kPi * freq * i / rate);
        ss += s * s;
        sc += s * c;
        cc += c * c;
        xs += x[i] * s;
        xc += x[i] * c;
    }
    double det = ss * cc - sc * sc;
    double a = (xs * cc - xc * sc) / det, b = (xc * ss - xs * sc) / det;
    double sig = 0, err = 0;
    for (size_t i = from; i < to; ++i) {
        double fit = a * std::sin(2 * kPi * freq * i / rate) + b * std::cos(2 * kPi * freq * i / rate);
        sig += fit * fit;
        err += (x[i] - fit) * (x[i] - fit);
    }
    return 10 * std::log10(sig / std::max(err, 1e-300));
}

double rmsDb(const std::vector<float>& x, size_t from, size_t to) {
    double e = 0;
    for (size_t i = from; i < to; ++i) e += (double)x[i] * x[i];
    return 10 * std::log10(std::max(e / (to - from), 1e-300));
}

}  // namespace

int main() {
    struct Case {
        double src, dst;
    } cases[] = {{96000, 48000}, {96000, 44100}, {48000, 44100}, {44100, 48000}, {48000, 96000}, {88200, 48000}};
    struct Q {
        ResamplerQuality q;
        double minSnr, maxStopband;
    } qualities[] = {{ResamplerQuality::Standard, 100, -110}, {ResamplerQuality::High, 125, -125}, {ResamplerQuality::Ultra, 125, -125}};

    int failures = 0;
    for (auto& c : cases) {
        for (auto& q : qualities) {
            const double f = 997.0;  // not a sub-multiple of any rate
            auto out = convert(c.src, c.dst, q.q, f, 1.0);
            size_t from = (size_t)(0.25 * c.dst), to = out.size() - (size_t)(0.05 * c.dst);
            double snr = snrDb(out, from, to, f, c.dst);
            // Fit ignores the (constant) group delay because phase is a free parameter.
            double stop = 0;
            bool down = c.dst < c.src;
            if (down) {
                // A tone above the output Nyquist must vanish (aliasing rejection), relative to the -6 dBFS input.
                double fs = c.dst * 0.5 + (c.src * 0.5 - c.dst * 0.5) * 0.5;
                auto alias = convert(c.src, c.dst, q.q, fs, 1.0);
                size_t af = (size_t)(0.25 * c.dst), at = alias.size() - (size_t)(0.05 * c.dst);
                stop = rmsDb(alias, af, at) - 20 * std::log10(0.5 / std::sqrt(2.0));
            }
            // Passband: an 18 kHz tone must come through within 0.1 dB.
            auto hi = convert(c.src, c.dst, q.q, 18000.0, 0.5);
            size_t hf = (size_t)(0.2 * c.dst), ht = hi.size() - (size_t)(0.02 * c.dst);
            double flat = rmsDb(hi, hf, ht) - 20 * std::log10(0.5 / std::sqrt(2.0));
            double latencyMs = MultiChannelResampler(c.src, c.dst, 1, 256, q.q).latencyInputFrames() * 1000.0 / c.src;
            bool ok = snr >= q.minSnr && (!down || stop <= q.maxStopband) && std::fabs(flat) < 0.1;
            std::printf("%-8s %6.0f -> %6.0f  SNR %6.1f dB  18k %+6.3f dB  latency %5.2f ms%s  %s\n",
                        resamplerQualityName(q.q), c.src, c.dst, snr, flat, latencyMs,
                        down ? ("  alias " + std::to_string((int)stop) + " dB").c_str() : "", ok ? "ok" : "FAIL");
            if (!ok) ++failures;
        }
    }
    std::printf(failures ? "FAILED (%d)\n" : "PASS\n", failures);
    return failures ? 1 : 0;
}
