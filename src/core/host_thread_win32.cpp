#include "host_thread.h"

#include <windows.h>
#include <ole2.h>

#include <deque>
#include <exception>
#include <future>
#include <map>
#include <mutex>
#include <stdexcept>
#include <thread>

EXTERN_C IMAGE_DOS_HEADER __ImageBase;

namespace brack {

namespace {
constexpr UINT kWakeMessage = WM_APP + 1;
constexpr UINT kQuitMessage = WM_APP + 2;
const wchar_t* kWindowClass = L"BrackHostThreadWindow";
}  // namespace

struct HostThread::Impl {
    std::thread thread;
    DWORD threadId = 0;
    HWND hwnd = nullptr;

    std::mutex mutex;
    std::deque<std::function<void()>> queue;
    std::map<uint32_t, std::function<void()>> timers;  // host thread only
    uint32_t nextTimerId = 1;                          // host thread only

    void drain() {
        for (;;) {
            std::function<void()> fn;
            {
                std::lock_guard lock(mutex);
                if (queue.empty()) return;
                fn = std::move(queue.front());
                queue.pop_front();
            }
            fn();
        }
    }

    static LRESULT CALLBACK wndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
        auto* self = reinterpret_cast<Impl*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (self) {
            if (msg == kWakeMessage) {
                self->drain();
                return 0;
            }
            if (msg == WM_TIMER) {
                auto it = self->timers.find((uint32_t)wp);
                if (it != self->timers.end()) {
                    auto fn = it->second;  // copy: fn may remove its own timer
                    fn();
                }
                return 0;
            }
            if (msg == kQuitMessage) {
                PostQuitMessage(0);
                return 0;
            }
        }
        return DefWindowProcW(hwnd, msg, wp, lp);
    }

    void run(std::promise<void>* ready) {
        threadId = GetCurrentThreadId();
        // Plugin editors are created on this thread, with the process's DPI awareness: plugins
        // (JUCE's) scale by the process's, and drew at the wrong size when only this thread was
        // per-monitor aware. Brack's programs are per-monitor aware; an application decides for
        // brack.dll.
        OleInitialize(nullptr);  // some plugin GUIs need OLE (drag & drop)

        WNDCLASSEXW wc{sizeof(wc)};
        wc.lpfnWndProc = &Impl::wndProc;
        wc.hInstance = reinterpret_cast<HINSTANCE>(&__ImageBase);
        wc.lpszClassName = kWindowClass;
        RegisterClassExW(&wc);  // fails harmlessly if already registered
        hwnd = CreateWindowExW(0, kWindowClass, L"brack", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, wc.hInstance, nullptr);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
        ready->set_value();

        MSG msg;
        while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        drain();
        for (auto& [id, fn] : timers) KillTimer(hwnd, id);
        timers.clear();
        DestroyWindow(hwnd);
        hwnd = nullptr;
        OleUninitialize();
    }

    void wake() { PostMessageW(hwnd, kWakeMessage, 0, 0); }
};

HostThread::HostThread() : impl_(std::make_unique<Impl>()) {
    std::promise<void> ready;
    auto fut = ready.get_future();
    impl_->thread = std::thread([this, &ready] { impl_->run(&ready); });
    fut.wait();
}

HostThread::~HostThread() {
    PostMessageW(impl_->hwnd, kQuitMessage, 0, 0);
    impl_->thread.join();
}

bool HostThread::isCurrent() const { return GetCurrentThreadId() == impl_->threadId; }

void HostThread::post(std::function<void()> fn) {
    {
        std::lock_guard lock(impl_->mutex);
        impl_->queue.push_back(std::move(fn));
    }
    impl_->wake();
}

void HostThread::invoke(const std::function<void()>& fn) {
    if (isCurrent()) {
        fn();
        return;
    }
    HANDLE done = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!done) throw std::runtime_error("CreateEvent failed");
    std::exception_ptr error;
    post([&] {
        try {
            fn();
        } catch (...) {
            error = std::current_exception();
        }
        SetEvent(done);
    });
    // Keep answering messages other threads send to this thread's windows while waiting: a
    // plugin editor embedded in a window of the caller's makes the host thread send to that
    // window, and neither side may wait for the other.
    while (MsgWaitForMultipleObjectsEx(1, &done, INFINITE, QS_SENDMESSAGE, 0) != WAIT_OBJECT_0) {
        MSG msg;
        PeekMessageW(&msg, nullptr, 0, 0, PM_NOREMOVE | PM_QS_SENDMESSAGE);
    }
    CloseHandle(done);
    if (error) std::rethrow_exception(error);
}

uint32_t HostThread::addTimer(uint32_t periodMs, std::function<void()> fn) {
    uint32_t id = 0;
    invoke([&] {
        id = impl_->nextTimerId++;
        impl_->timers[id] = std::move(fn);
        SetTimer(impl_->hwnd, id, periodMs, nullptr);
    });
    return id;
}

void HostThread::removeTimer(uint32_t id) {
    invoke([&] {
        KillTimer(impl_->hwnd, id);
        impl_->timers.erase(id);
    });
}

}  // namespace brack
