#pragma once
// The native window a plugin editor is embedded into: a top-level window of its own, or a
// child of a window the application gives (Placement::parent). Shared by the formats; each
// one wires the callbacks to its own editor API. Host thread only.

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace brack {

class HostWindow {
public:
    // The window system editors are embedded with. Each format names it its own way: CLAP's
    // CLAP_WINDOW_API_*, VST3's kPlatformType*, and VST2 by what effEditOpen's pointer is (HWND,
    // NSView*, or an X11 Window id).
    enum class Api { Win32, Cocoa, X11 };
    static Api api();  // of this build
    // False, with the reason, where no editor can be shown at all (Linux without libX11).
    static bool available(std::string& error);

    struct Callbacks {
        // The user resized the window to this client size (not called for setClientSize()).
        // The callee may change w/h to a size the editor accepts; the window then snaps to it.
        std::function<void(uint32_t& w, uint32_t& h)> resized;
        std::function<void(double scale)> scaleChanged;  // DPI change; 1.0 = 96 dpi
        std::function<void()> closeRequested;            // the user clicked close
    };

    // Where the window goes, decided by whoever asks for the editor (see PluginInstance::openGui).
    struct Placement {
        // A window of the application's (HWND on Windows) to open in as a child, at its top left
        // and without a frame. Null: a top-level window.
        void* parent = nullptr;
        // The client size changed, whatever the cause (the plugin asked, or the user resized).
        std::function<void(uint32_t w, uint32_t h)> sized;
    };

    static std::unique_ptr<HostWindow> create(const std::string& title, bool resizable, Callbacks cb,
                                              const Placement& placement, std::string& error);
    virtual ~HostWindow() = default;

    // HWND (Win32), NSView* (Cocoa), or the X11 Window id cast to a pointer.
    virtual void* nativeHandle() const = 0;
    virtual double scale() const = 0;        // current DPI scale
    virtual void setClientSize(uint32_t w, uint32_t h) = 0;
    virtual void setResizable(bool resizable) = 0;
    virtual void show() = 0;
    virtual void hide() = 0;
    virtual void setTitle(const std::string& title) = 0;
    // Hides the window and drops the callbacks, leaving it (and whatever the plugin put in it)
    // otherwise untouched. For a crashed plugin, whose child windows must not be destroyed.
    virtual void abandon() = 0;
};

// While it exists, windows the host thread creates take the DPI awareness of `parent` (if any),
// as Windows requires of child windows. Around the creation of an editor in a parent window,
// so the plugin's own windows in it match too.
class ParentDpiScope {
public:
    explicit ParentDpiScope(void* parent);
    ~ParentDpiScope();
    ParentDpiScope(const ParentDpiScope&) = delete;
    ParentDpiScope& operator=(const ParentDpiScope&) = delete;

private:
    [[maybe_unused]] void* previous_ = nullptr;
};

}  // namespace brack
