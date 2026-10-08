// Format dispatch: creating instances, standard locations, scanning.
#include <algorithm>
#include <limits>
#include <cstdlib>
#include <set>

#include "clap/clap_instance.h"
#include "plugin/library.h"
#include "plugin/plugin.h"
#include "plugin/plugin_files.h"
#include "remote/plugin_host.h"
#include "util/common.h"
#include "vst2/vst2_plugin.h"
#include "vst3/vst3_plugin.h"

#ifdef _WIN32
#include <windows.h>
#include <shlobj.h>
#endif

namespace brack {

namespace {
// Any library could be a VST2 plugin: look before loading one (nothing in it runs).
bool checkVst2Binary(const std::filesystem::path& file, std::string& error) {
    BinaryInfo bi;
    if (!inspectBinary(pluginBinaryPath(PluginFormat::Vst2, file), bi, error)) return false;
    if (!bi.hasExport("VSTPluginMain") && !bi.hasExport("main")) {
        error = "not a VST2 plugin (no VSTPluginMain)";
        return false;
    }
    if (!bi.loadable) {
        error = "built for " + bi.architecture() + ", which this build of Brack cannot load";
        return false;
    }
    return true;
}
}  // namespace

std::string pluginArchitecture(const std::filesystem::path& path) {
    PluginFormat format;
    if (!pluginFormatFromPath(path, format)) return {};
    std::error_code ec;
    // A Windows or Linux VST3 bundle holds a binary per architecture, in folders named for them;
    // a macOS bundle holds one, maybe universal, whose headers say.
    if (format == PluginFormat::Vst3 && kCurrentOs != TargetOs::MacOS && std::filesystem::is_directory(path, ec))
        return vst3BundleArchitecture(path);
    BinaryInfo bi;
    std::string error;
    return inspectBinary(pluginBinaryPath(format, path), bi, error) ? bi.architecture() : std::string();
}

std::unique_ptr<PluginInstance> createPluginInstance(HostThread& host, PluginHostListener& listener,
                                                     const std::string& pathUtf8, const std::string& pluginId,
                                                     std::string& error) {
    PluginFormat format;
    if (!pluginFormatFromPath(pathFromUtf8(pathUtf8), format)) {
        error = "unknown plugin format (expected " + pluginExtensionsText() + ")";
        return nullptr;
    }
    switch (format) {
        case PluginFormat::Clap: {
            auto module = ClapModule::load(pathUtf8, error);
            if (!module) return nullptr;
            std::vector<PluginDescription> descs;
            if (!module->describe(descs, error)) return nullptr;
            auto it = std::find_if(descs.begin(), descs.end(),
                                   [&](auto& d) { return pluginId.empty() || d.id == pluginId; });
            if (it == descs.end()) {
                error = pluginId.empty() ? "no plugins in the file" : "plugin " + pluginId + " not found";
                return nullptr;
            }
            PluginDescription desc = *it;
            std::string name = desc.name;
            return std::make_unique<ClapInstance>(host, listener, std::move(module), std::move(desc), name);
        }
        case PluginFormat::Vst2:
            if (!checkVst2Binary(pathFromUtf8(pathUtf8), error)) return nullptr;
            return createVst2Instance(host, listener, pathUtf8, pluginId, error);
        case PluginFormat::Vst3: return createVst3Instance(host, listener, pathUtf8, pluginId, error);
    }
    return nullptr;
}

std::vector<std::filesystem::path> defaultPluginSearchPaths(PluginFormat f) {
    std::vector<std::filesystem::path> dirs;
    auto addEnvList = [&](const char* var) {
        const char* v = std::getenv(var);
        if (!v) return;
        std::string s = v;
#ifdef _WIN32
        const char sep = ';';
#else
        const char sep = ':';
#endif
        size_t start = 0;
        while (start <= s.size()) {
            size_t end = s.find(sep, start);
            if (end == std::string::npos) end = s.size();
            if (end > start) dirs.push_back(pathFromUtf8(s.substr(start, end - start)));
            start = end + 1;
        }
    };
#ifdef _WIN32
    // The 64-bit and the 32-bit places both, whatever this build is: a plugin of another
    // architecture runs in its plugin host, and every build finds the same plugins (and can share
    // the scan cache). findPluginFiles() drops what is found twice.
    auto known = [&](REFKNOWNFOLDERID id, const wchar_t* sub) {
        PWSTR p = nullptr;
        if (SUCCEEDED(SHGetKnownFolderPath(id, 0, nullptr, &p))) dirs.push_back(std::filesystem::path(p) / sub);
        CoTaskMemFree(p);
    };
    // A 32-bit process gets the 64-bit Program Files only from these (FOLDERID_ProgramFilesX64
    // fails there).
    auto env = [&](const wchar_t* var, const wchar_t* sub) {
        if (const wchar_t* v = _wgetenv(var); v && *v) dirs.push_back(std::filesystem::path(v) / sub);
    };
    auto programFiles = [&](const wchar_t* sub) {
        known(FOLDERID_ProgramFiles, sub);
        known(FOLDERID_ProgramFilesX86, sub);
        env(L"ProgramW6432", sub);
    };
    auto commonFiles = [&](const wchar_t* sub) {
        known(FOLDERID_ProgramFilesCommon, sub);
        known(FOLDERID_ProgramFilesCommonX86, sub);
        env(L"CommonProgramW6432", sub);
    };
    switch (f) {
        case PluginFormat::Clap:
            addEnvList("CLAP_PATH");
            commonFiles(L"CLAP");
            known(FOLDERID_LocalAppData, L"Programs\\Common\\CLAP");
            break;
        case PluginFormat::Vst3:
            addEnvList("VST3_PATH");
            commonFiles(L"VST3");
            known(FOLDERID_LocalAppData, L"Programs\\Common\\VST3");
            break;
        case PluginFormat::Vst2: {
            addEnvList("VST_PATH");
            // The folder hosts and installers agree on, if one is registered: HKLM has one for
            // 64-bit plugins and one for 32-bit ones (WOW6432Node); HKCU is the same for both.
            auto registered = [&](HKEY root, REGSAM view) {
                HKEY key = nullptr;
                if (RegOpenKeyExW(root, L"SOFTWARE\\VST", 0, KEY_QUERY_VALUE | view, &key) != ERROR_SUCCESS) return;
                wchar_t buf[MAX_PATH * 2];
                DWORD size = sizeof buf;
                if (RegGetValueW(key, nullptr, L"VSTPluginsPath", RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ, nullptr, buf,
                                 &size) == ERROR_SUCCESS && buf[0])
                    dirs.emplace_back(buf);
                RegCloseKey(key);
            };
            registered(HKEY_LOCAL_MACHINE, KEY_WOW64_64KEY);
            registered(HKEY_LOCAL_MACHINE, KEY_WOW64_32KEY);
            registered(HKEY_CURRENT_USER, 0);
            programFiles(L"VSTPlugins");
            programFiles(L"Steinberg\\VSTPlugins");
            commonFiles(L"VST2");
            commonFiles(L"Steinberg\\VST2");
            break;
        }
    }
#elif defined(__APPLE__)
    const char* home = std::getenv("HOME");
    const char* sub = f == PluginFormat::Clap ? "CLAP" : f == PluginFormat::Vst3 ? "VST3" : "VST";
    addEnvList(f == PluginFormat::Clap ? "CLAP_PATH" : f == PluginFormat::Vst3 ? "VST3_PATH" : "VST_PATH");
    dirs.push_back(std::filesystem::path("/Library/Audio/Plug-Ins") / sub);
    if (home) dirs.push_back(std::filesystem::path(home) / "Library/Audio/Plug-Ins" / sub);
#else
    const char* home = std::getenv("HOME");
    switch (f) {
        case PluginFormat::Clap:
            addEnvList("CLAP_PATH");
            if (home) dirs.push_back(std::filesystem::path(home) / ".clap");
            dirs.emplace_back("/usr/lib/clap");
            break;
        case PluginFormat::Vst3:
            addEnvList("VST3_PATH");
            if (home) dirs.push_back(std::filesystem::path(home) / ".vst3");
            dirs.emplace_back("/usr/lib/vst3");
            break;
        case PluginFormat::Vst2:
            addEnvList("VST_PATH");
            if (home) dirs.push_back(std::filesystem::path(home) / ".vst");
            dirs.emplace_back("/usr/lib/vst");
            break;
    }
#endif
    return dirs;
}

std::vector<std::filesystem::path> defaultPluginSearchPaths() {
    std::vector<std::filesystem::path> all;
    for (PluginFormat f : {PluginFormat::Clap, PluginFormat::Vst3, PluginFormat::Vst2}) {
        auto d = defaultPluginSearchPaths(f);
        all.insert(all.end(), d.begin(), d.end());
    }
    return all;
}

std::vector<std::filesystem::path> findPluginFiles(const std::vector<std::filesystem::path>& dirs) {
    std::vector<std::filesystem::path> files;
    std::set<std::filesystem::path> seen;
    for (auto& dir : dirs) {
        std::error_code ec;
        if (!std::filesystem::is_directory(dir, ec)) continue;
        auto it = std::filesystem::recursive_directory_iterator(dir, std::filesystem::directory_options::skip_permission_denied, ec);
        for (; !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
            PluginFormat format;
            if (!pluginFormatFromPath(it->path(), format)) continue;
            std::error_code entryEc;  // not ec: a failure here must not end the walk
            if (it->is_directory(entryEc)) {
                // Bundles (.vst3 folders; .clap and .vst on macOS) are one plugin: list them, skip
                // their insides.
                if (!canBeBundle(format)) continue;
                it.disable_recursion_pending();
            } else if (!it->is_regular_file(entryEc)) {
                continue;
            }
            if (format == PluginFormat::Vst2) {
                // Plugin folders hold helper libraries too: only those with a VST2 entry point count.
                BinaryInfo bi;
                std::string err;
                if (!inspectBinary(pluginBinaryPath(format, it->path()), bi, err) ||
                    !(bi.hasExport("VSTPluginMain") || bi.hasExport("main")))
                    continue;
            }
            auto canon = std::filesystem::weakly_canonical(it->path(), entryEc);
            if (seen.insert(canon).second) files.push_back(canon);
        }
    }
    return files;
}

std::vector<PluginDescription> describePluginFile(const std::filesystem::path& file, std::string& error) {
    PluginFormat format;
    if (!pluginFormatFromPath(file, format)) {
        error = "unknown plugin format (expected " + pluginExtensionsText() + ")";
        return {};
    }
    switch (format) {
        case PluginFormat::Clap: {
            auto m = ClapModule::load(pathToUtf8(file), error);
            std::vector<PluginDescription> descs;
            if (m) m->describe(descs, error);
            return descs;
        }
        case PluginFormat::Vst3: return describeVst3File(file, error);
        case PluginFormat::Vst2:
            if (!checkVst2Binary(file, error)) return {};
            return describeVst2File(file, error);
    }
    return {};
}

namespace {
// Size and newest modification time of a plugin file, or of everything inside a bundle.
bool fileStamp(const std::filesystem::path& p, uint64_t& size, int64_t& time) {
    std::error_code ec;
    size = 0;
    time = std::numeric_limits<int64_t>::min();  // libstdc++'s file clock counts back from 2174: now is negative
    auto add = [&](const std::filesystem::path& f) {
        size += std::filesystem::file_size(f, ec);
        time = std::max<int64_t>(time, std::filesystem::last_write_time(f, ec).time_since_epoch().count());
    };
    if (!std::filesystem::is_directory(p, ec)) {
        add(p);
        return !ec;
    }
    auto it = std::filesystem::recursive_directory_iterator(p, std::filesystem::directory_options::skip_permission_denied, ec);
    for (; !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec))
        if (it->is_regular_file(ec)) add(it->path());
    return !ec;
}
}  // namespace

