#include <windows.h>

#include "plugin/host_window.h"
#include "util/common.h"

EXTERN_C IMAGE_DOS_HEADER __ImageBase;

namespace brack {

namespace {

const wchar_t* kEditorClass = L"BrackPluginEditor";

class Win32HostWindow final : public HostWindow {
public:
    Win32HostWindow(Callbacks cb, Placement placement) : cb_(std::move(cb)), placement_(std::move(placement)) {}

    ~Win32HostWindow() override {
        if (hwnd_) {
            SetWindowLongPtrW(hwnd_, GWLP_USERDATA, 0);
            DestroyWindow(hwnd_);
        }
    }

    bool open(const std::string& title, bool resizable, std::string& error) {
        registerClass();
        resizable_ = resizable;
        HWND parent = static_cast<HWND>(placement_.parent);
        if (parent && !IsWindow(parent)) {
            error = "the parent window does not exist";
            return false;
        }
        // No WM_PARENTNOTIFY: the parent belongs to another thread, which may be waiting for ours.
        const DWORD exStyle = parent ? WS_EX_NOPARENTNOTIFY | WS_EX_CONTROLPARENT : 0;
        const int x = parent ? 0 : CW_USEDEFAULT, y = parent ? 0 : CW_USEDEFAULT;
        hwnd_ = CreateWindowExW(exStyle, kEditorClass, widen(title).c_str(), style(), x, y, 400, 300, parent, nullptr,
                                reinterpret_cast<HINSTANCE>(&__ImageBase), nullptr);
        if (!hwnd_) {
            error = "could not create editor window";
            return false;
        }
        SetWindowLongPtrW(hwnd_, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
        return true;
    }

    void* nativeHandle() const override { return hwnd_; }
    double scale() const override { return GetDpiForWindow(hwnd_) / 96.0; }

    void setClientSize(uint32_t w, uint32_t h) override {
        RECT r{0, 0, (LONG)w, (LONG)h};
        if (!child()) AdjustWindowRectExForDpi(&r, style(), FALSE, 0, GetDpiForWindow(hwnd_));
        inHostResize_ = true;
        SetWindowPos(hwnd_, nullptr, 0, 0, r.right - r.left, r.bottom - r.top, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
        inHostResize_ = false;
        reportSize();
    }

    void setResizable(bool resizable) override {
        if (resizable == resizable_) return;
        RECT rc;
        GetClientRect(hwnd_, &rc);
        resizable_ = resizable;
        SetWindowLongPtrW(hwnd_, GWL_STYLE, style() | (IsWindowVisible(hwnd_) ? WS_VISIBLE : 0));
        setClientSize((uint32_t)rc.right, (uint32_t)rc.bottom);
    }

    void show() override {
        if (child()) {
            ShowWindow(hwnd_, SW_SHOWNA);  // the application decides what has the focus
            return;
        }
        ShowWindow(hwnd_, SW_SHOWNORMAL);
        SetForegroundWindow(hwnd_);
    }
    void hide() override { ShowWindow(hwnd_, SW_HIDE); }
    void abandon() override {
        cb_ = {};
        ShowWindow(hwnd_, SW_HIDE);
    }
    void setTitle(const std::string& title) override { SetWindowTextW(hwnd_, widen(title).c_str()); }

private:
    bool child() const { return placement_.parent != nullptr; }

    DWORD style() const {
        if (child()) return WS_CHILD | WS_CLIPCHILDREN | WS_CLIPSIBLINGS;
        DWORD s = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_CLIPCHILDREN;
        if (resizable_) s |= WS_THICKFRAME | WS_MAXIMIZEBOX;
        return s;
    }

    static void registerClass() {
        static bool done = false;
        if (done) return;
        WNDCLASSEXW wc{sizeof(wc)};
        wc.style = CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc = &Win32HostWindow::wndProc;
        wc.hInstance = reinterpret_cast<HINSTANCE>(&__ImageBase);
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
        wc.lpszClassName = kEditorClass;
        RegisterClassExW(&wc);
        done = true;
    }

    void reportSize() {
        if (!placement_.sized || !hwnd_) return;
        RECT rc;
        GetClientRect(hwnd_, &rc);
        placement_.sized((uint32_t)(rc.right - rc.left), (uint32_t)(rc.bottom - rc.top));
    }

    void onUserResize() {
        if (inHostResize_) return;
        if (!resizable_ || !cb_.resized) {
            reportSize();
            return;
        }
        RECT rc;
        GetClientRect(hwnd_, &rc);
        uint32_t w = (uint32_t)(rc.right - rc.left), h = (uint32_t)(rc.bottom - rc.top);
        if (w == 0 || h == 0) return;
        uint32_t aw = w, ah = h;
        cb_.resized(aw, ah);
        if (hwnd_ && (aw != w || ah != h)) setClientSize(aw, ah);
        else reportSize();
    }

    static LRESULT CALLBACK wndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
        auto* self = reinterpret_cast<Win32HostWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (self) {
            switch (msg) {
                case WM_CLOSE:
                    // Not DestroyWindow: the owner tears the editor down, and not from in here.
                    if (self->cb_.closeRequested) self->cb_.closeRequested();
                    return 0;
                case WM_DESTROY:
                    // Destroyed along with the application's parent window, not by us: the
                    // editor has to go too.
                    self->hwnd_ = nullptr;
                    if (self->cb_.closeRequested) self->cb_.closeRequested();
                    break;
                case WM_SIZE:
                    if (wp != SIZE_MINIMIZED) self->onUserResize();
                    break;
                case WM_DPICHANGED: {
                    if (self->cb_.scaleChanged) self->cb_.scaleChanged(HIWORD(wp) / 96.0);
                    const RECT* r = reinterpret_cast<const RECT*>(lp);
                    SetWindowPos(hwnd, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top,
                                 SWP_NOZORDER | SWP_NOACTIVATE);
                    return 0;
                }
                default: break;
            }
        }
        return DefWindowProcW(hwnd, msg, wp, lp);
    }

    Callbacks cb_;
    Placement placement_;
    HWND hwnd_ = nullptr;
    bool resizable_ = false;
    bool inHostResize_ = false;
};

}  // namespace

HostWindow::Api HostWindow::api() { return Api::Win32; }

bool HostWindow::available(std::string&) { return true; }

std::unique_ptr<HostWindow> HostWindow::create(const std::string& title, bool resizable, Callbacks cb,
                                               const Placement& placement, std::string& error) {
    auto w = std::make_unique<Win32HostWindow>(std::move(cb), placement);
    if (!w->open(title, resizable, error)) return nullptr;
    return w;
}

ParentDpiScope::ParentDpiScope(void* parent) {
    if (!parent || !IsWindow(static_cast<HWND>(parent))) return;
    previous_ = SetThreadDpiAwarenessContext(GetWindowDpiAwarenessContext(static_cast<HWND>(parent)));
}

ParentDpiScope::~ParentDpiScope() {
    if (previous_) SetThreadDpiAwarenessContext(static_cast<DPI_AWARENESS_CONTEXT>(previous_));
}

}  // namespace brack
