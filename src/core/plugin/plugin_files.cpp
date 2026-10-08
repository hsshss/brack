#include "plugin/plugin_files.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <iterator>

namespace brack {

namespace fs = std::filesystem;

const char* vst2Extension(TargetOs os) {
    switch (os) {
        case TargetOs::Windows: return ".dll";
        case TargetOs::MacOS: return ".vst";
        case TargetOs::Linux: return ".so";
    }
    return ".dll";
}

std::string pluginExtensionsText(TargetOs os) { return std::string(".clap, .vst3 or ") + vst2Extension(os); }

bool pluginFormatFromPath(const fs::path& path, PluginFormat& out, TargetOs os) {
    std::string ext = pathToUtf8(path.extension());
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return (char)std::tolower(c); });
    if (ext == ".clap") out = PluginFormat::Clap;
    else if (ext == ".vst3") out = PluginFormat::Vst3;
    else if (ext == vst2Extension(os)) out = PluginFormat::Vst2;
    else return false;
    return true;
}

bool canBeBundle(PluginFormat format, TargetOs os) { return format == PluginFormat::Vst3 || os == TargetOs::MacOS; }

std::vector<std::string> vst3ArchFolders(TargetOs os, std::string_view arch, std::string_view machine) {
    switch (os) {
        case TargetOs::Windows:
            // ARM64EC code loads only in an x64 process on an ARM64 PC (not in an arm64 one).
            if (arch == "arm64") return {"arm64x-win", "arm64-win"};
            if (arch == "x86") return {"x86-win"};
            if (machine == "arm64") return {"arm64ec-win", "arm64x-win", "x86_64-win"};
            return {"x86_64-win"};
        case TargetOs::Linux:
            if (arch == "arm64") return {"aarch64-linux"};
            if (arch == "x86") return {"i386-linux"};
            return {"x86_64-linux"};
        case TargetOs::MacOS: return {"MacOS"};
    }
    return {};
}

namespace {

std::string plistExecutable(const fs::path& bundle) {
    std::ifstream in(bundle / "Contents" / "Info.plist", std::ios::binary);
    const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    const std::string key = "<key>CFBundleExecutable</key>";
    size_t at = text.find(key);
    if (at == std::string::npos) return {};
    at = text.find_first_not_of(" \t\r\n", at + key.size());
    const std::string open = "<string>";
    if (at == std::string::npos || text.compare(at, open.size(), open) != 0) return {};
    at += open.size();
    const size_t end = text.find('<', at);
    return end == std::string::npos ? std::string() : text.substr(at, end - at);
}

fs::path bundleBinaryName(const fs::path& bundle, TargetOs os) {
    switch (os) {
        case TargetOs::Windows: return bundle.filename();
        case TargetOs::Linux: return fs::path(pathToUtf8(bundle.stem()) + ".so");
        case TargetOs::MacOS: {
            if (std::string name = plistExecutable(bundle); !name.empty()) return pathFromUtf8(name);
            std::error_code ec;
            const fs::path folder = bundle / "Contents" / "MacOS";
            if (fs::is_regular_file(folder / bundle.stem(), ec)) return bundle.stem();
            fs::path only;
            for (auto& entry : fs::directory_iterator(folder, ec)) {
                if (!entry.is_regular_file(ec)) continue;
                if (!only.empty()) return bundle.stem();
                only = entry.path().filename();
            }
            return only.empty() ? bundle.stem() : only;
        }
    }
    return bundle.filename();
}

}  // namespace

fs::path pluginBinaryPath(PluginFormat format, const fs::path& path, TargetOs os, std::string_view arch,
                          std::string_view machine) {
    std::error_code ec;
    if (!canBeBundle(format, os) || !fs::is_directory(path, ec)) return path;
    const fs::path name = bundleBinaryName(path, os);
    if (format != PluginFormat::Vst3) return path / "Contents" / "MacOS" / name;  // macOS CLAP and VST2 bundles
    const auto folders = vst3ArchFolders(os, arch, machine);
    for (auto& folder : folders) {
        fs::path p = path / "Contents" / folder / name;
        if (fs::is_regular_file(p, ec)) return p;
    }
    return path / "Contents" / folders.front() / name;
}

std::string vst3BundleArchitecture(const fs::path& bundle, TargetOs os, std::string_view arch, std::string_view machine) {
    std::error_code ec;
    const fs::path name = bundleBinaryName(bundle, os);
    for (std::string_view a : {arch, std::string_view("x64"), std::string_view("x86"), std::string_view("arm64")})
        for (auto& folder : vst3ArchFolders(os, a, machine))
            if (fs::is_regular_file(bundle / "Contents" / folder / name, ec)) return std::string(a);
    return {};
}

}  // namespace brack
