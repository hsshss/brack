#pragma once
// Multi-producer / single-consumer queue of complete MIDI messages.
//
// Producers (MIDI driver threads, API callers) serialize on a mutex among
// themselves; the consumer (audio thread) never blocks. Storage is a fixed
// byte ring allocated up front, so pushing never allocates and popping is
// wait-free. When the ring is full the message is dropped and counted.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <vector>

namespace brack {

class MidiQueue {
public:
    // Messages without a render position are placed by the moment they are for (`when`) as
    // the consumer decides (Engine, ArrivalClock), or delivered as soon as possible.
    static constexpr uint64_t kNow = UINT64_MAX;

    struct Header {
        uint64_t time;     // render position to deliver at (Engine::renderPosition()), or kNow
        int64_t when;      // for kNow: the steady_clock nanoseconds it is for; 0 otherwise
        uint32_t size;     // payload bytes
        uint16_t port;     // destination note port (direct plugin queues), 0 otherwise
        uint16_t reserved;
    };

    explicit MidiQueue(size_t capacityBytes = 1 << 16) : buf_(roundUpPow2(capacityBytes)), mask_(buf_.size() - 1) {}

    // Any thread. Returns false if the message did not fit. `when`: for kNow, the steady_clock
    // nanoseconds it is for; 0 for the moment it is pushed.
    bool push(const uint8_t* data, uint32_t size, uint16_t port = 0, uint64_t time = kNow, int64_t when = 0) {
        const size_t need = sizeof(Header) + size;
        std::lock_guard lock(producerMutex_);
        const size_t w = write_.load(std::memory_order_relaxed);
        const size_t r = read_.load(std::memory_order_acquire);
        if (buf_.size() - (w - r) < need) {
            dropped_.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        if (time != kNow) when = 0;
        else if (when == 0)
            when = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
                       .count();
        Header h{time, when, size, port, 0};
        copyIn(w, &h, sizeof(h));
        copyIn(w + sizeof(h), data, size);
        write_.store(w + need, std::memory_order_release);
        return true;
    }

    // Consumer thread only. Calls fn(const uint8_t* data, uint32_t size, uint16_t port,
    // uint64_t time, int64_t when) for each queued message. `scratch` must be at least as
    // large as the largest message accepted; messages wrapping around the ring are copied there.
    template <typename Fn>
    void drain(std::vector<uint8_t>& scratch, Fn&& fn) {
        size_t r = read_.load(std::memory_order_relaxed);
        const size_t w = write_.load(std::memory_order_acquire);
        while (r != w) {
            Header h;
            copyOut(r, &h, sizeof(h));
            const size_t start = (r + sizeof(h)) & mask_;
            const uint8_t* p;
            if (start + h.size <= buf_.size()) {
                p = buf_.data() + start;
            } else {
                if (scratch.size() < h.size) {  // cannot happen if sized by maxMessageSize()
                    r += sizeof(h) + h.size;
                    continue;
                }
                copyOut(r + sizeof(h), scratch.data(), h.size);
                p = scratch.data();
            }
            fn(p, h.size, h.port, h.time, h.when);
            r += sizeof(h) + h.size;
        }
        read_.store(r, std::memory_order_release);
    }

    // Consumer thread only.
    void clear() { read_.store(write_.load(std::memory_order_acquire), std::memory_order_release); }

    size_t maxMessageSize() const { return buf_.size() - sizeof(Header); }
    uint64_t droppedCount() const { return dropped_.load(std::memory_order_relaxed); }

private:
    static size_t roundUpPow2(size_t v) {
        size_t p = 64;
        while (p < v) p <<= 1;
        return p;
    }
    void copyIn(size_t pos, const void* src, size_t n) {
        pos &= mask_;
        const size_t first = std::min(n, buf_.size() - pos);
        std::memcpy(buf_.data() + pos, src, first);
        std::memcpy(buf_.data(), static_cast<const uint8_t*>(src) + first, n - first);
    }
    void copyOut(size_t pos, void* dst, size_t n) const {
        pos &= mask_;
        const size_t first = std::min(n, buf_.size() - pos);
        std::memcpy(dst, buf_.data() + pos, first);
        std::memcpy(static_cast<uint8_t*>(dst) + first, buf_.data(), n - first);
    }

    std::vector<uint8_t> buf_;
    const size_t mask_;
    std::mutex producerMutex_;
    std::atomic<size_t> write_{0};
    std::atomic<size_t> read_{0};
    std::atomic<uint64_t> dropped_{0};
};

}  // namespace brack
