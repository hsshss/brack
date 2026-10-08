#pragma once
// Shared-library loading for the plugin formats.

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <type_traits>
#include <vector>

namespace brack {

void* openLibrary(const std::string& pathUtf8, std::string& error);
void* librarySymbol(void* lib, const char* name);
void closeLibrary(void* lib);

// A binary's headers, read without loading it (so none of its code runs): PE, ELF or Mach-O,
// whatever OS Brack runs on (plugin/binary_info.cpp).
struct BinaryInfo {
    // buildArchitecture() names ("x64", "x86", "arm64") of the code it holds: one, or several in
    // a universal Mach-O. Others read "ELF machine 0x28", say.
    std::vector<std::string> architectures;
    std::vector<std::string> exports;  // exported function names (from the image Brack would load)
    bool loadable = false;             // this process could load it: its OS's format, with its architecture
    // The architecture it runs as: Brack's own if it has that, else its first.
    std::string architecture() const;
    bool hasExport(const char* name) const;
};
bool inspectBinary(const std::filesystem::path& path, BinaryInfo& out, std::string& error);

// What a guarded call raised instead of returning.
struct GuardFault {
    uint32_t code = 0;     // exception code (0xC0000005 for an access violation)
    uint64_t address = 0;  // where it was raised
};

// Runs fn(ctx); false if it crashed instead of returning (an access violation or segmentation
// fault, a stack overflow, a C++ exception it did not catch), described in `fault`: a structured
// exception on Windows, a signal elsewhere (plugin/library_posix.cpp). For calls into plugins: a
// crashing plugin must not take brack down. Nothing between the fault and this call is unwound,
// so whatever fn's callees held or allocated stays so.
bool runGuarded(void (*fn)(void*), void* ctx, GuardFault* fault = nullptr);
// runGuarded() for a callable: guardedCall([&] { plugin->call(); }, fault).
template <typename F>
bool guardedCall(F&& f, GuardFault& fault) {
    using Fn = std::remove_reference_t<F>;
    return runGuarded([](void* p) { (*static_cast<Fn*>(p))(); }, &f, &fault);
}
// "access violation", "segmentation fault", ... for an exception code or signal; "" if not a
// known one.
const char* faultName(uint32_t code);
// "Module.dll+0x1234" for an address in a loaded module, else the bare address.
std::string faultLocation(uint64_t address);
// "crashed in <what>: access violation (0xC0000005) at Module.dll+0x1234"
std::string faultReport(const char* what, const GuardFault& fault);
#ifndef _WIN32
// Where a signal was raised: the instruction, from the thread's context (a ucontext_t), where we
// know how to read it; the faulting address (siginfo_t's) otherwise. For a signal handler.
uint64_t signalFaultAddress(const void* info, const void* context);
#endif

// Plugin modules (by their normalised path) whose code crashed while being loaded, asked for
// their plugins or unloaded. Such a module is never called again for the rest of the process,
// nor unloaded: `keep` (the module object, if there still is one) is held forever. Loading it
// again fails with the report instead. Any thread.
void markModuleBroken(const std::string& key, const std::string& report, std::shared_ptr<void> keep);
// True (and why, in `error`) if the module at `key` crashed before.
bool moduleBroken(const std::string& key, std::string& error);

}  // namespace brack
