#include "settings.h"

#include <fstream>
#include <nlohmann/json.hpp>
#include <sstream>

#include "util/common.h"

namespace brack {

using json = nlohmann::ordered_json;

std::filesystem::path guiSettingsPath(const std::filesystem::path& dir) {
    return dir.empty() ? dir : dir / "settings.json";
}

bool loadGuiSettings(const std::filesystem::path& dir, GuiSettings& out, std::string& error) {
    auto path = guiSettingsPath(dir);
    std::ifstream f(path, std::ios::binary);
    if (path.empty() || !f) return false;
    std::stringstream ss;
    ss << f.rdbuf();
    f.close();
    try {
        json j = json::parse(ss.str());
        if (j.value("format", "") != "brack-settings") throw std::runtime_error("not a Brack settings file");
        GuiSettings s;
        if (j.contains("window")) {
            auto& w = j["window"];
            s.hasWindow = true;
            s.x = w.value("x", 0);
            s.y = w.value("y", 0);
            s.width = w.value("width", 0);
            s.height = w.value("height", 0);
            s.maximized = w.value("maximized", false);
            s.hasWindow = s.width > 0 && s.height > 0;
        }
        s.scanDirs = j.value("scanDirs", "");
        s.newSourceKind = j.value("newSourceKind", s.newSourceKind);
        s.sessionPath = j.value("sessionPath", "");
        if (j.contains("session") && j["session"].is_object()) s.session = j["session"].dump();
        out = std::move(s);
        return true;
    } catch (const std::exception& e) {
        // Keep the broken file for inspection instead of overwriting it on exit.
        std::error_code ec;
        auto bad = path;
        bad += ".broken";
        std::filesystem::rename(path, bad, ec);
        error = pathToUtf8(path) + ": " + e.what() + (ec ? "" : " (moved to " + pathToUtf8(bad) + ")");
        return false;
    }
}

bool saveGuiSettings(const std::filesystem::path& dir, const GuiSettings& s, std::string& error) {
    auto path = guiSettingsPath(dir);
    if (path.empty()) {
        error = "no settings folder on this system";
        return false;
    }
    json j;
    j["format"] = "brack-settings";
    if (s.hasWindow)
        j["window"] = {{"x", s.x}, {"y", s.y}, {"width", s.width}, {"height", s.height}, {"maximized", s.maximized}};
    j["scanDirs"] = s.scanDirs;
    j["newSourceKind"] = s.newSourceKind;
    j["sessionPath"] = s.sessionPath;
    if (!s.session.empty()) {
        try {
            j["session"] = json::parse(s.session);
        } catch (const std::exception& e) {
            error = std::string("session: ") + e.what();
            return false;
        }
    }
    return writeFileAtomic(path, j.dump(2), error);
}

}  // namespace brack
