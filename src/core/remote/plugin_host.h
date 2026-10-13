#pragma once
// Both ends of a plugin host process (see remote/host_protocol.h).

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "plugin/chain.h"

namespace brack {

// The plugin host process's whole program: serves the Brack process that started it, given the
// arguments it was started with (after the program name, UTF-8). Returns the exit code.
int runPluginHost(const std::vector<std::string>& args);

// brack-host-<arch>(.exe), beside the module this code is in (brack.exe, brack-cli, brack.dll,
// libbrack.so, libbrack.dylib); on Linux and macOS, else in the installation's libexec/brack.
std::filesystem::path pluginHostExecutable(const std::string& architecture);

// A chain running in a plugin host process of its own: with the instrument at `pathUtf8`, in a
// process of its architecture, or (the master's) without one, in a process of Brack's. A crash or
// a hang ends only that process: the chain is then crashed, its plugins keep their last states,
// and they can be loaded again in a new process. Null, with `error`, if the instrument does not load.
std::unique_ptr<HostedChain> createRemoteChain(PluginHostListener& listener, const std::string& pathUtf8,
                                               const std::string& pluginId, const std::string& architecture,
                                               std::string& error);
std::unique_ptr<HostedChain> createRemoteMasterChain(PluginHostListener& listener, std::string& error);

// Describes plugin files in plugin host processes, one per architecture, kept for the next
// file. A file whose plugin crashes or hangs while being described fails alone: the next one
// gets a new process.
class RemoteDescriber {
public:
    RemoteDescriber();
    ~RemoteDescriber();
    // Whether a plugin host for `architecture` is installed, and this computer runs it.
    bool runs(const std::string& architecture) const;
    // Empty when not (runs()).
    std::optional<std::vector<PluginDescription>> describe(const std::filesystem::path& file,
                                                           const std::string& architecture, std::string& error);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace brack
