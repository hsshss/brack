#pragma once
// macOS: the host thread is the process's main thread (host_thread_mac.cpp), which must keep
// running its run loop. A program whose main thread would otherwise run its own work (brack-cli,
// a plugin host, the tests) runs `body` on another thread instead, and the main thread runs the
// application's loop until `body` returns. Elsewhere `body` runs on the calling thread.

#include <functional>

namespace brack {

int runWithMainLoop(const std::function<int()>& body);

}  // namespace brack
