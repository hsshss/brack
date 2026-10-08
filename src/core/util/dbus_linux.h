#pragma once
// Linux: libdbus-1, loaded when first needed (dlopen), for the few calls Brack makes over D-Bus:
// RealtimeKit (util/realtime_linux.cpp) and the desktop portal's file chooser (the GUI). Not
// linked, so that Brack runs where it is not installed.

#include <cstdint>

namespace brack::dbus {

struct Error {  // DBusError (dbus-errors.h)
    const char* name;
    const char* message;
    unsigned int dummy : 5;
    void* padding;
};
struct alignas(16) Iter {  // DBusMessageIter: opaque, and smaller than this on every ABI
    unsigned char bytes[256];
};

constexpr int kBusSession = 0, kBusSystem = 1;  // DBusBusType
// Type codes (dbus-protocol.h).
constexpr int kTypeInvalid = 0, kTypeByte = 'y', kTypeBoolean = 'b', kTypeString = 's', kTypeObjectPath = 'o',
              kTypeUint32 = 'u', kTypeUint64 = 't', kTypeInt32 = 'i', kTypeInt64 = 'x', kTypeVariant = 'v',
              kTypeArray = 'a', kTypeStruct = 'r', kTypeDictEntry = 'e';

// Connections and messages are the library's pointers; dbus_bool_t is a uint32_t.
struct Functions {
    void (*errorInit)(Error*);
    void (*errorFree)(Error*);
    void* (*busGetPrivate)(int, Error*);
    const char* (*busGetUniqueName)(void*);
    void (*busAddMatch)(void*, const char*, Error*);
    void (*setExitOnDisconnect)(void*, uint32_t);
    void (*connectionClose)(void*);
    void (*connectionUnref)(void*);
    uint32_t (*connectionReadWrite)(void*, int);
    void* (*connectionPopMessage)(void*);
    void* (*newMethodCall)(const char*, const char*, const char*, const char*);
    uint32_t (*appendArgs)(void*, int, ...);
    void* (*sendWithReplyAndBlock)(void*, void*, int, Error*);
    void (*messageUnref)(void*);
    uint32_t (*isSignal)(void*, const char*, const char*);
    const char* (*getPath)(void*);
    const char* (*getSender)(void*);
    uint32_t (*iterInit)(void*, Iter*);
    void (*iterInitAppend)(void*, Iter*);
    uint32_t (*iterAppendBasic)(Iter*, int, const void*);
    uint32_t (*iterOpenContainer)(Iter*, int, const char*, Iter*);
    uint32_t (*iterCloseContainer)(Iter*, Iter*);
    int (*iterArgType)(Iter*);
    uint32_t (*iterNext)(Iter*);
    void (*iterRecurse)(Iter*, Iter*);
    void (*iterGetBasic)(Iter*, void*);
};

// The library's functions; null without libdbus-1.so.3.
const Functions* functions();

}  // namespace brack::dbus