std::vector<PluginDescription> scanPlugins(const std::vector<std::filesystem::path>& dirs, PluginScanCache* cache) {
    std::vector<PluginDescription> all;
    std::map<std::string, PluginScanCache::Entry> found;
    RemoteDescriber hosts;
    for (auto& file : findPluginFiles(dirs)) {
        const std::string key = pathToUtf8(file);
        std::string architecture = pluginArchitecture(file);
        if (architecture.empty()) architecture = buildArchitecture();  // describing it says what is wrong
        PluginScanCache::Entry entry;
        const bool stamped = cache && fileStamp(file, entry.size, entry.time);
        if (stamped) {
            auto it = cache->files.find(key);
            if (it != cache->files.end() && it->second.size == entry.size && it->second.time == entry.time) {
                // Kept even when it is not for this build: builds for other architectures share the
                // cache. Which binary of a VST3 bundle runs is this build's choice.
                found[key] = it->second;
                if (architecture != buildArchitecture() && !hosts.runs(architecture)) {
                    log(LogLevel::Debug, "scan: " + key + ": built for " + architecture + ", for which no plugin host runs here");
                    continue;
                }
                for (PluginDescription d : it->second.plugins) {
                    d.architecture = architecture;
                    all.push_back(std::move(d));
                }
                continue;
            }
        }
        // In a plugin host process, so that a plugin that crashes or hangs while it is asked fails
        // alone; here only if there is no host for Brack's own architecture.
        std::string error;
        auto described = hosts.describe(file, architecture, error);
        if (!described && architecture != buildArchitecture()) {
            log(LogLevel::Debug, "scan: " + key + ": built for " + architecture + ", for which no plugin host runs here");
            continue;
        }
        std::vector<PluginDescription> ds = described ? std::move(*described) : describePluginFile(file, error);
        for (auto& d : ds) d.architecture = architecture;
        if (ds.empty()) {
            logWarn("scan: " + key + ": " + (error.empty() ? "no plugins" : error));
            continue;
        }
        all.insert(all.end(), ds.begin(), ds.end());
        if (stamped) {
            entry.plugins = std::move(ds);
            found[key] = std::move(entry);
        }
    }
    if (cache) cache->files = std::move(found);
    return all;
}

}  // namespace brack
