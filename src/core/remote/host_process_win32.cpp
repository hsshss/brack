// Windows: Brack's end of a plugin host process. A named pipe, a file mapping for the control
// block, two events, and the process in a job that ends with Brack. See remote/host_process.h.
#include <windows.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>

#include "remote/host_process.h"
#include "remote/plugin_host.h"
#include "util/common.h"
#include "util/realtime_win32.h"

namespace brack {

using remote::ControlBlock;
using remote::Message;

std::filesystem::path pluginHostExecutable(const std::string& architecture) {
    HMODULE self = nullptr;
    wchar_t path[MAX_PATH * 4] = {};
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&pluginHostExecutable), &self);
    GetModuleFileNameW(self, path, (DWORD)std::size(path));
    return std::filesystem::path(path).parent_path() / pathFromUtf8("brack-host-" + architecture + ".exe");
}

namespace {

// Every plugin host process is in this job, which ends them all when Brack's process ends,
// however it ends.
HANDLE hostJob() {
    static HANDLE job = [] {
        HANDLE j = CreateJobObjectW(nullptr, nullptr);
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags =
            JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE | JOB_OBJECT_LIMIT_DIE_ON_UNHANDLED_EXCEPTION;
        if (j) SetInformationJobObject(j, JobObjectExtendedLimitInformation, &limits, sizeof limits);
        return j;
    }();
    return job;
}

// Waits for `h`, answering messages other threads send to this one meanwhile (see
// HostThread::invoke). WAIT_OBJECT_0 or WAIT_TIMEOUT.
DWORD waitPumping(HANDLE h, DWORD ms) {
    const ULONGLONG until = GetTickCount64() + ms;
    for (;;) {
        const ULONGLONG now = GetTickCount64();
        const DWORD left = now >= until ? 0 : (DWORD)(until - now);
        const DWORD w = MsgWaitForMultipleObjectsEx(1, &h, left, QS_SENDMESSAGE, 0);
        if (w != WAIT_OBJECT_0 + 1) return w == WAIT_OBJECT_0 ? WAIT_OBJECT_0 : WAIT_TIMEOUT;
        MSG msg;
        PeekMessageW(&msg, nullptr, 0, 0, PM_NOREMOVE | PM_QS_SENDMESSAGE);
    }
}

// One plugin host process.
class Win32HostProcess final : public HostProcess {
public:
    static std::unique_ptr<HostProcess> create(const std::string& architecture, std::string& error) {
        if (!architectureRunsHere(architecture)) {
            error = "this computer does not run " + architecture + " programs";
            return nullptr;
        }
        const std::filesystem::path exe = pluginHostExecutable(architecture);
        std::error_code ec;
        if (!std::filesystem::is_regular_file(exe, ec)) {
            error = "no plugin host for " + architecture + " plugins (" + pathToUtf8(exe.filename()) +
                    " is not installed beside Brack)";
            return nullptr;
        }
        auto p = std::unique_ptr<Win32HostProcess>(new Win32HostProcess);
        if (!p->launch(exe, error)) return nullptr;
        return p;
    }

    ~Win32HostProcess() override {
        unmapOutputs();
        if (pipe_) CloseHandle(pipe_);  // the host sees the pipe close and ends
        if (process_) {
            if (WaitForSingleObject(process_, 2000) != WAIT_OBJECT_0) TerminateProcess(process_, 1);
            CloseHandle(process_);
        }
        if (control_) UnmapViewOfFile(control_);
        for (HANDLE h : {controlMapping_, processEvent_, doneEvent_, ioEvent_})
            if (h) CloseHandle(h);
    }

    Outcome call(const Message& request, Message& reply, std::chrono::milliseconds timeout) override {
        const ULONGLONG until = GetTickCount64() + (ULONGLONG)timeout.count();
        const std::vector<uint8_t> frame = remote::encodeMessage(request);
        Outcome o = io(true, const_cast<uint8_t*>(frame.data()), (DWORD)frame.size(), until);
        while (o == Outcome::Ok) {
            Message m;
            o = readMessage(m, until);
            if (o != Outcome::Ok) break;
            if (m.contains("reply")) {
                reply = std::move(m);
                return Outcome::Ok;
            }
            note(m);
        }
        if (o == Outcome::TimedOut) terminate();
        return o;
    }

    // A note that does not arrive whole ends the host: the stream would be out of step.
    void pollNotes() override {
        DWORD available = 0;
        while (PeekNamedPipe(pipe_, nullptr, 0, nullptr, &available, nullptr) && available >= 4) {
            Message m;
            if (readMessage(m, GetTickCount64() + 2000) != Outcome::Ok) {
                terminate();
                return;
            }
            note(m);
        }
    }

