#pragma once
// Linux: real-time scheduling for audio threads, Brack's own and its plugin hosts'. Brack asks
// for it from a thread of its own, for the thread a callback or a plugin host named: directly
// (SCHED_FIFO) when the thread's process may (an rtprio limit, as audio groups have, or
// CAP_SYS_NICE), else through RealtimeKit, as desktop sessions have it.

#include <sys/types.h>

namespace brack::realtime {

// The calling thread's id, for makeRealtime().
pid_t threadId();

// Makes thread `tid` of process `pid` (this process, or a plugin host it started) real-time,
// unless it already is, and logs what came of it whenever that differs from the last time
// (`what`: "audio output thread", say). Can wait for RealtimeKit, up to a second: not on an
// audio thread.
void makeRealtime(pid_t pid, pid_t tid, const char* what);

// In the process whose thread is to be made real-time, before: a thread that then runs too long
// without sleeping goes back to normal priority, where RealtimeKit's limit (RLIMIT_RTTIME) would
// otherwise end the process. Unless the process handles SIGXCPU itself.
void fallBackOnOverrun();

}  // namespace brack::realtime
