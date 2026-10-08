#pragma once
// Waiting on a 32-bit word of memory shared between processes (ControlBlock::turn): the Linux
// futex, not the process-private kind, since Brack and the plugin host map the word separately;
// on macOS, os_sync_wait_on_address with its flag for shared memory (macOS 14.4).

#include <climits>
#include <cstdint>
#include <ctime>

#ifdef __APPLE__
#include <os/os_sync_wait_on_address.h>
#else
#include <linux/futex.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

namespace brack::futex {

// Returns once `*word` is not `expected`, or after `timeout` (null: no limit), or on a signal, or
// for no reason at all: the caller looks at the word again.
#ifdef __APPLE__
inline void wait(uint32_t* word, uint32_t expected, const timespec* timeout) {
    if (!timeout) {
        os_sync_wait_on_address(word, expected, sizeof *word, OS_SYNC_WAIT_ON_ADDRESS_SHARED);
        return;
    }
    const uint64_t ns = (uint64_t)timeout->tv_sec * 1000000000u + (uint64_t)timeout->tv_nsec;
    if (ns == 0) return;
    os_sync_wait_on_address_with_timeout(word, expected, sizeof *word, OS_SYNC_WAIT_ON_ADDRESS_SHARED,
                                         OS_CLOCK_MACH_ABSOLUTE_TIME, ns);
}

inline void wakeAll(uint32_t* word) { os_sync_wake_by_address_all(word, sizeof *word, OS_SYNC_WAKE_BY_ADDRESS_SHARED); }
#else
static_assert(sizeof(timespec::tv_sec) == sizeof(long),
              "SYS_futex takes the old timespec: a 64-bit time_t in a 32-bit build needs SYS_futex_time64");

inline void wait(uint32_t* word, uint32_t expected, const timespec* timeout) {
    syscall(SYS_futex, word, FUTEX_WAIT, expected, timeout, nullptr, 0);
}

inline void wakeAll(uint32_t* word) { syscall(SYS_futex, word, FUTEX_WAKE, INT_MAX, nullptr, nullptr, 0); }
#endif

}  // namespace brack::futex
