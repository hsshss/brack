// Linux: editor windows on X11, the plugin's own window a child of ours (CLAP's x11 window, VST3's
// X11EmbedWindowID, VST2's Window). One connection per host thread, its events handled on that
// thread's loop (HostThread::watchFd()); a plugin has a connection of its own and registers it
// through CLAP posix-fd-support or VST3 IRunLoop. Under Wayland, through Xwayland.
//
// Our window embeds a child that asks for XEmbed (freedesktop.org's XEmbed protocol, version 0, by
// setting _XEMBED_INFO): it is told it is embedded, mapped when it says so, told when our window is
// active, and given the keyboard focus then and when it asks. Other children are left to themselves.
// Editors hear of a new scale when the desktop changes Xft.dpi in the X resources.
// libX11 is loaded when first needed (dlopen), not linked, so that Brack runs where it is not
// installed: without it there are no editors.
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xresource.h>
#include <X11/Xutil.h>

#include <dlfcn.h>
#include <unistd.h>

#include <climits>
#include <cstdlib>
#include <map>
#include <mutex>
#include <set>
#include <vector>

#include "host_thread.h"
#include "plugin/host_window.h"
#include "util/common.h"

namespace brack {

namespace {

class X11HostWindow;

#define BRACK_XLIB_FUNCTIONS(X)                                                                         \
    X(XOpenDisplay) X(XCloseDisplay) X(XSync) X(XFlush) X(XPending) X(XNextEvent) X(XSetErrorHandler)  \
    X(XInternAtom) X(XFree) X(XSelectInput) X(XSendEvent) X(XGetInputFocus) X(XSetInputFocus)          \
    X(XCreateWindow) X(XDestroyWindow) X(XMapWindow) X(XMapRaised) X(XUnmapWindow) X(XResizeWindow)    \
    X(XStoreName) X(XChangeProperty) X(XGetWindowProperty) X(XSetWMProtocols) X(XSetClassHint)         \
    X(XAllocSizeHints) X(XSetWMNormalHints)                                                            \
    X(XrmInitialize) X(XrmGetStringDatabase) X(XrmGetResource) X(XrmDestroyDatabase)

struct Xlib {
#define BRACK_XLIB_MEMBER(name) decltype(&::name) name;
    BRACK_XLIB_FUNCTIONS(BRACK_XLIB_MEMBER)
#undef BRACK_XLIB_MEMBER
};

constexpr const char* kNoXlib = "plugin editors are not available without libX11.so.6";

// Null without libX11.so.6, or with one that lacks a function.
const Xlib* xlib() {
    static const Xlib* loaded = []() -> const Xlib* {
        void* lib = dlopen("libX11.so.6", RTLD_NOW | RTLD_LOCAL);  // kept to the end
        if (!lib) return nullptr;
        static Xlib x;
        bool ok = true;
#define BRACK_XLIB_LOAD(name)                                          \
    x.name = reinterpret_cast<decltype(x.name)>(dlsym(lib, #name)); \
    ok = ok && x.name;
        BRACK_XLIB_FUNCTIONS(BRACK_XLIB_LOAD)
#undef BRACK_XLIB_LOAD
        return ok ? &x : nullptr;
    }();
    return loaded;
}

// XEmbed's messages and flags.
constexpr long kXembedEmbeddedNotify = 0, kXembedWindowActivate = 1, kXembedWindowDeactivate = 2,
               kXembedRequestFocus = 3, kXembedFocusIn = 4, kXembedFocusOut = 5;
constexpr long kXembedFocusCurrent = 0;
constexpr unsigned long kXembedMapped = 1;

// While it exists, errors on `display` are noted instead of ending the process (Xlib's default): a
// plugin may destroy its window between our learning of it and our requests on it. Errors on other
// connections go to the handler there was. One at a time: the handler is the process's.
class ErrorTrap {
public:
    ErrorTrap(const Xlib& x, Display* display) : lock_(s_mutex), x_(x), display_(display) {
        x_.XSync(display, False);
        s_display = display;
        s_failed = false;
        s_previous = x_.XSetErrorHandler(&onError);
    }
    ~ErrorTrap() {
        x_.XSync(display_, False);
        x_.XSetErrorHandler(s_previous);
        s_display = nullptr;
    }
    ErrorTrap(const ErrorTrap&) = delete;
    ErrorTrap& operator=(const ErrorTrap&) = delete;

    bool failed() {
        x_.XSync(display_, False);
        return s_failed;
    }

private:
    static int onError(Display* display, XErrorEvent* e) {
        if (display == s_display) {
            s_failed = true;
            return 0;
        }
        return s_previous ? s_previous(display, e) : 0;
    }

    static inline std::mutex s_mutex;
    static inline Display* s_display = nullptr;
    static inline bool s_failed = false;
    static inline XErrorHandler s_previous = nullptr;
    std::lock_guard<std::mutex> lock_;
    const Xlib& x_;
    Display* display_;
};

// The X resources' Xft.dpi, as desktops set it for scaling; 96 dpi without it. Read from the root
// window, not XResourceManagerString(), which keeps what there was when the connection opened.
double readScale(const Xlib& x, Display* display, Atom resourceManager) {
    double scale = 1.0;
    Atom type = 0;
    int format = 0;
    unsigned long count = 0, after = 0;
    unsigned char* data = nullptr;
    if (x.XGetWindowProperty(display, DefaultRootWindow(display), resourceManager, 0, LONG_MAX / 4, False, XA_STRING,
                             &type, &format, &count, &after, &data) != Success || !data)
        return scale;
    x.XrmInitialize();
    if (XrmDatabase db = x.XrmGetStringDatabase(reinterpret_cast<const char*>(data))) {
        char* valueType = nullptr;
        XrmValue value{};
        if (x.XrmGetResource(db, "Xft.dpi", "Xft.Dpi", &valueType, &value) && value.addr) {
            const double dpi = std::atof(value.addr);
            if (dpi > 0) scale = dpi / 96.0;
        }
        x.XrmDestroyDatabase(db);
    }
    x.XFree(data);
    return scale;
}

// The host thread's connection, open while it has windows.
struct Connection {
    const Xlib* x = nullptr;
    Display* display = nullptr;
    HostThread* thread = nullptr;
    uint32_t watch = 0;
    Atom wmProtocols = 0, wmDelete = 0, netWmName = 0, utf8 = 0, xembed = 0, xembedInfo = 0, resourceManager = 0;
    double scale = 1.0;
    std::map<Window, X11HostWindow*> windows;
    std::map<Window, X11HostWindow*> children;  // the plugins' windows in ours, watched for _XEMBED_INFO

    void dispatch();
    void resourcesChanged();
};

thread_local Connection* t_connection = nullptr;

Connection* connection(std::string& error) {
    if (t_connection) return t_connection;
    const Xlib* x = xlib();
    if (!x) {
        error = kNoXlib;
        return nullptr;
    }
    HostThread* thread = HostThread::current();
    if (!thread) {
        error = "editor windows are made on the host thread";
        return nullptr;
    }
    Display* display = x->XOpenDisplay(nullptr);
    if (!display) {
        const char* name = std::getenv("DISPLAY");
        error = std::string("cannot open the X11 display") + (name && *name ? std::string(" ") + name : " (DISPLAY is not set)");
        return nullptr;
    }
    auto* c = new Connection;
    c->x = x;
    c->display = display;
    c->thread = thread;
    c->wmProtocols = x->XInternAtom(display, "WM_PROTOCOLS", False);
    c->wmDelete = x->XInternAtom(display, "WM_DELETE_WINDOW", False);
    c->netWmName = x->XInternAtom(display, "_NET_WM_NAME", False);
    c->utf8 = x->XInternAtom(display, "UTF8_STRING", False);
    c->xembed = x->XInternAtom(display, "_XEMBED", False);
    c->xembedInfo = x->XInternAtom(display, "_XEMBED_INFO", False);
    c->resourceManager = x->XInternAtom(display, "RESOURCE_MANAGER", False);
    x->XSelectInput(display, DefaultRootWindow(display), PropertyChangeMask);  // for RESOURCE_MANAGER
    c->scale = readScale(*x, display, c->resourceManager);
    c->watch = thread->watchFd(ConnectionNumber(display), HostThread::FdRead, [c](uint32_t) { c->dispatch(); });
    t_connection = c;
    return c;
}

void closeIfUnused() {
    Connection* c = t_connection;
    if (!c || !c->windows.empty()) return;
    c->thread->unwatchFd(c->watch);
    c->x->XCloseDisplay(c->display);
    delete c;
    t_connection = nullptr;
}

class X11HostWindow final : public HostWindow {
public:
    X11HostWindow(Connection& c, Callbacks cb, Placement placement)
        : c_(c), cb_(std::move(cb)), placement_(std::move(placement)) {}

    ~X11HostWindow() override {
        for (Window child : children_) c_.children.erase(child);
        if (window_) {
            c_.windows.erase(window_);
            c_.x->XDestroyWindow(c_.display, window_);
            c_.x->XFlush(c_.display);
        }
        closeIfUnused();
    }

    bool open(const std::string& title, bool resizable, std::string& error) {
        resizable_ = resizable;
        const Xlib& x = *c_.x;
        Display* d = c_.display;
        const Window parent = placement_.parent ? (Window)(uintptr_t)placement_.parent : DefaultRootWindow(d);
        XSetWindowAttributes attrs{};
        attrs.background_pixel = BlackPixel(d, DefaultScreen(d));
        // Our own changes, the plugin's windows coming and going in it, and the keyboard focus
        // coming to it or into it (for XEmbed).
        attrs.event_mask = StructureNotifyMask | SubstructureNotifyMask | FocusChangeMask;
        window_ = x.XCreateWindow(d, parent, 0, 0, width_, height_, 0, CopyFromParent, InputOutput, CopyFromParent,
                                  CWBackPixel | CWEventMask, &attrs);
        if (!window_) {
            error = "could not create editor window";
            return false;
        }
        c_.windows[window_] = this;
        if (!child()) {
            x.XSetWMProtocols(d, window_, &c_.wmDelete, 1);
            setTitle(title);
            // Which program it is, for the desktop (and tools that find windows by process).
            const long pid = (long)getpid();
            x.XChangeProperty(d, window_, x.XInternAtom(d, "_NET_WM_PID", False), XA_CARDINAL, 32, PropModeReplace,
                              reinterpret_cast<const unsigned char*>(&pid), 1);
            char name[] = "brack-plugin-editor", cls[] = "Brack";
            XClassHint hint{name, cls};
            x.XSetClassHint(d, window_, &hint);
        }
        x.XFlush(d);
        return true;
    }

    void* nativeHandle() const override { return (void*)(uintptr_t)window_; }
    double scale() const override { return c_.scale; }

    void setClientSize(uint32_t w, uint32_t h) override {
        if (!window_ || w == 0 || h == 0) return;
        width_ = w;
        height_ = h;
        requested_ = {w, h};
        sizeHints();
        c_.x->XResizeWindow(c_.display, window_, w, h);
        c_.x->XFlush(c_.display);
        reportSize();
    }

    void setResizable(bool resizable) override {
        resizable_ = resizable;
        sizeHints();
        c_.x->XFlush(c_.display);
    }

    void show() override {
        if (!window_) return;
        if (child()) c_.x->XMapWindow(c_.display, window_);
        else c_.x->XMapRaised(c_.display, window_);
        c_.x->XFlush(c_.display);
    }
    void hide() override {
        if (!window_) return;
        c_.x->XUnmapWindow(c_.display, window_);
        c_.x->XFlush(c_.display);
    }
    void abandon() override {
        cb_ = {};
        hide();
    }
    void setTitle(const std::string& title) override {
        if (!window_ || child()) return;
        c_.x->XStoreName(c_.display, window_, title.c_str());
        c_.x->XChangeProperty(c_.display, window_, c_.netWmName, c_.utf8, 8, PropModeReplace,
                              reinterpret_cast<const unsigned char*>(title.data()), (int)title.size());
        c_.x->XFlush(c_.display);
    }

    void scaleChanged(double scale) {
        if (cb_.scaleChanged) cb_.scaleChanged(scale);
    }

    void handle(const XEvent& e) {
        switch (e.type) {
            case ClientMessage:
                if (e.xclient.message_type == c_.wmProtocols && (Atom)e.xclient.data.l[0] == c_.wmDelete &&
                    cb_.closeRequested)
                    cb_.closeRequested();
                else if (e.xclient.message_type == c_.xembed && e.xclient.data.l[1] == kXembedRequestFocus)
                    focusClient();
                break;
            case ConfigureNotify:
                if (e.xconfigure.window == window_)
                    onConfigure((uint32_t)e.xconfigure.width, (uint32_t)e.xconfigure.height);
                break;
            case CreateNotify:
                if (e.xcreatewindow.parent == window_) adopt(e.xcreatewindow.window);
                break;
            case ReparentNotify:
                if (e.xreparent.window == window_) break;  // ours, into a window manager's frame
                if (e.xreparent.parent == window_) adopt(e.xreparent.window);
                else forget(e.xreparent.window);
                break;
            case PropertyNotify:  // on a child of ours
                if (e.xproperty.atom == c_.xembedInfo) readXembedInfo(e.xproperty.window);
                break;
            case FocusIn:
            case FocusOut: onFocus(e.xfocus); break;
            case DestroyNotify:
                if (e.xdestroywindow.window != window_) {
                    forget(e.xdestroywindow.window);
                    break;
                }
                // Destroyed along with the application's parent window, not by us: the editor has
                // to go too.
                c_.windows.erase(window_);
                window_ = 0;
                if (cb_.closeRequested) cb_.closeRequested();
                break;
            default: break;
        }
    }

private:
    bool child() const { return placement_.parent != nullptr; }

    // A window the user may not resize says so with its minimum and maximum sizes.
    void sizeHints() {
        if (child()) return;
        XSizeHints* hints = c_.x->XAllocSizeHints();
        if (!hints) return;
        if (!resizable_) {
            hints->flags = PMinSize | PMaxSize;
            hints->min_width = hints->max_width = (int)width_;
            hints->min_height = hints->max_height = (int)height_;
        }
        c_.x->XSetWMNormalHints(c_.display, window_, hints);
        c_.x->XFree(hints);
    }

    void reportSize() {
        if (placement_.sized) placement_.sized(width_, height_);
    }

    // Ours (the size asked for last) or the user's (a window manager's resize).
    void onConfigure(uint32_t w, uint32_t h) {
        if (w == width_ && h == height_) return;
        if (w == requested_.first && h == requested_.second) {
            width_ = w;
            height_ = h;
            reportSize();
            return;
        }
        width_ = w;
        height_ = h;
        if (!resizable_ || !cb_.resized) {
            reportSize();
            return;
        }
        uint32_t aw = w, ah = h;
        cb_.resized(aw, ah);
        if (window_ && (aw != w || ah != h)) setClientSize(aw, ah);
        else reportSize();
    }

    // ---- XEmbed

    // A window of the plugin's in ours: watched for _XEMBED_INFO, which it may set now or later.
    void adopt(Window w) {
        if (!children_.insert(w).second) return;
        c_.children[w] = this;
        bool gone = false;
        {
            ErrorTrap trap(*c_.x, c_.display);
            c_.x->XSelectInput(c_.display, w, PropertyChangeMask);
            gone = trap.failed();
        }
        if (gone) forget(w);
        else readXembedInfo(w);
    }

    void forget(Window w) {
        if (!children_.erase(w)) return;
        c_.children.erase(w);
        if (w == client_) client_ = 0;
    }

    // The first child that sets _XEMBED_INFO is embedded; its flags say whether it is to be mapped.
    void readXembedInfo(Window w) {
        if (client_ && w != client_) return;
        Atom type = 0;
        int format = 0;
        unsigned long count = 0, after = 0;
        unsigned char* data = nullptr;
        unsigned long flags = 0;
        bool found = false;
        {
            ErrorTrap trap(*c_.x, c_.display);
            if (c_.x->XGetWindowProperty(c_.display, w, c_.xembedInfo, 0, 2, False, c_.xembedInfo, &type, &format,
                                         &count, &after, &data) == Success &&
                data && format == 32 && count >= 2) {
                flags = reinterpret_cast<unsigned long*>(data)[1];
                found = true;
            }
            if (data) c_.x->XFree(data);
        }
        if (!found) return;
        if (!client_) {
            client_ = w;
            clientMapped_ = false;
            send(kXembedEmbeddedNotify, 0, (long)window_, 0);
            if (active_) {
                send(kXembedWindowActivate);
                focusClient();
            }
        }
        const bool mapped = flags & kXembedMapped;
        if (mapped == clientMapped_) return;
        clientMapped_ = mapped;
        ErrorTrap trap(*c_.x, c_.display);
        if (mapped) c_.x->XMapWindow(c_.display, client_);
        else c_.x->XUnmapWindow(c_.display, client_);
    }

    // The keyboard focus coming to our window or into it from elsewhere makes it active, and going
    // elsewhere inactive; moves within it change nothing. Focus on our window itself goes on to the
    // client.
    void onFocus(const XFocusChangeEvent& e) {
        if (e.window != window_ || e.mode == NotifyGrab || e.mode == NotifyUngrab) return;
        if (e.detail == NotifyPointer || e.detail == NotifyPointerRoot || e.detail == NotifyDetailNone) return;
        const bool in = e.type == FocusIn;
        if (e.detail != NotifyInferior && in != active_) {
            active_ = in;
            if (client_ && in) send(kXembedWindowActivate);
            if (client_ && !in) {
                send(kXembedFocusOut);
                send(kXembedWindowDeactivate);
            }
        }
        if (in && e.detail != NotifyVirtual && e.detail != NotifyNonlinearVirtual) focusClient();
    }

    // Gives the client the keyboard focus, unless it has it already, and tells it.
    void focusClient() {
        if (!client_) return;
        Window focus = 0;
        int revert = 0;
        c_.x->XGetInputFocus(c_.display, &focus, &revert);
        if (focus != client_) {
            ErrorTrap trap(*c_.x, c_.display);  // BadMatch while it is not viewable
            c_.x->XSetInputFocus(c_.display, client_, RevertToParent, CurrentTime);
        }
        send(kXembedFocusIn, kXembedFocusCurrent);
    }

    void send(long message, long detail = 0, long data1 = 0, long data2 = 0) {
        XEvent e{};
        e.xclient.type = ClientMessage;
        e.xclient.window = client_;
        e.xclient.message_type = c_.xembed;
        e.xclient.format = 32;
        e.xclient.data.l[0] = CurrentTime;
        e.xclient.data.l[1] = message;
        e.xclient.data.l[2] = detail;
        e.xclient.data.l[3] = data1;
        e.xclient.data.l[4] = data2;
        ErrorTrap trap(*c_.x, c_.display);
        c_.x->XSendEvent(c_.display, client_, False, NoEventMask, &e);
    }

    Connection& c_;
    Callbacks cb_;
    Placement placement_;
    Window window_ = 0;
    uint32_t width_ = 400, height_ = 300;
    std::pair<uint32_t, uint32_t> requested_{0, 0};
    bool resizable_ = false;
    std::set<Window> children_;
    Window client_ = 0;  // the XEmbed client, if any
    bool clientMapped_ = false;
    bool active_ = false;
};

void Connection::dispatch() {
    while (x->XPending(display)) {
        XEvent e;
        x->XNextEvent(display, &e);
        if (e.type == PropertyNotify && e.xproperty.window == DefaultRootWindow(display)) {
            if (e.xproperty.atom == resourceManager) resourcesChanged();
            continue;
        }
        if (auto it = windows.find(e.xany.window); it != windows.end()) it->second->handle(e);
        else if (auto child = children.find(e.xany.window); child != children.end()) child->second->handle(e);
    }
}

// Each window's editor hears of a new Xft.dpi; one may close another meanwhile.
void Connection::resourcesChanged() {
    const double now = readScale(*x, display, resourceManager);
    if (now == scale) return;
    scale = now;
    std::vector<Window> ids;
    for (auto& [id, w] : windows) ids.push_back(id);
    for (Window id : ids)
        if (auto it = windows.find(id); it != windows.end()) it->second->scaleChanged(now);
}

}  // namespace

HostWindow::Api HostWindow::api() { return Api::X11; }

bool HostWindow::available(std::string& error) {
    if (xlib()) return true;
    error = kNoXlib;
    return false;
}

std::unique_ptr<HostWindow> HostWindow::create(const std::string& title, bool resizable, Callbacks cb,
                                               const Placement& placement, std::string& error) {
    Connection* c = connection(error);
    if (!c) return nullptr;
    auto w = std::make_unique<X11HostWindow>(*c, std::move(cb), placement);
    if (!w->open(title, resizable, error)) return nullptr;
    return w;
}

ParentDpiScope::ParentDpiScope(void*) {}
ParentDpiScope::~ParentDpiScope() = default;

}  // namespace brack
