#pragma once
// How each plugin format lays out its files on each OS: the extensions, which ones are bundles
// (folders), the binary inside a bundle, and the architecture folders of VST3 bundles. Path
// rules only, with the OS as a parameter, so every platform's rules are tested on any platform.

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "plugin/plugin.h"
#include "util/common.h"

namespace brack {

enum class TargetOs { Windows, MacOS, Linux };

#if defined(_WIN32)
constexpr TargetOs kCurrentOs = TargetOs::Windows;
#elif defined(__APPLE__)
constexpr TargetOs kCurrentOs = TargetOs::MacOS;
#else
constexpr TargetOs kCurrentOs = TargetOs::Linux;
#endif

// The VST2 plugin extension: ".dll" (Windows), ".vst" (a bundle, macOS), ".so" (Linux).
const char* vst2Extension(TargetOs os = kCurrentOs);
// For messages: ".clap, .vst3 or .dll".
std::string pluginExtensionsText(TargetOs os = kCurrentOs);

// By extension (case insensitive): .clap, .vst3 and vst2Extension().
bool pluginFormatFromPath(const std::filesystem::path& path, PluginFormat& out, TargetOs os = kCurrentOs);

// Whether a plugin of this format can be a folder (a bundle) that counts as one plugin: VST3
// everywhere (a single .vst3 file also loads on Windows), CLAP and VST2 on macOS.
bool canBeBundle(PluginFormat format, TargetOs os = kCurrentOs);

// The folders inside a VST3 bundle's Contents that hold a binary a build of `arch` (a
// buildArchitecture() name) loads on a computer of `machine` (a machineArchitecture() name),
// best first. On an ARM64 PC, an x64 build loads ARM64EC and ARM64X binaries too, which then
// run natively, so it takes them before the x64 one.
std::vector<std::string> vst3ArchFolders(TargetOs os, std::string_view arch, std::string_view machine);

// The library to load for the plugin at `path`: `path` itself if it is a file, otherwise the
// binary inside the bundle (the best one that exists, or where the best one would be).
std::filesystem::path pluginBinaryPath(PluginFormat format, const std::filesystem::path& path,
                                       TargetOs os = kCurrentOs, std::string_view arch = buildArchitecture(),
                                       std::string_view machine = machineArchitecture());

// The architecture (a buildArchitecture() name) of the binary a VST3 bundle would run as: `arch`
// if the bundle holds a binary it loads (vst3ArchFolders), else the first of x64, x86 and arm64
// it holds one for. Empty if it holds none. A bundle with only an ARM64EC binary is "x64" on an
// ARM64 PC (the x64 plugin host loads it) and nothing on an x64 PC.
// On macOS, `arch` whenever there is a binary: there is one, maybe universal, and only its
// headers say what it holds (pluginArchitecture() reads them).
std::string vst3BundleArchitecture(const std::filesystem::path& bundle, TargetOs os = kCurrentOs,
                                   std::string_view arch = buildArchitecture(),
                                   std::string_view machine = machineArchitecture());

}  // namespace brack
