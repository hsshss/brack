#pragma once
// Meters: written on the audio thread, read on any thread (relaxed atomics).
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>

namespace brack {

// A meter unwritten for this long (its chain out of the graph, the engine stopped) reads zero, and
// its next write starts afresh. Longer than any device buffer. Times are steady-clock ns.
constexpr int64_t kMeterStaleNs = 250'000'000;

// What a level falls by in a block of `frames` at `rate`: 0.9 every 10 ms.
inline float levelFall(uint32_t frames, uint32_t rate) {
    return (float)std::pow(0.9, (double)frames / ((double)rate * 0.01));
}

// A channel's level for meters.
struct LevelMeter {
    std::atomic<uint64_t> clips{0};  // blocks with a sample past full scale

    // Audio thread, at `nowNs`. `src` null: silent this block (the peak only falls). `fall`: see
    // levelFall(). Sample i is read times g0 + step * (i + 1), a gain ramped across the block.
    void update(const float* src, uint32_t frames, float fall, int64_t nowNs, float g0 = 1.0f, float step = 0.0f) {
        float pk = 0;
        if (src)
            for (uint32_t i = 0; i < frames; ++i) pk = std::max(pk, std::fabs(src[i] * (g0 + step * (float)(i + 1))));
        if (pk > 1.0f) clips.fetch_add(1, std::memory_order_relaxed);
        peak_.store(std::max(pk, peak(nowNs) * fall), std::memory_order_relaxed);
        updatedNs_.store(nowNs, std::memory_order_relaxed);
    }
    float peak(int64_t nowNs) const {
        const bool fresh = nowNs - updatedNs_.load(std::memory_order_relaxed) < kMeterStaleNs;
        return fresh ? peak_.load(std::memory_order_relaxed) : 0.0f;
    }

private:
    std::atomic<float> peak_{0};
    std::atomic<int64_t> updatedNs_{0};
};

// Processing time against real time (1: a block took as long as it lasts): an average with a
// time constant of 0.1 s, and the most within about the last second.
struct LoadMeter {
    // Audio thread: a block of `frames` at `rate` took `load`, as of `nowNs`.
    void add(float load, uint32_t frames, uint32_t rate, int64_t nowNs) {
        if (!fresh(nowNs)) {
            average_.store(0, std::memory_order_relaxed);
            windowMax_ = lastWindowMax_ = 0;
            windowFrames_ = 0;
        }
        const double seconds = (double)frames / rate;
        const float alpha = (float)(1.0 - std::exp(-seconds / 0.1));
        average_.store(average_.load(std::memory_order_relaxed) * (1.0f - alpha) + load * alpha,
                       std::memory_order_relaxed);
        // Two windows of half a second: the most of the current one and of the one before.
        windowMax_ = std::max(windowMax_, load);
        windowFrames_ += frames;
        if (windowFrames_ * 2 >= rate) {
            lastWindowMax_ = windowMax_;
            windowMax_ = 0;
            windowFrames_ = 0;
        }
        peak_.store(std::max(windowMax_, lastWindowMax_), std::memory_order_relaxed);
        updatedNs_.store(nowNs, std::memory_order_relaxed);
    }
    float average(int64_t nowNs) const { return fresh(nowNs) ? average_.load(std::memory_order_relaxed) : 0.0f; }
    float peak(int64_t nowNs) const { return fresh(nowNs) ? peak_.load(std::memory_order_relaxed) : 0.0f; }

private:
    bool fresh(int64_t nowNs) const { return nowNs - updatedNs_.load(std::memory_order_relaxed) < kMeterStaleNs; }

    std::atomic<float> average_{0}, peak_{0};
    std::atomic<int64_t> updatedNs_{0};
    float windowMax_ = 0, lastWindowMax_ = 0;  // audio thread
    uint64_t windowFrames_ = 0;
};

// A chain's meters.
struct ChainMeters {
    static constexpr size_t kChannels = 64;
    // Its output channels, port after port (those past kChannels are not metered).
    std::array<LevelMeter, kChannels> out;
    LoadMeter load;
    std::atomic<uint64_t> midi{0};  // messages the instrument took
};

}  // namespace brack
