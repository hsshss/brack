// Windows: the plugin host's end of its link to Brack. A named pipe, a file mapping for the
// control block and one per activation for the outputs, two events; all handles Brack gave.
#include <windows.h>
#include <avrt.h>
#include <werapi.h>

#include <cstdlib>
#include <cstring>
#include <mutex>

#include "plugin/library.h"
#include "remote/brack_link.h"

namespace brack {

using remote::ControlBlock;
using remote::Message;

namespace {

ControlBlock* g_control = nullptr;  // for the crash record

void writeCrashRecord(uint32_t code, const std::string& report) {
    ControlBlock& c = *g_control;
    c.crashCode = code;
    c.crashed = 1;
    const size_t n = std::min(report.size(), sizeof c.crashReport - 1);
    std::memcpy(c.crashReport, report.data(), n);
    c.crashReport[n] = 0;
}

// A crash nothing caught: on a thread of the plugin's own, or in its window procedures.
LONG WINAPI onUncaughtException(EXCEPTION_POINTERS* ep) {
    const GuardFault fault{(uint32_t)ep->ExceptionRecord->ExceptionCode,
                           (uint64_t)(uintptr_t)ep->ExceptionRecord->ExceptionAddress};
    g_control->crashCode = fault.code;
    g_control->crashed = 1;  // enough for a report, should building the text fail in a broken process
    writeCrashRecord(fault.code, faultReport("a thread or window of its own", fault));
    return EXCEPTION_EXECUTE_HANDLER;  // the process ends with the exception code
}

HANDLE handleArg(const std::string& s) { return (HANDLE)(uintptr_t)std::strtoull(s.c_str(), nullptr, 10); }

}  // namespace

struct BrackLink::Impl {
    HANDLE pipe = nullptr;
    HANDLE processEvent = nullptr, doneEvent = nullptr;
    HANDLE outputs = nullptr;
    void* outputsView = nullptr;
    std::mutex writeMutex;

    // Overlapped, so that notes can be written while the request loop waits to read (synchronous
    // I/O on one handle would serialise the two). Any thread: an event of its own per call.
    bool io(bool write, uint8_t* data, DWORD n) {
        HANDLE event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!event) return false;
        bool ok = true;
        while (ok && n > 0) {
            OVERLAPPED ov{};
            ov.hEvent = event;
            DWORD done = 0;
            const BOOL started = write ? WriteFile(pipe, data, n, nullptr, &ov) : ReadFile(pipe, data, n, nullptr, &ov);
            ok = (started || GetLastError() == ERROR_IO_PENDING) && GetOverlappedResult(pipe, &ov, &done, TRUE) && done > 0;
            data += done;
            n -= done;
        }
        CloseHandle(event);
        return ok;
    }
};

std::unique_ptr<BrackLink> BrackLink::open(const std::vector<std::string>& args, int& exitCode) {
    // --brack-host <version> <pipe> <control block> <process event> <done event>
    exitCode = remote::kExitBadArguments;
    if (args.size() != 6 || args[0] != "--brack-host") return nullptr;
    if (std::strtoul(args[1].c_str(), nullptr, 10) != remote::kProtocolVersion) {
        exitCode = remote::kExitWrongVersion;
        return nullptr;
    }
    g_control = static_cast<ControlBlock*>(MapViewOfFile(handleArg(args[3]), FILE_MAP_ALL_ACCESS, 0, 0, sizeof(ControlBlock)));
    if (!g_control) return nullptr;
    auto impl = std::make_unique<Impl>();
    impl->pipe = handleArg(args[2]);
    impl->processEvent = handleArg(args[4]);
    impl->doneEvent = handleArg(args[5]);
    return std::unique_ptr<BrackLink>(new BrackLink(std::move(impl)));
}

BrackLink::BrackLink(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
BrackLink::~BrackLink() = default;

bool BrackLink::receive(Message& m) {
    uint8_t len[4];
    if (!impl_->io(false, len, 4)) return false;
    const uint32_t n = len[0] | len[1] << 8 | len[2] << 16 | (uint32_t)len[3] << 24;
    if (n > remote::kMaxMessageBytes) return false;
    std::vector<uint8_t> body(n);
    return impl_->io(false, body.data(), (DWORD)body.size()) && remote::decodeMessage(body, m);
}

bool BrackLink::send(const Message& m) {
    const std::vector<uint8_t> frame = remote::encodeMessage(m);
    std::lock_guard lock(impl_->writeMutex);
    return impl_->io(true, const_cast<uint8_t*>(frame.data()), (DWORD)frame.size());
}

ControlBlock& BrackLink::control() { return *g_control; }

bool BrackLink::waitForBlock() { return WaitForSingleObject(impl_->processEvent, INFINITE) == WAIT_OBJECT_0; }

void BrackLink::blockDone() { SetEvent(impl_->doneEvent); }

// Brack logs what came of it (util/realtime_win32.h).
void BrackLink::raiseAudioThreadPriority(double, uint32_t) {
    thread_local bool raised = false;
    if (raised) return;
    raised = true;
    DWORD task = 0;
    const DWORD error = AvSetMmThreadCharacteristicsW(L"Pro Audio", &task) ? 0 : GetLastError();
    send({{"note", "audioThread"}, {"error", error}});
}

float* BrackLink::createOutputs(uint64_t bytes, Message& ref) {
    if (impl_->outputsView) UnmapViewOfFile(impl_->outputsView);
    if (impl_->outputs) CloseHandle(impl_->outputs);
    impl_->outputsView = nullptr;
    impl_->outputs = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, (DWORD)(bytes >> 32), (DWORD)bytes, nullptr);
    if (!impl_->outputs) return nullptr;
    impl_->outputsView = MapViewOfFile(impl_->outputs, FILE_MAP_ALL_ACCESS, 0, 0, 0);
    ref = (uint64_t)(uintptr_t)impl_->outputs;  // Brack duplicates the handle into its process
    return static_cast<float*>(impl_->outputsView);
}

void BrackLink::recordUncaughtCrashes() {
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    WerSetFlags(WER_FAULT_REPORTING_NO_UI);
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
    SetUnhandledExceptionFilter(&onUncaughtException);
}

void BrackLink::protectThisThread() {}

void BrackLink::endAfterCrash(const std::string& report) {
    writeCrashRecord(0, report);
    TerminateProcess(GetCurrentProcess(), 1);
    for (;;) Sleep(INFINITE);
}

void BrackLink::end() {
    TerminateProcess(GetCurrentProcess(), 0);
    for (;;) Sleep(INFINITE);
}

}  // namespace brack
