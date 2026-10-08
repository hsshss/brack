#include "clap/clap_instance.h"
#include "plugin/host_window.h"
#include "util/common.h"

namespace brack {

namespace {

const char* clapWindowApi() {
    switch (HostWindow::api()) {
        case HostWindow::Api::Win32: return CLAP_WINDOW_API_WIN32;
        case HostWindow::Api::Cocoa: return CLAP_WINDOW_API_COCOA;
        case HostWindow::Api::X11: return CLAP_WINDOW_API_X11;
    }
    return CLAP_WINDOW_API_WIN32;
}

clap_window_t clapWindow(void* handle) {
    clap_window_t w{};
    w.api = clapWindowApi();
    switch (HostWindow::api()) {
        case HostWindow::Api::Win32: w.win32 = handle; break;
        case HostWindow::Api::Cocoa: w.cocoa = handle; break;
        case HostWindow::Api::X11: w.x11 = (clap_xwnd)(uintptr_t)handle; break;
    }
    return w;
}

// Embedded in a HostWindow, or the plugin's own floating window when it cannot be embedded.
class HostedClapEditor final : public ClapEditor {
public:
    HostedClapEditor(ClapInstance& owner, const clap_plugin_t* plugin, const clap_plugin_gui_t* gui)
        : owner_(owner), plugin_(plugin), gui_(gui) {}

    ~HostedClapEditor() override {
        if (guiCreated_) {
            gui_->hide(plugin_);
            gui_->destroy(plugin_);
        }
        window_.reset();
    }

    bool open(const std::string& title, std::string& error) {
        // Before create(), which fails without saying why when libX11 is missing.
        if (!HostWindow::available(error)) return false;
        floating_ = !gui_->is_api_supported(plugin_, clapWindowApi(), false);
        const HostWindow::Placement placement = owner_.editorPlacement();
        if (floating_ && placement.parent) {
            error = "this plugin's editor only opens in a window of its own";
            return false;
        }
        if (!gui_->create(plugin_, clapWindowApi(), floating_)) {
            error = "plugin failed to create its GUI";
            return false;
        }
        guiCreated_ = true;

        if (floating_) {
            gui_->suggest_title(plugin_, title.c_str());
            gui_->show(plugin_);
            return true;
        }

        HostWindow::Callbacks cb;
        cb.resized = [this](uint32_t& w, uint32_t& h) {
            owner_.callPlugin("editor resize", [&] {
                uint32_t aw = w, ah = h;
                if (gui_->adjust_size(plugin_, &aw, &ah)) {
                    w = aw;
                    h = ah;
                }
                gui_->set_size(plugin_, w, h);
            });
        };
        cb.scaleChanged = [this](double scale) {
            owner_.callPlugin("editor scale", [&] { gui_->set_scale(plugin_, scale); });
        };
        cb.closeRequested = [this] { owner_.editorClosedByUser(); };
        window_ = HostWindow::create(title, gui_->can_resize(plugin_), std::move(cb), placement, error);
        if (!window_) return false;

        // Plugins may ignore this on Windows and read the DPI themselves. With Cocoa the OS scales.
        if (HostWindow::api() != HostWindow::Api::Cocoa) gui_->set_scale(plugin_, window_->scale());
        uint32_t w = 0, h = 0;
        if (!gui_->get_size(plugin_, &w, &h) || w == 0 || h == 0) {
            w = 800;
            h = 600;
        }
        window_->setClientSize(w, h);

        const clap_window_t win = clapWindow(window_->nativeHandle());
        if (!gui_->set_parent(plugin_, &win)) {
            error = "plugin refused the editor window";
            return false;
        }
        window_->show();
        gui_->show(plugin_);
        // Editors that know their size only once they have a parent window (JUCE's) ask for it from
        // set_parent or show, before there is an editor to ask (ClapInstance::onGuiRequestResize).
        // Some know only then whether they can be resized (LSP's).
        if (uint32_t aw = 0, ah = 0; gui_->get_size(plugin_, &aw, &ah) && aw && ah && (aw != w || ah != h))
            window_->setClientSize(aw, ah);
        window_->setResizable(gui_->can_resize(plugin_));
        return true;
    }

    bool requestResize(uint32_t w, uint32_t h) override {
        if (!window_) return false;
        window_->setClientSize(w, h);
        return true;
    }
    void setTitle(const std::string& title) override {
        if (window_) window_->setTitle(title);
        else if (floating_) gui_->suggest_title(plugin_, title.c_str());
    }
    void show() override {
        if (window_) window_->show();
        gui_->show(plugin_);
    }
    void hide() override {
        gui_->hide(plugin_);
        if (window_) window_->hide();
    }
    void abandon() override {
        // A floating window is the plugin's own: nothing to hide without calling it.
        if (window_) window_->abandon();
        (void)window_.release();
        guiCreated_ = false;
    }

private:
    ClapInstance& owner_;
    const clap_plugin_t* plugin_;
    const clap_plugin_gui_t* gui_;
    std::unique_ptr<HostWindow> window_;
    bool guiCreated_ = false;
    bool floating_ = false;
};

}  // namespace

bool clapEditorSupported(const clap_plugin_t* plugin, const clap_plugin_gui_t* gui) {
    return gui->is_api_supported(plugin, clapWindowApi(), false) || gui->is_api_supported(plugin, clapWindowApi(), true);
}

std::unique_ptr<ClapEditor> createClapEditor(ClapInstance& owner, const clap_plugin_t* plugin,
                                             const clap_plugin_gui_t* gui, const std::string& title,
                                             std::string& error) {
    auto e = std::make_unique<HostedClapEditor>(owner, plugin, gui);
    if (!e->open(title, error)) return nullptr;
    return e;
}

}  // namespace brack
