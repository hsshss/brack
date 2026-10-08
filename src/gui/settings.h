#pragma once
// GUI settings persisted in settings.json in the settings folder (configDirectory(), or the one
// --config names): saved on exit, restored on startup. Every architecture's build shares the
// file: each runs the others' plugins in their architectures' plugin hosts (design notes,
// chapter 10).
#include <filesystem>
#include <string>

namespace brack {

struct GuiSettings {
    // Window placement in screen coordinates (the restored, non-maximised geometry).
    bool hasWindow = false;
    int x = 0, y = 0, width = 0, height = 0;
    bool maximized = false;

    std::string scanDirs;     // extra plugin folders, ';'-separated
    std::string newSourceKind = "hardware";  // MIDI inputs: kind picked for adding (midiSourceKindName)
    std::string sessionPath;  // session file the rack was last opened from / saved to
    std::string session;      // the rack itself (session JSON), restored on startup
};

std::filesystem::path guiSettingsPath(const std::filesystem::path& dir);
// Returns false when there is nothing to load; `error` is set only for a broken file.
bool loadGuiSettings(const std::filesystem::path& dir, GuiSettings& out, std::string& error);
bool saveGuiSettings(const std::filesystem::path& dir, const GuiSettings& s, std::string& error);

}  // namespace brack
