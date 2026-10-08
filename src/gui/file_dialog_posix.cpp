// Linux: the desktop portal's file dialog (org.freedesktop.portal.FileChooser, over D-Bus), which
// attaches to Brack's window and works inside a sandbox (Flatpak, Snap); without a portal, zenity
// (GNOME) or else kdialog (KDE) as a child process. Each dialog starts in the folder of the last
// file chosen with the same filter, else the home folder.
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#define GLFW_EXPOSE_NATIVE_X11
#include <GLFW/glfw3.h>
#include <GLFW/glfw3native.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "file_dialog.h"
#include "util/common.h"
#include "util/dbus_linux.h"

extern char** environ;

namespace brack {

namespace {

unsigned long g_owner = 0;                   // Brack's X11 window
std::map<std::string, std::string> g_folders;  // pattern -> the folder of the last file chosen

// "*.clap;*.vst3" -> {"*.clap", "*.vst3"}.
std::vector<std::string> globs(const char* pattern) {
    std::vector<std::string> out;
    std::string s = pattern;
    for (size_t at = 0; at <= s.size();) {
        size_t end = s.find(';', at);
        if (end == std::string::npos) end = s.size();
        if (end > at) out.push_back(s.substr(at, end - at));
        at = end + 1;
    }
    return out;
}

std::string startFolder(const char* pattern) {
    if (auto it = g_folders.find(pattern); it != g_folders.end()) return it->second;
    const char* home = std::getenv("HOME");
    return home && *home ? home : "/";
}

// ---- the portal

// "file:///home/a%20b/x.json" -> "/home/a b/x.json"; "" for anything but a local file.
std::string pathOfUri(const std::string& uri) {
    const std::string scheme = "file://";
    if (uri.rfind(scheme, 0) != 0) return {};
    std::string path;
    for (size_t i = scheme.size(); i < uri.size(); ++i) {
        if (uri[i] == '%' && i + 2 < uri.size()) {
            path += (char)std::strtol(uri.substr(i + 1, 2).c_str(), nullptr, 16);
            i += 2;
        } else {
            path += uri[i];
        }
    }
    return path;
}

class PortalCall {
public:
    explicit PortalCall(const dbus::Functions& d) : d_(d) {}

    void string(dbus::Iter& it, const std::string& s) {
        const char* p = s.c_str();
        d_.iterAppendBasic(&it, dbus::kTypeString, &p);
    }
    // An entry of the a{sv} of options, its value of type `signature`, which `fill` appends.
    template <typename F>
    void option(dbus::Iter& options, const char* key, const char* signature, F fill) {
        dbus::Iter entry{}, value{};
        d_.iterOpenContainer(&options, dbus::kTypeDictEntry, nullptr, &entry);
        string(entry, key);
        d_.iterOpenContainer(&entry, dbus::kTypeVariant, signature, &value);
        fill(value);
        d_.iterCloseContainer(&entry, &value);
        d_.iterCloseContainer(&options, &entry);
    }
    // A filter, (sa(us)): its name and its glob patterns (0).
    void filter(dbus::Iter& into, const std::string& name, const std::vector<std::string>& patterns) {
        dbus::Iter entry{}, list{};
        d_.iterOpenContainer(&into, dbus::kTypeStruct, nullptr, &entry);
        string(entry, name);
        d_.iterOpenContainer(&entry, dbus::kTypeArray, "(us)", &list);
        for (const std::string& p : patterns) {
            dbus::Iter item{};
            const uint32_t glob = 0;
            d_.iterOpenContainer(&list, dbus::kTypeStruct, nullptr, &item);
            d_.iterAppendBasic(&item, dbus::kTypeUint32, &glob);
            string(item, p);
            d_.iterCloseContainer(&list, &item);
        }
        d_.iterCloseContainer(&entry, &list);
        d_.iterCloseContainer(&into, &entry);
    }

