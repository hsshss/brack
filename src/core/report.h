#pragma once
// JSON views of engine state, shared by the DLL API and the CLI.
#include <string>
#include <vector>

#include "audio/audio_output.h"
#include "plugin/plugin.h"
#include "engine.h"

namespace brack {

std::string snapshotToJson(const EngineSnapshot& s);
std::string audioDevicesToJson(const std::vector<AudioDeviceInfo>& d);
std::string midiInputsToJson(const std::vector<std::string>& names);
std::string pluginsToJson(const std::vector<PluginDescription>& plugins);

}  // namespace brack