    bool running() override { return WaitForSingleObject(process_, 0) == WAIT_TIMEOUT; }
    void terminate() override { TerminateProcess(process_, 1); }

    std::string endReport() override {
        if (WaitForSingleObject(process_, 1000) != WAIT_OBJECT_0) return "stopped answering";
        if (control_->crashed) {
            if (control_->crashReport[0]) return std::string(control_->crashReport, strnlen(control_->crashReport, sizeof control_->crashReport));
            return faultReport("a thread or window of its own", {control_->crashCode, 0});
        }
        DWORD code = 0;
        GetExitCodeProcess(process_, &code);
        if (code == remote::kExitWrongVersion) return "the plugin host is from another build of Brack (build both architectures again)";
        char buf[96];
        if (code >= 0xC0000000) {
            const char* name = faultName(code);
            std::snprintf(buf, sizeof buf, "crashed: %s (0x%08lX)", *name ? name : "exception", (unsigned long)code);
        } else {
            std::snprintf(buf, sizeof buf, "ended unexpectedly (exit code %lu)", (unsigned long)code);
        }
        return buf;
    }

    ControlBlock& control() override { return *control_; }

    void signalBlock() override { SetEvent(processEvent_); }

    Outcome waitBlock(uint32_t timeoutMs) override {
        HANDLE waits[] = {doneEvent_, process_};
        const DWORD w = WaitForMultipleObjects(2, waits, FALSE, timeoutMs);
        return w == WAIT_OBJECT_0 ? Outcome::Ok : w == WAIT_OBJECT_0 + 1 ? Outcome::Ended : Outcome::TimedOut;
    }

    // `ref`: the handle in the host's process.
    float* mapOutputs(const Message& ref, uint64_t bytes) override {
        unmapOutputs();
        const uint64_t handleInHost = ref.is_number_unsigned() ? ref.get<uint64_t>() : 0;
        if (!handleInHost || !bytes) return nullptr;
        if (!DuplicateHandle(process_, (HANDLE)(uintptr_t)handleInHost, GetCurrentProcess(), &outputs_, 0, FALSE,
                             DUPLICATE_SAME_ACCESS))
            return nullptr;
        outputsView_ = MapViewOfFile(outputs_, FILE_MAP_ALL_ACCESS, 0, 0, (SIZE_T)bytes);
        return static_cast<float*>(outputsView_);
    }
    void unmapOutputs() override {
        if (outputsView_) UnmapViewOfFile(outputsView_);
        if (outputs_) CloseHandle(outputs_);
        outputsView_ = nullptr;
        outputs_ = nullptr;
    }

    void allowForeground() override { AllowSetForegroundWindow(GetProcessId(process_)); }

private:
    Win32HostProcess() = default;

    bool launch(const std::filesystem::path& exe, std::string& error) {
        auto fail = [&](const char* what) {
            error = std::string("cannot start the plugin host: ") + what + " failed (" + std::to_string(GetLastError()) + ")";
            return false;
        };
        static std::atomic<uint32_t> counter{0};
        wchar_t name[96];
        std::swprintf(name, std::size(name), L"\\\\.\\pipe\\brack-host-%lu-%lu-%llu", GetCurrentProcessId(),
                      (unsigned long)++counter, (unsigned long long)GetTickCount64());
        pipe_ = CreateNamedPipeW(name, PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
                                 PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, 1, 1 << 20,
                                 1 << 20, 0, nullptr);
        if (pipe_ == INVALID_HANDLE_VALUE) {
            pipe_ = nullptr;
            return fail("CreateNamedPipe");
        }
        SECURITY_ATTRIBUTES inherit{sizeof inherit, nullptr, TRUE};
        HANDLE client = CreateFileW(name, GENERIC_READ | GENERIC_WRITE, 0, &inherit, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
        if (client == INVALID_HANDLE_VALUE) return fail("opening the pipe");
        controlMapping_ = CreateFileMappingW(INVALID_HANDLE_VALUE, &inherit, PAGE_READWRITE, 0, sizeof(ControlBlock), nullptr);
        processEvent_ = CreateEventW(&inherit, FALSE, FALSE, nullptr);
        doneEvent_ = CreateEventW(&inherit, FALSE, FALSE, nullptr);
        ioEvent_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!controlMapping_ || !processEvent_ || !doneEvent_ || !ioEvent_) {
            CloseHandle(client);
            return fail("creating the shared memory");
        }
        control_ = static_cast<ControlBlock*>(MapViewOfFile(controlMapping_, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(ControlBlock)));
        if (!control_) {
            CloseHandle(client);
            return fail("MapViewOfFile");
        }

        // Only these handles go to the host, whatever else of Brack's is inheritable.
        HANDLE handles[] = {client, controlMapping_, processEvent_, doneEvent_};
        SIZE_T size = 0;
        InitializeProcThreadAttributeList(nullptr, 1, 0, &size);
        std::vector<uint8_t> attrStorage(size);
        auto* attrs = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attrStorage.data());
        InitializeProcThreadAttributeList(attrs, 1, 0, &size);
        UpdateProcThreadAttribute(attrs, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, handles, sizeof handles, nullptr, nullptr);
        STARTUPINFOEXW si{};
        si.StartupInfo.cb = sizeof si;
        si.lpAttributeList = attrs;

