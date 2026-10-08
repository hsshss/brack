#pragma once
// VST3 instruments, through Steinberg's pluginterfaces (MIT).

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "plugin/plugin.h"

namespace brack {

std::unique_ptr<PluginInstance> createVst3Instance(HostThread& host, PluginHostListener& listener,
                                                   const std::string& pathUtf8, const std::string& pluginId,
                                                   std::string& error);
// The binary for this architecture inside a bundle (Name.vst3/Contents/<arch>-win/Name.vst3),
// or the path itself when it is a file. It may not exist.
// The audio processor classes in a .vst3 (file or bundle), from its factory.
std::vector<PluginDescription> describeVst3File(const std::filesystem::path& file, std::string& error);

}  // namespace brack
