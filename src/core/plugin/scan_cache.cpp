#include <fstream>
#include <nlohmann/json.hpp>

#include "plugin/plugin.h"
#include "plugin/plugin_files.h"
#include "util/common.h"

namespace brack {

using nlohmann::json;

bool PluginScanCache::load(const std::filesystem::path& file) {
    files.clear();
    std::ifstream f(file, std::ios::binary);
    if (!f) return false;
    try {
        json j = json::parse(f);
        if (j.value("version", 0) != 1) return false;
        for (auto& jf : j.at("files")) {
            Entry e;
            e.size = jf.at("size").get<uint64_t>();
            e.time = jf.at("time").get<int64_t>();
            for (auto& jp : jf.at("plugins")) {
                PluginDescription d;
                if (!pluginFormatFromPath(pathFromUtf8(jp.at("path").get<std::string>()), d.format)) continue;
                d.path = jp.at("path").get<std::string>();
                d.id = jp.at("id").get<std::string>();
                d.name = jp.value("name", "");
                d.vendor = jp.value("vendor", "");
                d.version = jp.value("version", "");
                d.description = jp.value("description", "");
                d.features = jp.value("features", std::vector<std::string>{});
                d.instrument = jp.value("instrument", false);
                d.architecture = jp.at("architecture").get<std::string>();
                e.plugins.push_back(std::move(d));
            }
            files[jf.at("path").get<std::string>()] = std::move(e);
        }
        return true;
    } catch (const std::exception&) {
        files.clear();
        return false;
    }
}

bool PluginScanCache::save(const std::filesystem::path& file, std::string& error) const {
    json jfiles = json::array();
    for (auto& [path, e] : files) {
        json plugins = json::array();
        for (auto& d : e.plugins)
            plugins.push_back({{"path", d.path},
                               {"id", d.id},
                               {"name", d.name},
                               {"vendor", d.vendor},
                               {"version", d.version},
                               {"description", d.description},
                               {"features", d.features},
                               {"instrument", d.instrument},
                               {"architecture", d.architecture}});
        jfiles.push_back({{"path", path}, {"size", e.size}, {"time", e.time}, {"plugins", plugins}});
    }
    json j = {{"version", 1}, {"files", jfiles}};
    return writeFileAtomic(file, j.dump(1), error);
}

}  // namespace brack
