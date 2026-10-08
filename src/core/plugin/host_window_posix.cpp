// Linux without the X11 development files (host_window_x11.cpp): no editor windows.
#include "plugin/host_window.h"

namespace brack {

HostWindow::Api HostWindow::api() { return Api::X11; }

bool HostWindow::available(std::string& error) {
    error = "plugin editors are not available on this system yet";
    return false;
}

std::unique_ptr<HostWindow> HostWindow::create(const std::string&, bool, Callbacks, const Placement&, std::string& error) {
    available(error);
    return nullptr;
}

ParentDpiScope::ParentDpiScope(void*) {}
ParentDpiScope::~ParentDpiScope() = default;

}  // namespace brack