        std::wstring cmd = L"\"" + exe.wstring() + L"\" --brack-host " + std::to_wstring(remote::kProtocolVersion);
        for (HANDLE h : handles) cmd += L" " + std::to_wstring((uintptr_t)h);
        PROCESS_INFORMATION pi{};
        const BOOL created =
            CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, TRUE,
                           CREATE_SUSPENDED | CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT, nullptr, nullptr,
                           &si.StartupInfo, &pi);
        DeleteProcThreadAttributeList(attrs);
        CloseHandle(client);  // the host holds the only other end now: its end breaks when it ends
        if (!created) return fail("CreateProcess");
        if (!AssignProcessToJobObject(hostJob(), pi.hProcess))
            logWarn("plugin host: not in Brack's job (" + std::to_string(GetLastError()) +
                    "), so it may outlive a Brack that ends abruptly");
        ResumeThread(pi.hThread);
        CloseHandle(pi.hThread);
        process_ = pi.hProcess;
        // Not inherited by later children of Brack's.
        for (HANDLE h : {controlMapping_, processEvent_, doneEvent_}) SetHandleInformation(h, HANDLE_FLAG_INHERIT, 0);
        return true;
    }

    // The host tells what came of its audio thread joining MMCSS (BrackLink::raiseAudioThreadPriority()).
    void note(const Message& m) {
        if (m.value("note", "") == "audioThread") {
            const DWORD error = m.value("error", (DWORD)0);
            realtime::report("plugin hosts' audio threads",
                             error ? "AvSetMmThreadCharacteristics failed (" + std::to_string(error) + ")" : "");
            return;
        }
        if (onNote) onNote(m);
    }

    Outcome readMessage(Message& m, ULONGLONG until) {
        uint8_t len[4];
        Outcome o = io(false, len, 4, until);
        if (o != Outcome::Ok) return o;
        const uint32_t n = len[0] | len[1] << 8 | len[2] << 16 | (uint32_t)len[3] << 24;
        if (n > remote::kMaxMessageBytes) {
            terminate();  // not something a host of this version would send
            return Outcome::Ended;
        }
        std::vector<uint8_t> body(n);
        o = io(false, body.data(), (DWORD)body.size(), until);
        if (o != Outcome::Ok) return o;
        if (!remote::decodeMessage(body, m)) {
            terminate();
            return Outcome::Ended;
        }
        return Outcome::Ok;
    }

    Outcome io(bool write, uint8_t* data, DWORD n, ULONGLONG until) {
        while (n > 0) {
            OVERLAPPED ov{};
            ov.hEvent = ioEvent_;
            ResetEvent(ioEvent_);
            BOOL ok = write ? WriteFile(pipe_, data, n, nullptr, &ov) : ReadFile(pipe_, data, n, nullptr, &ov);
            if (!ok && GetLastError() != ERROR_IO_PENDING) return Outcome::Ended;
            const ULONGLONG now = GetTickCount64();
            if (waitPumping(ioEvent_, now >= until ? 0 : (DWORD)(until - now)) != WAIT_OBJECT_0) {
                CancelIoEx(pipe_, &ov);
                DWORD ignored;
                GetOverlappedResult(pipe_, &ov, &ignored, TRUE);  // the OVERLAPPED is ours again
                return Outcome::TimedOut;
            }
            DWORD done = 0;
            if (!GetOverlappedResult(pipe_, &ov, &done, FALSE) || done == 0) return Outcome::Ended;
            data += done;
            n -= done;
        }
        return Outcome::Ok;
    }

    HANDLE process_ = nullptr;
    HANDLE pipe_ = nullptr;
    HANDLE controlMapping_ = nullptr;
    HANDLE processEvent_ = nullptr, doneEvent_ = nullptr;
    HANDLE ioEvent_ = nullptr;
    ControlBlock* control_ = nullptr;
    HANDLE outputs_ = nullptr;
    void* outputsView_ = nullptr;
};

}  // namespace

std::unique_ptr<HostProcess> HostProcess::start(const std::string& architecture, std::string& error) {
    return Win32HostProcess::create(architecture, error);
}

}  // namespace brack
