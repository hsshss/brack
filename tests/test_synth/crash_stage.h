#pragma once
// Module-level crashes for the host's crash containment, shared by the CLAP and VST3 test
// plugins. A copy of the plugin whose file name contains "crash-at-<stage>" crashes (an
// access violation) when it reaches crashAt("<stage>"): the stages are calls made while the
// module is loaded, listed or unloaded, before there is any plugin instance to report through.
// The file name decides, so the tests need no shared state with the plugin.

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <dlfcn.h>
#endif

#include <cstring>
#include <string>

inline bool fileNamedFor(const char* stage) {
#ifdef _WIN32
    HMODULE self = nullptr;
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCSTR>(&fileNamedFor), &self))
        return false;
    char path[MAX_PATH * 2];
    const DWORD n = GetModuleFileNameA(self, path, sizeof path);
    if (n == 0 || n >= sizeof path) return false;
#else
    Dl_info info{};
    if (!dladdr(reinterpret_cast<void*>(&fileNamedFor), &info) || !info.dli_fname) return false;
    const char* path = info.dli_fname;
#endif
    return std::strstr(path, (std::string("crash-at-") + stage).c_str()) != nullptr;
}

inline void crashAt(const char* stage) {
    if (!fileNamedFor(stage)) return;
    volatile int* volatile nowhere = nullptr;
    *nowhere = 1;
}
