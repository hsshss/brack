#pragma once
// VST 2.4 instruments, through the clean-room vst2sdk header (Xaymar/vst2sdk).

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "plugin/plugin.h"

namespace brack {

std::unique_ptr<PluginInstance> createVst2Instance(HostThread& host, PluginHostListener& listener,
                                                   const std::string& pathUtf8, const std::string& pluginId,
                                                   std::string& error);
// Instantiates each plugin in the file (every one of a shell) just long enough to ask.
std::vector<PluginDescription> describeVst2File(const std::filesystem::path& file, std::string& error);

// VST2 unique ids as text: the four characters when printable ("Abcd"), else "0x%08X".
std::string vst2IdToString(int32_t id);
bool vst2IdFromString(const std::string& s, int32_t& id);

}  // namespace brack