    // The first of the response's "uris", if it is a local file.
    std::string chosenPath(void* response) {
        dbus::Iter it{}, results{};
        uint32_t code = 2;
        if (!d_.iterInit(response, &it) || d_.iterArgType(&it) != dbus::kTypeUint32) return {};
        d_.iterGetBasic(&it, &code);
        if (code != 0 || !d_.iterNext(&it) || d_.iterArgType(&it) != dbus::kTypeArray) return {};  // 1: cancelled
        for (d_.iterRecurse(&it, &results); d_.iterArgType(&results) == dbus::kTypeDictEntry; d_.iterNext(&results)) {
            dbus::Iter entry{}, value{}, uris{};
            const char* key = nullptr;
            d_.iterRecurse(&results, &entry);
            d_.iterGetBasic(&entry, &key);
            if (!key || std::strcmp(key, "uris") != 0 || !d_.iterNext(&entry)) continue;
            d_.iterRecurse(&entry, &value);
            if (d_.iterArgType(&value) != dbus::kTypeArray) return {};
            d_.iterRecurse(&value, &uris);
            if (d_.iterArgType(&uris) != dbus::kTypeString) return {};
            const char* uri = nullptr;
            d_.iterGetBasic(&uris, &uri);
            return uri ? pathOfUri(uri) : std::string();
        }
        return {};
    }

private:
    const dbus::Functions& d_;
};

// The chosen path, "" when cancelled; nothing when there is no portal to ask.
std::optional<std::string> fromPortal(bool save, const char* name, const char* pattern) {
    const dbus::Functions* d = dbus::functions();
    if (!d) return std::nullopt;
    dbus::Error error;
    d->errorInit(&error);
    void* bus = d->busGetPrivate(dbus::kBusSession, &error);
    if (!bus) {
        d->errorFree(&error);
        return std::nullopt;
    }
    d->setExitOnDisconnect(bus, 0);  // libdbus would end the process
    struct Close {
        const dbus::Functions* d;
        void* bus;
        ~Close() {
            d->connectionClose(bus);
            d->connectionUnref(bus);
        }
    } close{d, bus};

    // The answer comes as a signal on a request object, whose path the call's token sets: listened
    // for before the call, so that it cannot come first, and from the portal only.
    static int s_calls = 0;
    const std::string token = "brack" + std::to_string(getpid()) + "_" + std::to_string(++s_calls);
    std::string sender = d->busGetUniqueName(bus) ? d->busGetUniqueName(bus) + 1 : "";  // past ':'
    for (char& c : sender)
        if (c == '.') c = '_';
    std::string request = "/org/freedesktop/portal/desktop/request/" + sender + "/" + token;
    auto listen = [&](const std::string& path) {
        const std::string rule = "type='signal',sender='org.freedesktop.portal.Desktop',"
                                 "interface='org.freedesktop.portal.Request',member='Response',path='" +
                                 path + "'";
        d->busAddMatch(bus, rule.c_str(), nullptr);
    };
    listen(request);

    void* call = d->newMethodCall("org.freedesktop.portal.Desktop", "/org/freedesktop/portal/desktop",
                                  "org.freedesktop.portal.FileChooser", save ? "SaveFile" : "OpenFile");
    if (!call) return std::nullopt;
    PortalCall p(*d);
    dbus::Iter args{}, options{};
    d->iterInitAppend(call, &args);
    char parent[32] = "";
    if (g_owner) std::snprintf(parent, sizeof parent, "x11:%lx", g_owner);
    p.string(args, parent);
    p.string(args, save ? "Save" : "Open");
    d->iterOpenContainer(&args, dbus::kTypeArray, "{sv}", &options);
    p.option(options, "handle_token", "s", [&](dbus::Iter& v) { p.string(v, token); });
    p.option(options, "modal", "b", [&](dbus::Iter& v) {
        const uint32_t yes = 1;
        d->iterAppendBasic(&v, dbus::kTypeBoolean, &yes);
    });
    p.option(options, "filters", "a(sa(us))", [&](dbus::Iter& v) {
        dbus::Iter list{};
        d->iterOpenContainer(&v, dbus::kTypeArray, "(sa(us))", &list);
        p.filter(list, name, globs(pattern));
        p.filter(list, "All files", {"*"});
        d->iterCloseContainer(&v, &list);
    });
    p.option(options, "current_folder", "ay", [&](dbus::Iter& v) {
        dbus::Iter bytes{};
        const std::string folder = startFolder(pattern);
        d->iterOpenContainer(&v, dbus::kTypeArray, "y", &bytes);
        for (size_t i = 0; i <= folder.size(); ++i) {  // with its terminating NUL
            const unsigned char c = (unsigned char)folder.c_str()[i];
            d->iterAppendBasic(&bytes, dbus::kTypeByte, &c);
        }
        d->iterCloseContainer(&v, &bytes);
    });
    d->iterCloseContainer(&args, &options);

    void* reply = d->sendWithReplyAndBlock(bus, call, -1, &error);
    d->messageUnref(call);
    if (!reply) {
        // No portal, or none with a file chooser, on this desktop.
        d->errorFree(&error);
        return std::nullopt;
    }
    dbus::Iter it{};
    const char* handle = nullptr;
    if (d->iterInit(reply, &it) && d->iterArgType(&it) == dbus::kTypeObjectPath) d->iterGetBasic(&it, &handle);
    if (handle && request != handle) {  // older portals choose the path themselves
        request = handle;
        listen(request);
    }
    const std::string portal = d->getSender(reply) ? d->getSender(reply) : "";  // its unique name
    d->messageUnref(reply);

    // Until the user is done with it.
    while (d->connectionReadWrite(bus, -1)) {
        while (void* m = d->connectionPopMessage(bus)) {
            const char *path = d->getPath(m), *sender = d->getSender(m);
            const bool ours = d->isSignal(m, "org.freedesktop.portal.Request", "Response") && path && request == path &&
                              sender && portal == sender;
            std::string chosen = ours ? p.chosenPath(m) : std::string();
            d->messageUnref(m);
            if (ours) return chosen;
        }
    }
    return std::string();  // the bus went away
}

// ---- zenity, kdialog

// What `args` (its program found in PATH) printed, empty when it was cancelled or failed; nothing
// when the program is not there.
std::optional<std::string> output(const std::vector<std::string>& args) {
    int out[2];
    if (pipe(out) != 0) return std::nullopt;
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, out[1], 1);
    posix_spawn_file_actions_addclose(&actions, out[0]);
    std::vector<char*> argv;
    for (const std::string& a : args) argv.push_back(const_cast<char*>(a.c_str()));
    argv.push_back(nullptr);
    pid_t pid = 0;
    const int spawned = posix_spawnp(&pid, argv[0], &actions, nullptr, argv.data(), environ);
    posix_spawn_file_actions_destroy(&actions);
    close(out[1]);
    if (spawned != 0) {
        close(out[0]);
        return std::nullopt;
    }
    std::string text;
    char buf[4096];
    for (ssize_t n; (n = read(out[0], buf, sizeof buf)) != 0;) {
        if (n > 0) text.append(buf, (size_t)n);
        else if (errno != EINTR) break;
    }
    close(out[0]);
    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) return std::string();
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) text.pop_back();
    return text;
}

