// Shared-library loading and the record of crashed modules. The guard against crashes and
// the binary inspection are per platform: library_win32.cpp, library_posix.cpp.
#include "plugin/library.h"

#include <algorithm>
#include <cstdio>
#include <map>
#include <mutex>

#include "util/common.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace brack {

void* openLibrary(const std::string& pathUtf8, std::string& error) {
#ifdef _WIN32
    HMODULE h = LoadLibraryExW(widen(pathUtf8).c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!h) {
        DWORD e = GetLastError();
        if (e == ERROR_BAD_EXE_FORMAT) error = "not built for this architecture (a 32-bit plugin?)";
        else error = "LoadLibrary failed (" + std::to_string(e) + ")";
    }
    return h;
#else
    void* h = dlopen(pathUtf8.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!h) error = dlerror();
    return h;
#endif
}

void* librarySymbol(void* lib, const char* name) {
#ifdef _WIN32
    return reinterpret_cast<void*>(GetProcAddress(static_cast<HMODULE>(lib), name));
#else
    return dlsym(lib, name);
#endif
}

void closeLibrary(void* lib) {
#ifdef _WIN32
    FreeLibrary(static_cast<HMODULE>(lib));
#else
    dlclose(lib);
#endif
}

std::string faultReport(const char* what, const GuardFault& fault) {
    char code[16];
    std::snprintf(code, sizeof code, "0x%08X", fault.code);
    const char* name = faultName(fault.code);
    return std::string("crashed in ") + what + ": " + (*name ? name : "exception") + " (" + code + ") at " +
           faultLocation(fault.address);
}

namespace {
struct BrokenModules {
    std::mutex mutex;
    std::map<std::string, std::pair<std::string, std::shared_ptr<void>>> modules;
};
BrokenModules& brokenModules() {
    static auto* b = new BrokenModules;  // never destroyed: what it keeps must not be unloaded
    return *b;
}
}  // namespace

void markModuleBroken(const std::string& key, const std::string& report, std::shared_ptr<void> keep) {
    auto& b = brokenModules();
    std::lock_guard lock(b.mutex);
    auto& entry = b.modules[key];
    if (entry.first.empty()) entry.first = report;
    if (keep) entry.second = std::move(keep);
}

bool moduleBroken(const std::string& key, std::string& error) {
    auto& b = brokenModules();
    std::lock_guard lock(b.mutex);
    auto it = b.modules.find(key);
    if (it == b.modules.end()) return false;
    error = "this plugin crashed earlier (" + it->second.first + "); restart Brack to try it again";
    return true;
}

}  // namespace brack
