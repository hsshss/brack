// brack-host-<arch>: runs one plugin for a Brack process (see src/core/remote/host_protocol.h).
#include <string>
#include <vector>

#include "remote/plugin_host.h"
#include "util/common.h"
#include "util/main_thread.h"

#ifdef _WIN32
#include <windows.h>

int wmain(int argc, wchar_t** argv) {
    // As a whole, not only the host thread: plugins (JUCE's) scale their editors by the process's.
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    std::vector<std::string> args;
    for (int i = 1; i < argc; ++i) args.push_back(brack::narrow(argv[i]));
    return brack::runPluginHost(args);
}
#else
int main(int argc, char** argv) {
    const std::vector<std::string> args(argv + 1, argv + argc);
    return brack::runWithMainLoop([&] { return brack::runPluginHost(args); });
}
#endif