// zenity cannot be attached to Brack's window (its --attach does nothing since zenity 4).
std::optional<std::string> fromProgram(bool save, const char* name, const char* pattern) {
    std::string spaced;  // "*.clap *.vst3", as both programs take them
    for (const std::string& g : globs(pattern)) spaced += (spaced.empty() ? "" : " ") + g;
    const std::string folder = startFolder(pattern);
    std::vector<std::string> zenity = {"zenity", "--file-selection", "--filename=" + folder + "/",
                                       std::string("--file-filter=") + name + " | " + spaced,
                                       "--file-filter=All files | *"};
    if (save) zenity.push_back("--save");  // zenity asks before overwriting
    if (std::optional<std::string> path = output(zenity)) return path;
    std::vector<std::string> kdialog = {"kdialog", save ? "--getsavefilename" : "--getopenfilename", folder,
                                        spaced + "|" + name};
    if (g_owner) {
        kdialog.push_back("--attach");
        kdialog.push_back(std::to_string(g_owner));
    }
    return output(kdialog);
}

std::string run(bool save, const char* name, const char* pattern, const char* ext) {
    std::optional<std::string> path = fromPortal(save, name, pattern);
    if (!path) path = fromProgram(save, name, pattern);
    if (!path) {
        logWarn("file dialogs need the desktop portal, zenity or kdialog (none was found)");
        return {};
    }
    if (path->empty()) return {};
    if (save && ext && pathFromUtf8(*path).extension().empty()) *path += std::string(".") + ext;
    g_folders[pattern] = pathToUtf8(pathFromUtf8(*path).parent_path());
    return *path;
}

}  // namespace

void setFileDialogOwner(GLFWwindow* owner) { g_owner = owner ? (unsigned long)glfwGetX11Window(owner) : 0; }
std::future<std::string> openFileDialog(std::string filterName, std::string pattern) {
    return std::async(std::launch::async, [=] { return run(false, filterName.c_str(), pattern.c_str(), nullptr); });
}
std::future<std::string> saveFileDialog(std::string filterName, std::string pattern, std::string defaultExt) {
    return std::async(std::launch::async,
                      [=] { return run(true, filterName.c_str(), pattern.c_str(), defaultExt.c_str()); });
}

}  // namespace brack
