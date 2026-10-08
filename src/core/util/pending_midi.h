#pragma once
// MIDI messages that arrived before their time: the audio thread keeps them here until the
// block they belong to. Storage is allocated up front; adding never allocates. When full, a
// message is dropped and counted.

#include <cstdint>
#include <cstring>
#include <vector>

namespace brack {

class PendingMidi {
public:
    explicit PendingMidi(size_t maxMessages = 4096, size_t arenaBytes = 64 * 1024)
        : items_(maxMessages), arena_(arenaBytes) {}

    // Audio thread.
    bool add(const uint8_t* data, uint32_t size, uint16_t port, uint64_t time) {
        if (count_ == items_.size() || used_ + size > arena_.size()) {
            ++dropped_;
            return false;
        }
        std::memcpy(arena_.data() + used_, data, size);
        items_[count_++] = {time, (uint32_t)used_, size, port};
        used_ += size;
        return true;
    }

    // Audio thread. Calls fn(data, size, port, time) for every message due(time) accepts, in
    // the order they were added, and keeps the others.
    template <typename Due, typename Fn>
    void take(Due&& due, Fn&& fn) {
        size_t kept = 0, write = 0;
        for (size_t i = 0; i < count_; ++i) {
            Item it = items_[i];
            if (due(it.time)) {
                fn(arena_.data() + it.offset, it.size, it.port, it.time);
                continue;
            }
            // Kept bytes only ever move towards the front, past bytes already consumed.
            std::memmove(arena_.data() + write, arena_.data() + it.offset, it.size);
            it.offset = (uint32_t)write;
            write += it.size;
            items_[kept++] = it;
        }
        count_ = kept;
        used_ = write;
    }

    // Audio thread, or any thread while no audio thread runs.
    void clear() { count_ = used_ = 0; }
    uint64_t droppedCount() const { return dropped_; }

private:
    struct Item {
        uint64_t time;
        uint32_t offset, size;
        uint16_t port;
    };
    std::vector<Item> items_;
    std::vector<uint8_t> arena_;
    size_t count_ = 0, used_ = 0;
    uint64_t dropped_ = 0;
};

}  // namespace brack
