// Linux: threads the plugin starts get a stack of their own for the crash handler too
// (BrackLink::protectThisThread()), so that a stack overflow on one is recorded like any other
// crash, not only the signal that ends the process. This program's pthread_create stands in for
// the C library's: the program comes first in the plugins' symbol lookup (exported, CMakeLists.txt).
#include <dlfcn.h>
#include <pthread.h>

#include <cerrno>
#include <new>

#include "remote/brack_link.h"

namespace {

struct Start {
    void* (*fn)(void*);
    void* arg;
};

void* run(void* p) {
    const Start start = *static_cast<Start*>(p);
    delete static_cast<Start*>(p);
    brack::BrackLink::protectThisThread();
    return start.fn(start.arg);
}

}  // namespace

extern "C" int pthread_create(pthread_t* thread, const pthread_attr_t* attr, void* (*fn)(void*), void* arg) {
    using Create = int (*)(pthread_t*, const pthread_attr_t*, void* (*)(void*), void*);
    static const auto real = reinterpret_cast<Create>(dlsym(RTLD_NEXT, "pthread_create"));
    if (!real) return EAGAIN;
    auto* start = new (std::nothrow) Start{fn, arg};
    if (!start) return EAGAIN;
    const int result = real(thread, attr, &run, start);
    if (result != 0) delete start;
    return result;
}
