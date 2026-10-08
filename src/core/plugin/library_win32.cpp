// Windows: plugin crashes are structured exceptions (SEH).
#include <malloc.h>  // _resetstkoflw
#include <windows.h>

#include <cstdio>

#include "plugin/library.h"
#include "util/common.h"

namespace brack {

namespace {
int captureFault(const EXCEPTION_POINTERS* ep, GuardFault* fault) {
    if (fault) {
        fault->code = ep->ExceptionRecord->ExceptionCode;
        fault->address = (uint64_t)(uintptr_t)ep->ExceptionRecord->ExceptionAddress;
    }
    return EXCEPTION_EXECUTE_HANDLER;
}

// No C++ objects with destructors in here: __try cannot share a function with them.
bool runGuardedSeh(void (*fn)(void*), void* ctx, GuardFault* fault) {
    __try {
        fn(ctx);
        return true;
    } __except (captureFault(GetExceptionInformation(), fault)) {
        return false;
    }
}
}  // namespace

bool runGuarded(void (*fn)(void*), void* ctx, GuardFault* fault) {
    GuardFault local;
    if (!fault) fault = &local;
    if (runGuardedSeh(fn, ctx, fault)) return true;
    // The guard page is gone after an overflow; put it back, or the next one is fatal.
    if (fault->code == EXCEPTION_STACK_OVERFLOW) _resetstkoflw();
    return false;
}

const char* faultName(uint32_t code) {
    switch (code) {
        case EXCEPTION_ACCESS_VIOLATION: return "access violation";
        case EXCEPTION_STACK_OVERFLOW: return "stack overflow";
        case EXCEPTION_ILLEGAL_INSTRUCTION: return "illegal instruction";
        case EXCEPTION_PRIV_INSTRUCTION: return "privileged instruction";
        case EXCEPTION_INT_DIVIDE_BY_ZERO: return "integer division by zero";
        case EXCEPTION_INT_OVERFLOW: return "integer overflow";
        case EXCEPTION_IN_PAGE_ERROR: return "in-page error";
        case EXCEPTION_DATATYPE_MISALIGNMENT: return "misaligned access";
        case EXCEPTION_BREAKPOINT: return "breakpoint";
        case 0xE06D7363: return "uncaught C++ exception";
        case 0xC0000409: return "fast fail";  // __fastfail: a detected heap or stack corruption, say
        default: return "";
    }
}

std::string faultLocation(uint64_t address) {
    HMODULE mod = nullptr;
    wchar_t path[MAX_PATH] = {};
    char buf[64];
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>((uintptr_t)address), &mod) &&
        GetModuleFileNameW(mod, path, MAX_PATH)) {
        std::snprintf(buf, sizeof buf, "+0x%llx", (unsigned long long)(address - (uint64_t)(uintptr_t)mod));
        return pathToUtf8(std::filesystem::path(path).filename()) + buf;
    }
    std::snprintf(buf, sizeof buf, "0x%llx", (unsigned long long)address);
    return buf;
}

}  // namespace brack
