#include "util/dbus_linux.h"

#include <dlfcn.h>

#include <type_traits>

namespace brack::dbus {

const Functions* functions() {
    static const Functions* loaded = []() -> const Functions* {
        void* lib = dlopen("libdbus-1.so.3", RTLD_NOW | RTLD_LOCAL);  // kept to the end
        if (!lib) return nullptr;
        static Functions f;
        bool ok = true;
        auto get = [&](auto& fn, const char* name) {
            fn = reinterpret_cast<std::remove_reference_t<decltype(fn)>>(dlsym(lib, name));
            ok = ok && fn;
        };
        get(f.errorInit, "dbus_error_init");
        get(f.errorFree, "dbus_error_free");
        get(f.busGetPrivate, "dbus_bus_get_private");
        get(f.busGetUniqueName, "dbus_bus_get_unique_name");
        get(f.busAddMatch, "dbus_bus_add_match");
        get(f.setExitOnDisconnect, "dbus_connection_set_exit_on_disconnect");
        get(f.connectionClose, "dbus_connection_close");
        get(f.connectionUnref, "dbus_connection_unref");
        get(f.connectionReadWrite, "dbus_connection_read_write");
        get(f.connectionPopMessage, "dbus_connection_pop_message");
        get(f.newMethodCall, "dbus_message_new_method_call");
        get(f.appendArgs, "dbus_message_append_args");
        get(f.sendWithReplyAndBlock, "dbus_connection_send_with_reply_and_block");
        get(f.messageUnref, "dbus_message_unref");
        get(f.isSignal, "dbus_message_is_signal");
        get(f.getPath, "dbus_message_get_path");
        get(f.getSender, "dbus_message_get_sender");
        get(f.iterInit, "dbus_message_iter_init");
        get(f.iterInitAppend, "dbus_message_iter_init_append");
        get(f.iterAppendBasic, "dbus_message_iter_append_basic");
        get(f.iterOpenContainer, "dbus_message_iter_open_container");
        get(f.iterCloseContainer, "dbus_message_iter_close_container");
        get(f.iterArgType, "dbus_message_iter_get_arg_type");
        get(f.iterNext, "dbus_message_iter_next");
        get(f.iterRecurse, "dbus_message_iter_recurse");
        get(f.iterGetBasic, "dbus_message_iter_get_basic");
        return ok ? &f : nullptr;
    }();
    return loaded;
}

}  // namespace brack::dbus
