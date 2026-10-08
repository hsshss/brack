#include "vst3/vst3_plugin.h"

#include <pluginterfaces/base/funknownimpl.h>
#include <pluginterfaces/base/ibstream.h>
#include <pluginterfaces/base/ipluginbase.h>
#include <pluginterfaces/base/smartpointer.h>
#include <pluginterfaces/gui/iplugview.h>
#include <pluginterfaces/gui/iplugviewcontentscalesupport.h>
#include <pluginterfaces/vst/ivstattributes.h>
#include <pluginterfaces/vst/ivstaudioprocessor.h>
#include <pluginterfaces/vst/ivstcomponent.h>
#include <pluginterfaces/vst/ivsteditcontroller.h>
#include <pluginterfaces/vst/ivstevents.h>
#include <pluginterfaces/vst/ivsthostapplication.h>
#include <pluginterfaces/vst/ivstmessage.h>
#include <pluginterfaces/vst/ivstmidicontrollers.h>
#include <pluginterfaces/vst/ivstparameterchanges.h>
#include <pluginterfaces/vst/ivstpluginterfacesupport.h>
#include <pluginterfaces/vst/ivstprocesscontext.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <variant>

#ifdef __APPLE__
#include <CoreFoundation/CoreFoundation.h>
#endif

#include "host_thread.h"
#include "plugin/host_window.h"
#include "plugin/library.h"
#include "plugin/plugin_files.h"
#include "util/common.h"

namespace brack {

namespace {

using namespace Steinberg;
using namespace Steinberg::Vst;

const char kStateMagic[4] = {'B', 'R', 'V', '3'};

// ---------------------------------------------------------------------------
// helpers

// What VST3 calls the window system editors are embedded with (HostWindow::api()).
FIDString vst3PlatformType() {
    switch (HostWindow::api()) {
        case HostWindow::Api::Win32: return kPlatformTypeHWND;
        case HostWindow::Api::Cocoa: return kPlatformTypeNSView;
        case HostWindow::Api::X11: return kPlatformTypeX11EmbedWindowID;
    }
    return kPlatformTypeHWND;
}

std::string fromUtf16(const char16* s) {
    if (!s) return {};
    return utf16ToUtf8(std::u16string_view(reinterpret_cast<const char16_t*>(s)));
}

void toUtf16(const std::string& utf8, String128 out) {
    const std::u16string w = utf8ToUtf16(utf8);
    size_t n = std::min<size_t>(w.size(), 127);
    if (n < w.size() && n > 0 && w[n - 1] >= 0xD800 && w[n - 1] < 0xDC00) --n;  // not half a pair
    std::memcpy(out, w.data(), n * sizeof(char16));
    out[n] = 0;
}

// Class ids as text the way Steinberg writes them (moduleinfo.json, FUID::toString): the four
// 32-bit words in hex, which reads the same on every platform whatever the byte layout.
std::string uidToString(const TUID uid) {
    FUID f = FUID::fromTUID(uid);
    char buf[40];
    std::snprintf(buf, sizeof buf, "%08X%08X%08X%08X", f.getLong1(), f.getLong2(), f.getLong3(), f.getLong4());
    return buf;
}

bool uidFromString(const std::string& s, TUID uid) {
    if (s.size() != 32 || s.find_first_not_of("0123456789abcdefABCDEF") != std::string::npos) return false;
    uint32 l[4];
    for (int i = 0; i < 4; ++i) l[i] = (uint32)std::stoul(s.substr((size_t)i * 8, 8), nullptr, 16);
    FUID(l[0], l[1], l[2], l[3]).toTUID(uid);
    return true;
}

// Fixed-size queue of parameter changes. Producers serialize on a mutex; the consumer
// never blocks (the audio thread is on one end or the other).
class ParamRing {
public:
    struct Change {
        ParamID id;
        ParamValue value;
    };
    bool push(ParamID id, ParamValue value) {
        std::lock_guard lock(producerMutex_);
        const size_t w = write_.load(std::memory_order_relaxed);
        if (w - read_.load(std::memory_order_acquire) >= buf_.size()) return false;
        buf_[w % buf_.size()] = {id, value};
        write_.store(w + 1, std::memory_order_release);
        return true;
    }
    bool pop(Change& out) {
        const size_t r = read_.load(std::memory_order_relaxed);
        if (r == write_.load(std::memory_order_acquire)) return false;
        out = buf_[r % buf_.size()];
        read_.store(r + 1, std::memory_order_release);
        return true;
    }

private:
    std::array<Change, 4096> buf_{};
    std::atomic<size_t> write_{0}, read_{0};
    std::mutex producerMutex_;
};

// ---------------------------------------------------------------------------
// host-side objects

class HostAttributeList final : public U::Implements<U::Directly<IAttributeList>> {
public:
    tresult PLUGIN_API setInt(AttrID id, int64 value) override { return set(id, value); }
    tresult PLUGIN_API getInt(AttrID id, int64& value) override { return get(id, value); }
    tresult PLUGIN_API setFloat(AttrID id, double value) override { return set(id, value); }
    tresult PLUGIN_API getFloat(AttrID id, double& value) override { return get(id, value); }
    tresult PLUGIN_API setString(AttrID id, const TChar* string) override {
        if (!id || !string) return kInvalidArgument;
        return set(id, std::u16string(string));
    }
    tresult PLUGIN_API getString(AttrID id, TChar* string, uint32 sizeInBytes) override {
        std::u16string s;
        if (!string || get(id, s) != kResultOk) return kResultFalse;
        const size_t n = std::min<size_t>(s.size(), sizeInBytes / sizeof(TChar) - 1);
        std::memcpy(string, s.data(), n * sizeof(TChar));
        string[n] = 0;
        return kResultOk;
    }
    tresult PLUGIN_API setBinary(AttrID id, const void* data, uint32 size) override {
        if (!id || (!data && size)) return kInvalidArgument;
        auto* p = static_cast<const uint8_t*>(data);
        return set(id, std::vector<uint8_t>(p, p + size));
    }
    tresult PLUGIN_API getBinary(AttrID id, const void*& data, uint32& size) override {
        if (!id) return kInvalidArgument;
        auto it = values_.find(id);
        if (it == values_.end() || !std::holds_alternative<std::vector<uint8_t>>(it->second)) return kResultFalse;
        auto& v = std::get<std::vector<uint8_t>>(it->second);
        data = v.data();
        size = (uint32)v.size();
        return kResultOk;
    }

private:
    using Value = std::variant<int64, double, std::u16string, std::vector<uint8_t>>;
    template <typename T>
    tresult set(AttrID id, T value) {
        if (!id) return kInvalidArgument;
        values_[id] = std::move(value);
        return kResultOk;
    }
    template <typename T>
    tresult get(AttrID id, T& out) {
        if (!id) return kInvalidArgument;
        auto it = values_.find(id);
        if (it == values_.end() || !std::holds_alternative<T>(it->second)) return kResultFalse;
        out = std::get<T>(it->second);
        return kResultOk;
    }
    std::map<std::string, Value> values_;
};

class HostMessage final : public U::Implements<U::Directly<IMessage>> {
public:
    FIDString PLUGIN_API getMessageID() override { return id_.c_str(); }
    void PLUGIN_API setMessageID(FIDString id) override { id_ = id ? id : ""; }
    IAttributeList* PLUGIN_API getAttributes() override { return attributes_; }

private:
    std::string id_;
    IPtr<HostAttributeList> attributes_ = owned(new HostAttributeList);
};

#if SMTG_OS_LINUX
// Linux: the host thread's loop, for a plugin's own file descriptors and timers (its X11
// connection, say). Each editor gets one through its IPlugFrame, and each instance one through its
// host context, for a plugin that wants it without an editor. What is left registered goes with the
// editor or the instance (clear()), after which the plugin may still hold the run loop, but not use it.
class RunLoop final : public U::Implements<U::Directly<Linux::IRunLoop>> {
public:
    explicit RunLoop(PluginInstance& owner) : owner_(&owner) {}

    // `release`: false for a crashed plugin, whose code must not run again.
    void clear(bool release) {
        if (!owner_) return;
        for (auto& [handler, id] : fds_) {
            owner_->hostThread().unwatchFd(id);
            if (release) handler->release();
        }
        for (auto& [handler, id] : timers_) {
            owner_->hostThread().removeTimer(id);
            if (release) handler->release();
        }
        fds_.clear();
        timers_.clear();
        owner_ = nullptr;
    }

    tresult PLUGIN_API registerEventHandler(Linux::IEventHandler* handler, Linux::FileDescriptor fd) override {
        if (!handler || !owner_ || !owner_->hostThread().isCurrent()) return kInvalidArgument;
        handler->addRef();
        fds_.emplace(handler, owner_->hostThread().watchFd(fd, HostThread::FdRead, [this, handler, fd](uint32_t) {
            call("fd handler", handler, [&] { handler->onFDIsSet(fd); });
        }));
        return kResultTrue;
    }
    tresult PLUGIN_API unregisterEventHandler(Linux::IEventHandler* handler) override {
        return forget(fds_, handler, [&](uint32_t id) { owner_->hostThread().unwatchFd(id); });
    }
    tresult PLUGIN_API registerTimer(Linux::ITimerHandler* handler, Linux::TimerInterval ms) override {
        if (!handler || !owner_ || !owner_->hostThread().isCurrent()) return kInvalidArgument;
        handler->addRef();
        const uint32_t period = (uint32_t)std::clamp<Linux::TimerInterval>(ms, 1, 60000);
        timers_.emplace(handler, owner_->hostThread().addTimer(period, [this, handler] {
            call("timer", handler, [&] { handler->onTimer(); });
        }));
        return kResultTrue;
    }
    tresult PLUGIN_API unregisterTimer(Linux::ITimerHandler* handler) override {
        return forget(timers_, handler, [&](uint32_t id) { owner_->hostThread().removeTimer(id); });
    }

private:
    // Held while it runs: the handler may unregister itself.
    template <typename F>
    void call(const char* what, FUnknown* handler, F&& f) {
        handler->addRef();
        PluginInstance& owner = *owner_;
        owner.callPlugin(what, f);
        if (!owner.crashed()) handler->release();
    }
    template <typename Handler, typename Stop>
    tresult forget(std::multimap<Handler*, uint32_t>& registered, Handler* handler, Stop stop) {
        auto [first, last] = registered.equal_range(handler);
        if (first == last) return kInvalidArgument;
        for (auto it = first; it != last; ++it) {
            stop(it->second);
            handler->release();
        }
        registered.erase(first, last);
        return kResultTrue;
    }

    PluginInstance* owner_;  // none once cleared
    std::multimap<Linux::IEventHandler*, uint32_t> fds_;    // -> HostThread::watchFd() id
    std::multimap<Linux::ITimerHandler*, uint32_t> timers_;  // -> HostThread::addTimer() id
};
#endif

class HostApplication final : public U::Implements<U::Directly<IHostApplication, IPlugInterfaceSupport>> {
public:
#if SMTG_OS_LINUX
    // `runLoop`: what the plugin finds through this context (an instance's); none for a module's.
    explicit HostApplication(IPtr<RunLoop> runLoop = nullptr) : runLoop_(std::move(runLoop)) {}

    tresult PLUGIN_API queryInterface(const TUID wanted, void** obj) override {
        if (runLoop_ && obj && FUnknownPrivate::iidEqual(wanted, Linux::IRunLoop::iid.toTUID())) {
            runLoop_->addRef();
            *obj = static_cast<Linux::IRunLoop*>(runLoop_.get());
            return kResultOk;
        }
        return U::Implements<U::Directly<IHostApplication, IPlugInterfaceSupport>>::queryInterface(wanted, obj);
    }
#endif
    tresult PLUGIN_API getName(String128 name) override {
        toUtf16("Brack", name);
        return kResultOk;
    }
    tresult PLUGIN_API createInstance(TUID cid, TUID wanted, void** obj) override {
        if (!obj) return kInvalidArgument;
        *obj = nullptr;
        if (FUnknownPrivate::iidEqual(cid, IMessage::iid.toTUID()) && FUnknownPrivate::iidEqual(wanted, IMessage::iid.toTUID())) {
            *obj = static_cast<IMessage*>(new HostMessage);
            return kResultOk;
        }
        if (FUnknownPrivate::iidEqual(cid, IAttributeList::iid.toTUID()) &&
            FUnknownPrivate::iidEqual(wanted, IAttributeList::iid.toTUID())) {
            *obj = static_cast<IAttributeList*>(new HostAttributeList);
            return kResultOk;
        }
        return kNoInterface;
    }
    tresult PLUGIN_API isPlugInterfaceSupported(const TUID id) override {
        for (const FUID* s : {&IComponent::iid, &IAudioProcessor::iid, &IEditController::iid, &IConnectionPoint::iid,
                              &IMidiMapping::iid, &IPlugViewContentScaleSupport::iid})
            if (FUnknownPrivate::iidEqual(id, s->toTUID())) return kResultTrue;
        return kResultFalse;
    }

#if SMTG_OS_LINUX
private:
    IPtr<RunLoop> runLoop_;
#endif
};

// The host context of modules (IPluginFactory3::setHostContext). Never released.
FUnknown* hostApplication() {
    static HostApplication* app = new HostApplication;
    return static_cast<IHostApplication*>(app);
}

class MemoryStream final : public U::Implements<U::Directly<IBStream, ISizeableStream>> {
public:
    MemoryStream() = default;
    MemoryStream(const uint8_t* data, size_t size) : data_(data, data + size) {}
    const std::vector<uint8_t>& data() const { return data_; }

    tresult PLUGIN_API read(void* buffer, int32 numBytes, int32* numBytesRead) override {
        if (numBytes < 0 || (!buffer && numBytes)) return kInvalidArgument;
        const int64 n = std::max<int64>(0, std::min<int64>(numBytes, (int64)data_.size() - pos_));
        if (n) std::memcpy(buffer, data_.data() + pos_, (size_t)n);
        pos_ += n;
        if (numBytesRead) *numBytesRead = (int32)n;
        return kResultOk;
    }
    tresult PLUGIN_API write(void* buffer, int32 numBytes, int32* numBytesWritten) override {
        if (numBytes < 0 || (!buffer && numBytes)) return kInvalidArgument;
        if ((size_t)(pos_ + numBytes) > data_.size()) data_.resize((size_t)(pos_ + numBytes));
        if (numBytes) std::memcpy(data_.data() + pos_, buffer, (size_t)numBytes);
        pos_ += numBytes;
        if (numBytesWritten) *numBytesWritten = numBytes;
        return kResultOk;
    }
    tresult PLUGIN_API seek(int64 pos, int32 mode, int64* result) override {
        int64 base = mode == kIBSeekSet ? 0 : mode == kIBSeekCur ? pos_ : (int64)data_.size();
        if (mode != kIBSeekSet && mode != kIBSeekCur && mode != kIBSeekEnd) return kInvalidArgument;
        if (base + pos < 0) return kResultFalse;
        pos_ = base + pos;
        if (result) *result = pos_;
        return kResultOk;
    }
    tresult PLUGIN_API tell(int64* pos) override {
        if (!pos) return kInvalidArgument;
        *pos = pos_;
        return kResultOk;
    }
    tresult PLUGIN_API getStreamSize(int64& size) override {
        size = (int64)data_.size();
        return kResultOk;
    }
    tresult PLUGIN_API setStreamSize(int64 size) override {
        if (size < 0) return kInvalidArgument;
        data_.resize((size_t)size);
        pos_ = std::min(pos_, size);
        return kResultOk;
    }

private:
    std::vector<uint8_t> data_;
    int64 pos_ = 0;
};

// Pre-allocated event list; the audio thread fills and clears it without allocating.
class EventList final : public U::Implements<U::Directly<IEventList>> {
public:
    explicit EventList(size_t capacity) : events_(capacity) {}
    void clear() { count_ = 0; }
    int32 PLUGIN_API getEventCount() override { return (int32)count_; }
    tresult PLUGIN_API getEvent(int32 index, Event& e) override {
        if (index < 0 || (size_t)index >= count_) return kInvalidArgument;
        e = events_[(size_t)index];
        return kResultOk;
    }
    tresult PLUGIN_API addEvent(Event& e) override {
        if (count_ >= events_.size()) return kResultFalse;
        events_[count_++] = e;
        return kResultOk;
    }

private:
    std::vector<Event> events_;
    size_t count_ = 0;
};

class ParamQueue final : public U::Implements<U::Directly<IParamValueQueue>> {
public:
    ParamQueue() : points_(64) {}
    void reset(ParamID id) {
        id_ = id;
        count_ = 0;
    }
    bool lastValue(ParamValue& v) const {
        if (!count_) return false;
        v = points_[count_ - 1].second;
        return true;
    }
    ParamID PLUGIN_API getParameterId() override { return id_; }
    int32 PLUGIN_API getPointCount() override { return (int32)count_; }
    tresult PLUGIN_API getPoint(int32 index, int32& sampleOffset, ParamValue& value) override {
        if (index < 0 || (size_t)index >= count_) return kInvalidArgument;
        sampleOffset = points_[(size_t)index].first;
        value = points_[(size_t)index].second;
        return kResultOk;
    }
    tresult PLUGIN_API addPoint(int32 sampleOffset, ParamValue value, int32& index) override {
        // Several points at one offset: the last one wins.
        if (count_ && points_[count_ - 1].first == sampleOffset) {
            points_[count_ - 1].second = value;
            index = (int32)count_ - 1;
            return kResultOk;
        }
        if (count_ >= points_.size()) return kResultFalse;
        points_[count_] = {sampleOffset, value};
        index = (int32)count_++;
        return kResultOk;
    }

private:
    ParamID id_ = kNoParamId;
    std::vector<std::pair<int32, ParamValue>> points_;
    size_t count_ = 0;
};

class ParamChanges final : public U::Implements<U::Directly<IParameterChanges>> {
public:
    explicit ParamChanges(size_t capacity) {
        for (size_t i = 0; i < capacity; ++i) queues_.push_back(owned(new ParamQueue));
    }
    void clear() { used_ = 0; }
    ParamQueue* queue(size_t i) { return queues_[i]; }
    size_t used() const { return used_; }
    int32 PLUGIN_API getParameterCount() override { return (int32)used_; }
    IParamValueQueue* PLUGIN_API getParameterData(int32 index) override {
        return index >= 0 && (size_t)index < used_ ? queues_[(size_t)index].get() : nullptr;
    }
    IParamValueQueue* PLUGIN_API addParameterData(const ParamID& id, int32& index) override {
        for (size_t i = 0; i < used_; ++i)
            if (queues_[i]->getParameterId() == id) {
                index = (int32)i;
                return queues_[i];
            }
        if (used_ >= queues_.size()) return nullptr;
        queues_[used_]->reset(id);
        index = (int32)used_;
        return queues_[used_++];
    }

private:
    std::vector<IPtr<ParamQueue>> queues_;
    size_t used_ = 0;
};

// ---------------------------------------------------------------------------
// module

// The module's entry and exit functions differ by OS (Steinberg's VST3 module architecture):
//   Windows  bool InitDll() / bool ExitDll()                        both optional
//   Linux    bool ModuleEntry(void* library) / bool ModuleExit()    both required
//   macOS    bool bundleEntry(CFBundleRef) / bool bundleExit()      both required
#if defined(_WIN32)
constexpr const char* kModuleEntry = "InitDll";
constexpr const char* kModuleExit = "ExitDll";
constexpr bool kModuleEntryRequired = false;
#elif defined(__APPLE__)
constexpr const char* kModuleEntry = "bundleEntry";
constexpr const char* kModuleExit = "bundleExit";
constexpr bool kModuleEntryRequired = true;
#else
constexpr const char* kModuleEntry = "ModuleEntry";
constexpr const char* kModuleExit = "ModuleExit";
constexpr bool kModuleEntryRequired = true;
#endif

bool callModuleEntry(void* fn, void* library, void* bundle) {
#if defined(_WIN32)
    (void)library;
    (void)bundle;
    return reinterpret_cast<bool(PLUGIN_API*)()>(fn)();
#elif defined(__APPLE__)
    (void)library;
    return reinterpret_cast<bool(PLUGIN_API*)(CFBundleRef)>(fn)(static_cast<CFBundleRef>(bundle));
#else
    (void)bundle;
    return reinterpret_cast<bool(PLUGIN_API*)(void*)>(fn)(library);
#endif
}

#ifdef __APPLE__
void* createBundle(const std::filesystem::path& path) {
    const std::string utf8 = pathToUtf8(path);
    CFURLRef url = CFURLCreateFromFileSystemRepresentation(nullptr, reinterpret_cast<const UInt8*>(utf8.data()),
                                                           (CFIndex)utf8.size(), true);
    if (!url) return nullptr;
    CFBundleRef bundle = CFBundleCreate(nullptr, url);
    CFRelease(url);
    return bundle;
}
#endif

// Every call into the module is guarded: a crash in its entry function, GetPluginFactory,
// setHostContext or listing its classes marks it broken for good (markModuleBroken()).
struct Vst3Module : std::enable_shared_from_this<Vst3Module> {
    std::string path;  // as given: the bundle folder or the .vst3 file
    void* library = nullptr;
    void* bundle = nullptr;
    IPluginFactory* factory = nullptr;
    bool initialised = false;  // the entry function succeeded (or there is none): the exit is due

    ~Vst3Module() {
        GuardFault fault;
        const bool ok = guardedCall([&] {
            if (factory) factory->release();
            using ExitFn = bool(PLUGIN_API*)();
            if (library && initialised)
                if (auto exit = reinterpret_cast<ExitFn>(librarySymbol(library, kModuleExit))) exit();
        }, fault);
        if (!ok) {
            // Too late to keep this object, but the library stays loaded and is not used again.
            const std::string report = faultReport("unloading", fault);
            logWarn(path + ": " + report + "; it stays loaded");
            markModuleBroken(path, report, nullptr);
            return;
        }
        if (library) closeLibrary(library);
#ifdef __APPLE__
        if (bundle) CFRelease(bundle);
#endif
    }

    // The audio processor classes. False (with `error`) if the module crashed listing them.
    bool describe(std::vector<PluginDescription>& out, std::string& error) const {
        out.clear();
        GuardFault fault;
        std::vector<PluginDescription> result;
        if (!guardedCall([&] { result = listClasses(); }, fault)) {
            error = faultReport("listing its plugins", fault);
            markModuleBroken(path, error, std::const_pointer_cast<Vst3Module>(shared_from_this()));
            return false;
        }
        out = std::move(result);
        return true;
    }

private:
    std::vector<PluginDescription> listClasses() const {
        std::vector<PluginDescription> result;
        PFactoryInfo fi{};
        factory->getFactoryInfo(&fi);
        FUnknownPtr<IPluginFactory2> f2(factory);
        FUnknownPtr<IPluginFactory3> f3(factory);
        const int32 n = factory->countClasses();
        for (int32 i = 0; i < n; ++i) {
            PClassInfo ci{};
            if (factory->getClassInfo(i, &ci) != kResultOk || std::strcmp(ci.category, kVstAudioEffectClass) != 0)
                continue;
            PluginDescription d;
            d.format = PluginFormat::Vst3;
            d.path = path;
            d.id = uidToString(ci.cid);
            d.name = ci.name;
            d.vendor = fi.vendor;
            std::string subCategories;
            PClassInfoW w{};
            PClassInfo2 c2{};
            if (f3 && f3->getClassInfoUnicode(i, &w) == kResultOk) {
                d.name = fromUtf16(w.name);
                if (w.vendor[0]) d.vendor = fromUtf16(w.vendor);
                d.version = fromUtf16(w.version);
                subCategories = w.subCategories;
            } else if (f2 && f2->getClassInfo2(i, &c2) == kResultOk) {
                if (c2.vendor[0]) d.vendor = c2.vendor;
                d.version = c2.version;
                subCategories = c2.subCategories;
            }
            for (size_t start = 0; start < subCategories.size();) {
                size_t end = subCategories.find('|', start);
                if (end == std::string::npos) end = subCategories.size();
                if (end > start) d.features.push_back(subCategories.substr(start, end - start));
                start = end + 1;
            }
            for (auto& f : d.features)
                if (f == PlugType::kInstrument) d.instrument = true;
            if (d.name.empty()) d.name = d.id;
            result.push_back(std::move(d));
        }
        return result;
    }
};

std::mutex g_moduleMutex;
std::map<std::string, std::weak_ptr<Vst3Module>> g_modules;

std::shared_ptr<Vst3Module> loadModule(const std::string& pathUtf8, std::string& error) {
    std::lock_guard lock(g_moduleMutex);
    auto abs = std::filesystem::absolute(pathFromUtf8(pathUtf8)).lexically_normal();
    std::string key = pathToUtf8(abs);
    if (moduleBroken(key, error)) return nullptr;
    if (auto it = g_modules.find(key); it != g_modules.end())
        if (auto m = it->second.lock()) return m;
    auto m = std::make_shared<Vst3Module>();
    m->path = key;
    auto binary = pluginBinaryPath(PluginFormat::Vst3, abs);
    std::error_code ec;
    if (!std::filesystem::is_regular_file(binary, ec)) {
        error = std::string("no ") + buildArchitecture() + " binary in the bundle (" +
                pathToUtf8(binary.parent_path().filename()) + ")";
        return nullptr;
    }
    m->library = openLibrary(pathToUtf8(binary), error);
    if (!m->library) return nullptr;
    // Guarded call by call, so that the lock above is always released normally.
    GuardFault fault;
    auto crashed = [&](const char* what) {
        error = faultReport(what, fault);
        markModuleBroken(key, error, m);
        return nullptr;
    };
    bool initialised = true;
#ifdef __APPLE__
    if (!(m->bundle = createBundle(abs))) {
        error = "not a bundle";
        return nullptr;
    }
#endif
    if (void* entry = librarySymbol(m->library, kModuleEntry)) {
        if (!guardedCall([&] { initialised = callModuleEntry(entry, m->library, m->bundle); }, fault))
            return crashed(kModuleEntry);
    } else if (kModuleEntryRequired) {
        error = std::string("not a VST3 plugin (no ") + kModuleEntry + ")";
        return nullptr;
    }
    if (!initialised) {
        error = std::string(kModuleEntry) + " failed";
        return nullptr;
    }
    m->initialised = true;
    using FactoryFn = IPluginFactory*(PLUGIN_API*)();
    auto getFactory = reinterpret_cast<FactoryFn>(librarySymbol(m->library, "GetPluginFactory"));
    if (!getFactory) {
        error = "not a VST3 plugin (no GetPluginFactory)";
        return nullptr;
    }
    if (!guardedCall([&] { m->factory = getFactory(); }, fault)) return crashed("GetPluginFactory");
    if (!m->factory) {
        error = "GetPluginFactory returned nothing";
        return nullptr;
    }
    if (!guardedCall([&] {
            if (FUnknownPtr<IPluginFactory3> f3(m->factory); f3) f3->setHostContext(hostApplication());
        }, fault))
        return crashed("setHostContext");
    g_modules[key] = m;
    return m;
}

// ---------------------------------------------------------------------------
// instance

class Vst3Instance;

class ComponentHandler final : public U::Implements<U::Directly<IComponentHandler, IComponentHandler2>> {
public:
    explicit ComponentHandler(Vst3Instance* owner) : owner_(owner) {}
    void detach() { owner_ = nullptr; }
    tresult PLUGIN_API beginEdit(ParamID) override { return kResultOk; }
    tresult PLUGIN_API performEdit(ParamID id, ParamValue value) override;
    tresult PLUGIN_API endEdit(ParamID) override;
    tresult PLUGIN_API restartComponent(int32 flags) override;
    // IComponentHandler2
    tresult PLUGIN_API setDirty(TBool state) override;
    tresult PLUGIN_API requestOpenEditor(FIDString) override { return kNotImplemented; }
    tresult PLUGIN_API startGroupEdit() override { return kResultOk; }
    tresult PLUGIN_API finishGroupEdit() override { return kResultOk; }

private:
    std::atomic<Vst3Instance*> owner_;
};

class Vst3Editor;

class PlugFrame final : public U::Implements<U::Directly<IPlugFrame>> {
public:
    explicit PlugFrame(Vst3Editor* editor, PluginInstance& owner) : editor_(editor) {
#if SMTG_OS_LINUX
        runLoop_ = owned(new RunLoop(owner));
#else
        (void)owner;
#endif
    }
    // `release`: false for a crashed plugin (see RunLoop::clear()).
    void detach(bool release) {
        editor_ = nullptr;
#if SMTG_OS_LINUX
        runLoop_->clear(release);
#else
        (void)release;
#endif
    }
    tresult PLUGIN_API resizeView(IPlugView* view, ViewRect* newSize) override;
    tresult PLUGIN_API queryInterface(const TUID wanted, void** obj) override {
#if SMTG_OS_LINUX
        if (obj && FUnknownPrivate::iidEqual(wanted, Linux::IRunLoop::iid.toTUID())) {
            runLoop_->addRef();
            *obj = static_cast<Linux::IRunLoop*>(runLoop_.get());
            return kResultOk;
        }
#endif
        return U::Implements<U::Directly<IPlugFrame>>::queryInterface(wanted, obj);
    }

private:
    Vst3Editor* editor_;
#if SMTG_OS_LINUX
    IPtr<RunLoop> runLoop_;
#endif
};

class Vst3Editor final : public PluginEditor {
public:
    Vst3Editor(Vst3Instance& owner, IEditController* controller) : owner_(owner), controller_(controller) {}
    ~Vst3Editor() override {
        if (view_) {
            if (attached_) view_->removed();
            view_->setFrame(nullptr);
        }
        if (frame_) frame_->detach(true);
        view_ = nullptr;
        window_.reset();
    }
    bool open(const std::string& title, std::string& error);
    tresult onResizeRequest(IPlugView* view, ViewRect* r) {
        if (!r || !window_ || view != view_.get()) return kInvalidArgument;
        window_->setClientSize((uint32_t)std::max(r->getWidth(), 1), (uint32_t)std::max(r->getHeight(), 1));
        view_->onSize(r);
        return kResultTrue;
    }
    void show() override { window_->show(); }
    void hide() override { window_->hide(); }
    void setTitle(const std::string& title) override { window_->setTitle(title); }
    void abandon() override {
        if (window_) window_->abandon();
        (void)window_.release();
        (void)view_.take();  // not released: that would run the plugin's code
        if (frame_) frame_->detach(false);
        attached_ = false;
    }

private:
    Vst3Instance& owner_;
    IEditController* controller_;
    IPtr<IPlugView> view_;
    IPtr<PlugFrame> frame_;
    std::unique_ptr<HostWindow> window_;
    bool attached_ = false;
};

tresult PLUGIN_API PlugFrame::resizeView(IPlugView* view, ViewRect* newSize) {
    return editor_ ? editor_->onResizeRequest(view, newSize) : kResultFalse;
}

// MIDI controller -> parameter assignments of the controller (IMidiMapping), read on the
// host thread and published to the audio thread whole.
struct MidiMap {
    static constexpr int kControllers = kCtrlProgramChange + 1;  // CCs, aftertouch, pitch bend, program
    size_t buses = 0;
    std::vector<ParamID> ids;            // [bus][channel][controller]
    std::vector<int32> programSteps;     // [bus][channel]: step count of the program parameter
    ParamID id(size_t bus, int ch, int ctrl) const { return ids[(bus * 16 + (size_t)ch) * kControllers + (size_t)ctrl]; }
};

class Vst3Instance final : public PluginInstance {
public:
    Vst3Instance(HostThread& host, PluginHostListener& listener, std::shared_ptr<Vst3Module> module,
                 PluginDescription desc, std::string displayName)
        : PluginInstance(host, listener, std::move(desc), std::move(displayName)), module_(std::move(module)) {}

    ~Vst3Instance() override {
        shutdown();
        if (handler_) handler_->detach();
        callPlugin("destroy", [&] {
            if (componentCP_ && controllerCP_) {
                componentCP_->disconnect(controllerCP_);
                controllerCP_->disconnect(componentCP_);
            }
            if (controller_) controller_->setComponentHandler(nullptr);
            if (controller_ && !singleComponent_) controller_->terminate();
            componentCP_ = nullptr;
            controllerCP_ = nullptr;
            midiMapping_ = nullptr;
            controller_ = nullptr;
            processor_ = nullptr;
            if (component_) component_->terminate();
            component_ = nullptr;
#if SMTG_OS_LINUX
            if (runLoop_) runLoop_->clear(true);  // what the plugin left registered
#endif
        });
        if (crashed()) {
#if SMTG_OS_LINUX
            if (runLoop_) runLoop_->clear(false);
#endif
            // Leave the plugin's objects as they are: releasing them runs its code.
            for (auto* p : {&componentCP_, &controllerCP_}) (void)p->take();
            (void)midiMapping_.take();
            (void)controller_.take();
            (void)processor_.take();
            (void)component_.take();
            keepForever(module_);  // not unloaded under a broken plugin
        }
    }

    bool initPlugin(std::string& error) override {
        TUID cid;
        if (!uidFromString(desc_.id, cid)) {
            error = "invalid VST3 class id: " + desc_.id;
            return false;
        }
        IComponent* c = nullptr;
        if (module_->factory->createInstance(cid, IComponent::iid, reinterpret_cast<void**>(&c)) != kResultOk || !c) {
            error = "VST3 plugin failed to instantiate";
            return false;
        }
        component_ = owned(c);
#if SMTG_OS_LINUX
        runLoop_ = owned(new RunLoop(*this));
        hostContext_ = owned(new HostApplication(runLoop_));
#else
        hostContext_ = owned(new HostApplication);
#endif
        if (component_->initialize(static_cast<IHostApplication*>(hostContext_.get())) != kResultOk) {
            error = "VST3 component failed to initialize";
            component_ = nullptr;
            return false;
        }
        processor_ = FUnknownPtr<IAudioProcessor>(component_);
        if (!processor_) {
            error = "VST3 component is not an audio processor";
            return false;
        }

        // The edit controller: the component itself, or a separate object to create and connect.
        if (FUnknownPtr<IEditController> ec(component_); ec) {
            controller_ = ec;
            singleComponent_ = true;
        } else {
            TUID ccid;
            IEditController* e = nullptr;
            if (component_->getControllerClassId(ccid) == kResultTrue &&
                module_->factory->createInstance(ccid, IEditController::iid, reinterpret_cast<void**>(&e)) == kResultOk &&
                e) {
                controller_ = owned(e);
                if (controller_->initialize(static_cast<IHostApplication*>(hostContext_.get())) != kResultOk) {
                    logWarn(desc_.name + ": VST3 edit controller failed to initialize");
                    controller_ = nullptr;
                }
            }
        }
        if (controller_) {
            handler_ = owned(new ComponentHandler(this));
            controller_->setComponentHandler(handler_);
            if (!singleComponent_) {
                componentCP_ = FUnknownPtr<IConnectionPoint>(component_);
                controllerCP_ = FUnknownPtr<IConnectionPoint>(controller_);
                if (componentCP_ && controllerCP_) {
                    componentCP_->connect(controllerCP_);
                    controllerCP_->connect(componentCP_);
                }
            }
            syncControllerFromComponent();
            midiMapping_ = FUnknownPtr<IMidiMapping>(controller_);
        }

        inEvents_ = owned(new EventList(4096));
        outEvents_ = owned(new EventList(1024));
        inParams_ = owned(new ParamChanges(256));
        outParams_ = owned(new ParamChanges(256));
        queryPorts();
        return true;
    }

    void idlePlugin() override {
        if (controller_) {
            ParamRing::Change c;
            while (toController_.pop(c)) controller_->setParamNormalized(c.id, c.value);
        }
        if (midiMapDirty_.exchange(false) && isActive()) buildMidiMap();
    }

    bool saveStatePlugin(std::vector<uint8_t>& out) override {
        auto cs = owned(new MemoryStream), ks = owned(new MemoryStream);
        if (component_->getState(cs) != kResultOk) return false;
        if (controller_) controller_->getState(ks);
        out.assign(kStateMagic, kStateMagic + 4);
        auto put = [&](const std::vector<uint8_t>& v) {
            uint64_t n = v.size();
            uint8_t b[8];
            std::memcpy(b, &n, 8);
            out.insert(out.end(), b, b + 8);
            out.insert(out.end(), v.begin(), v.end());
        };
        put(cs->data());
        put(ks->data());
        return true;
    }

    bool loadStatePlugin(const std::vector<uint8_t>& in) override {
        if (in.size() < 4 || std::memcmp(in.data(), kStateMagic, 4) != 0) return false;
        size_t pos = 4;
        auto take = [&](const uint8_t*& p, uint64_t& n) {
            if (in.size() - pos < 8) return false;
            std::memcpy(&n, in.data() + pos, 8);
            pos += 8;
            if (in.size() - pos < n) return false;
            p = in.data() + pos;
            pos += (size_t)n;
            return true;
        };
        const uint8_t *cp = nullptr, *kp = nullptr;
        uint64_t cn = 0, kn = 0;
        if (!take(cp, cn) || !take(kp, kn)) return false;
        auto cs = owned(new MemoryStream(cp, (size_t)cn));
        if (component_->setState(cs) != kResultOk) return false;
        if (controller_) {
            cs->seek(0, IBStream::kIBSeekSet, nullptr);
            controller_->setComponentState(cs);
            if (kn) {
                auto ks = owned(new MemoryStream(kp, (size_t)kn));
                controller_->setState(ks);
            }
        }
        return true;
    }

    bool pluginHasGui() const override { return controller_ != nullptr; }
    uint32_t pluginLatency() const override { return processorLatency_; }

    // ---- IComponentHandler ----
    void onPerformEdit(ParamID id, ParamValue value) {
        toProcessor_.push(id, value);
        stateChanged();
    }
    void onStateDirty() { stateChanged(); }
    void onRestartComponent(int32 flags) {
        if (flags & kParamValuesChanged) stateChanged();
        auto apply = [this, flags] {
            if (flags & kMidiCCAssignmentChanged) midiMapDirty_ = true;
            if (flags & kIoChanged) portsChanged();
            if (flags & (kReloadComponent | kLatencyChanged)) requestRestart();
        };
        if (host_.isCurrent()) apply();
        else
            host_.post([apply, alive = std::weak_ptr<int>(lifeToken_)] {
                if (alive.lock()) apply();
            });
    }

private:
    void syncControllerFromComponent() {
        auto s = owned(new MemoryStream);
        if (component_->getState(s) == kResultOk) {
            s->seek(0, IBStream::kIBSeekSet, nullptr);
            controller_->setComponentState(s);
        }
    }

    void queryPorts() override {
        notePorts_.clear();
        audioIns_.clear();
        audioOuts_.clear();
        for (int32 i = 0, n = component_->getBusCount(kEvent, kInput); i < n; ++i) {
            BusInfo bi{};
            if (component_->getBusInfo(kEvent, kInput, i, bi) != kResultOk) continue;
            notePorts_.push_back({(clap_id)i, fromUtf16(bi.name), CLAP_NOTE_DIALECT_MIDI});
        }
        auto audio = [&](BusDirection dir, std::vector<AudioPortInfo>& out) {
            for (int32 i = 0, n = component_->getBusCount(kAudio, dir); i < n; ++i) {
                BusInfo bi{};
                if (component_->getBusInfo(kAudio, dir, i, bi) != kResultOk) bi.channelCount = 0;
                out.push_back({(clap_id)i, fromUtf16(bi.name), (uint32_t)std::max(bi.channelCount, 0), bi.busType == kMain});
            }
        };
        audio(kInput, audioIns_);
        audio(kOutput, audioOuts_);
    }

    bool activatePlugin(double sampleRate, uint32_t maxFrames, std::string& error) override {
        ProcessSetup setup{kRealtime, kSample32, (int32)maxFrames, sampleRate};
        if (tresult r = processor_->setupProcessing(setup); r != kResultOk && r != kNotImplemented) {
            error = "VST3 setupProcessing failed (" + desc_.name + ")";
            return false;
        }
        for (size_t i = 0; i < audioOuts_.size(); ++i) component_->activateBus(kAudio, kOutput, (int32)i, true);
        for (size_t i = 0; i < notePorts_.size(); ++i) component_->activateBus(kEvent, kInput, (int32)i, true);

        inBufs_.assign(audioIns_.size(), AudioBusBuffers{});
        for (size_t i = 0; i < inBufs_.size(); ++i) {
            inBufs_[i].numChannels = (int32)audioIns_[i].channels;
            inBufs_[i].silenceFlags = ~0ull;
            inBufs_[i].channelBuffers32 = inPtrs_[i].empty() ? nullptr : inPtrs_[i].data();
        }
        outBufs_.assign(audioOuts_.size(), AudioBusBuffers{});
        for (size_t i = 0; i < outBufs_.size(); ++i) {
            outBufs_[i].numChannels = (int32)audioOuts_[i].channels;
            outBufs_[i].channelBuffers32 = outPtrs_[i].empty() ? nullptr : outPtrs_[i].data();
        }
        data_ = ProcessData{};
        data_.processMode = kRealtime;
        data_.symbolicSampleSize = kSample32;
        data_.numInputs = (int32)inBufs_.size();
        data_.numOutputs = (int32)outBufs_.size();
        data_.inputs = inBufs_.empty() ? nullptr : inBufs_.data();
        data_.outputs = outBufs_.empty() ? nullptr : outBufs_.data();
        data_.inputParameterChanges = inParams_;
        data_.outputParameterChanges = outParams_;
        data_.inputEvents = inEvents_;
        data_.outputEvents = outEvents_;
        data_.processContext = &context_;
        // brack has no transport: a stopped timeline at 120 bpm, 4/4.
        context_ = ProcessContext{};
        context_.state = ProcessContext::kTempoValid | ProcessContext::kTimeSigValid | ProcessContext::kContTimeValid;
        context_.sampleRate = sampleRate;
        context_.tempo = 120.0;
        context_.timeSigNumerator = 4;
        context_.timeSigDenominator = 4;

        buildMidiMap();
        if (tresult r = component_->setActive(true); r != kResultOk && r != kNotImplemented) {
            error = "VST3 setActive failed (" + desc_.name + ")";
            return false;
        }
        processorLatency_ = processor_->getLatencySamples();
        return true;
    }

    void deactivatePlugin() override {
        component_->setActive(false);
        // The audio thread is done with every map but the current one.
        std::erase_if(midiMaps_, [&](auto& m) { return m.get() != midiMap_.load(); });
    }

    bool startProcessing() override {
        processor_->setProcessing(true);  // kNotImplemented is fine
        return true;
    }

    void stopProcessing() override { processor_->setProcessing(false); }

    void buildMidiMap() {
        if (!midiMapping_ || notePorts_.empty()) {
            midiMap_.store(nullptr, std::memory_order_release);
            return;
        }
        auto m = std::make_unique<MidiMap>();
        m->buses = notePorts_.size();
        m->ids.assign(m->buses * 16 * MidiMap::kControllers, kNoParamId);
        m->programSteps.assign(m->buses * 16, 0);
        std::map<ParamID, std::vector<size_t>> programParams;
        for (size_t b = 0; b < m->buses; ++b)
            for (int ch = 0; ch < 16; ++ch)
                for (int ctrl = 0; ctrl < MidiMap::kControllers; ++ctrl) {
                    ParamID id = kNoParamId;
                    if (midiMapping_->getMidiControllerAssignment((int32)b, (int16)ch, (CtrlNumber)ctrl, id) != kResultTrue)
                        continue;
                    m->ids[(b * 16 + (size_t)ch) * MidiMap::kControllers + (size_t)ctrl] = id;
                    if (ctrl == kCtrlProgramChange) programParams[id].push_back(b * 16 + (size_t)ch);
                }
        if (!programParams.empty())
            for (int32 i = 0, n = controller_->getParameterCount(); i < n; ++i) {
                ParameterInfo pi{};
                if (controller_->getParameterInfo(i, pi) != kResultOk) continue;
                if (auto it = programParams.find(pi.id); it != programParams.end())
                    for (size_t slot : it->second) m->programSteps[slot] = pi.stepCount;
            }
        midiMaps_.push_back(std::move(m));
        midiMap_.store(midiMaps_.back().get(), std::memory_order_release);
    }

    // A controller message as a parameter change, when the plugin maps that controller.
    void mapController(const MidiMap* map, size_t bus, int ch, int ctrl, ParamValue value, int32 offset) {
        if (!map || bus >= map->buses) return;
        const ParamID id = map->id(bus, ch, ctrl);
        if (id == kNoParamId) return;
        if (ctrl == kCtrlProgramChange) {
            const int32 steps = map->programSteps[bus * 16 + (size_t)ch];
            value = steps > 0 ? std::min(value * 127.0, (double)steps) / steps : value;
        }
        int32 index = 0;
        if (IParamValueQueue* q = inParams_->addParameterData(id, index)) q->addPoint(offset, value, index);
        toController_.push(id, value);
    }

    bool processPlugin(uint32_t frames, uint64_t steadyTime, const InputEventList& in) override {
        inEvents_->clear();
        outEvents_->clear();
        inParams_->clear();
        outParams_->clear();

        ParamRing::Change c;
        while (toProcessor_.pop(c)) {
            int32 index = 0;
            if (IParamValueQueue* q = inParams_->addParameterData(c.id, index)) q->addPoint(0, c.value, index);
        }

        const MidiMap* map = midiMap_.load(std::memory_order_acquire);
        for (size_t i = 0; i < in.size(); ++i) {
            const clap_event_header_t* h = in.at(i);
            if (h->space_id != CLAP_CORE_EVENT_SPACE_ID) continue;
            Event e{};
            e.sampleOffset = (int32)h->time;
            e.flags = Event::kIsLive;
            if (h->type == CLAP_EVENT_MIDI_SYSEX) {
                auto* s = reinterpret_cast<const clap_event_midi_sysex_t*>(h);
                e.busIndex = s->port_index;
                e.type = Event::kDataEvent;
                e.data.size = s->size;
                e.data.type = DataEvent::kMidiSysEx;
                e.data.bytes = s->buffer;
                inEvents_->addEvent(e);
                continue;
            }
            if (h->type != CLAP_EVENT_MIDI) continue;
            auto* m = reinterpret_cast<const clap_event_midi_t*>(h);
            const uint8_t type = m->data[0] & 0xF0, d1 = m->data[1] & 0x7F, d2 = m->data[2] & 0x7F;
            const int ch = m->data[0] & 0x0F;
            e.busIndex = m->port_index;
            switch (type) {
                case 0x90:
                    if (d2 > 0) {
                        e.type = Event::kNoteOnEvent;
                        e.noteOn = {(int16)ch, (int16)d1, 0.0f, d2 / 127.0f, 0, -1};
                        inEvents_->addEvent(e);
                        break;
                    }
                    [[fallthrough]];
                case 0x80:
                    e.type = Event::kNoteOffEvent;
                    e.noteOff = {(int16)ch, (int16)d1, d2 / 127.0f, -1, 0.0f};
                    inEvents_->addEvent(e);
                    break;
                case 0xA0:
                    e.type = Event::kPolyPressureEvent;
                    e.polyPressure = {(int16)ch, (int16)d1, d2 / 127.0f, -1};
                    inEvents_->addEvent(e);
                    break;
                case 0xB0: mapController(map, m->port_index, ch, d1, d2 / 127.0, e.sampleOffset); break;
                case 0xC0: mapController(map, m->port_index, ch, kCtrlProgramChange, d1 / 127.0, e.sampleOffset); break;
                case 0xD0: mapController(map, m->port_index, ch, kAfterTouch, d1 / 127.0, e.sampleOffset); break;
                case 0xE0:
                    mapController(map, m->port_index, ch, kPitchBend, ((d2 << 7) | d1) / 16383.0, e.sampleOffset);
                    break;
                default: break;  // system messages have no VST3 equivalent
            }
        }

        context_.projectTimeSamples = (TSamples)steadyTime;
        context_.continousTimeSamples = (TSamples)steadyTime;
        data_.numSamples = (int32)frames;
        for (auto& b : outBufs_) b.silenceFlags = 0;
        processor_->process(data_);
        // A channel flagged silent need not have been written: make it silent.
        for (size_t i = 0; i < outBufs_.size(); ++i)
            for (size_t ch = 0; ch < outPtrs_[i].size() && ch < 64; ++ch)
                if (outBufs_[i].silenceFlags & (1ull << ch)) std::memset(outPtrs_[i][ch], 0, frames * sizeof(float));

        for (size_t i = 0; i < outParams_->used(); ++i) {
            ParamValue v;
            if (outParams_->queue(i)->lastValue(v)) toController_.push(outParams_->queue(i)->getParameterId(), v);
        }
        return true;
    }

    std::unique_ptr<PluginEditor> createEditor(std::string& error) override {
        auto e = std::make_unique<Vst3Editor>(*this, controller_);
        if (!e->open(displayName(), error)) return nullptr;
        return e;
    }

    std::shared_ptr<Vst3Module> module_;
    IPtr<HostApplication> hostContext_;  // the instance's own
#if SMTG_OS_LINUX
    IPtr<RunLoop> runLoop_;  // found through it
#endif
    IPtr<IComponent> component_;
    IPtr<IAudioProcessor> processor_;
    IPtr<IEditController> controller_;
    bool singleComponent_ = false;
    IPtr<IConnectionPoint> componentCP_, controllerCP_;
    IPtr<IMidiMapping> midiMapping_;
    IPtr<ComponentHandler> handler_;
    uint32_t processorLatency_ = 0;

    // processing (audio thread, set up while inactive)
    std::vector<AudioBusBuffers> inBufs_, outBufs_;
    ProcessData data_{};
    ProcessContext context_{};
    IPtr<EventList> inEvents_, outEvents_;
    IPtr<ParamChanges> inParams_, outParams_;

    ParamRing toProcessor_;   // edits from the controller (any thread) -> audio thread
    ParamRing toController_;  // audio thread -> controller (host thread, idle())
    std::vector<std::unique_ptr<MidiMap>> midiMaps_;  // host thread; the audio thread reads midiMap_
    std::atomic<MidiMap*> midiMap_{nullptr};
    std::atomic<bool> midiMapDirty_{false};
};

tresult PLUGIN_API ComponentHandler::performEdit(ParamID id, ParamValue value) {
    if (auto* o = owner_.load()) o->onPerformEdit(id, value);
    return kResultOk;
}

tresult PLUGIN_API ComponentHandler::endEdit(ParamID) {
    if (auto* o = owner_.load()) o->onStateDirty();
    return kResultOk;
}

tresult PLUGIN_API ComponentHandler::restartComponent(int32 flags) {
    if (auto* o = owner_.load()) o->onRestartComponent(flags);
    return kResultOk;
}

tresult PLUGIN_API ComponentHandler::setDirty(TBool state) {
    if (auto* o = owner_.load(); o && state) o->onStateDirty();
    return kResultOk;
}

bool Vst3Editor::open(const std::string& title, std::string& error) {
    view_ = owned(controller_->createView(ViewType::kEditor));
    if (!view_) {
        error = "plugin has no editor";
        return false;
    }
    if (view_->isPlatformTypeSupported(vst3PlatformType()) != kResultTrue) {
        error = std::string("plugin editor does not support ") + vst3PlatformType() + " windows";
        return false;
    }
    frame_ = owned(new PlugFrame(this, owner_));
    view_->setFrame(frame_);

    FUnknownPtr<IPlugViewContentScaleSupport> scaling(view_);
    HostWindow::Callbacks cb;
    cb.resized = [this](uint32_t& w, uint32_t& h) {
        owner_.callPlugin("editor resize", [&] {
            ViewRect r(0, 0, (int32)w, (int32)h);
            if (view_->checkSizeConstraint(&r) == kResultTrue && r.getWidth() > 0 && r.getHeight() > 0) {
                w = (uint32_t)r.getWidth();
                h = (uint32_t)r.getHeight();
            }
            ViewRect size(0, 0, (int32)w, (int32)h);
            view_->onSize(&size);
        });
    };
    cb.scaleChanged = [this](double scale) {
        owner_.callPlugin("editor scale", [&] {
            if (FUnknownPtr<IPlugViewContentScaleSupport> s(view_); s) s->setContentScaleFactor((float)scale);
        });
    };
    cb.closeRequested = [this] { owner_.editorClosedByUser(); };
    window_ = HostWindow::create(title, view_->canResize() == kResultTrue, std::move(cb), owner_.editorPlacement(),
                                 error);
    if (!window_) return false;
    if (scaling) scaling->setContentScaleFactor((float)window_->scale());

    ViewRect r{};
    if (view_->getSize(&r) != kResultOk || r.getWidth() <= 0 || r.getHeight() <= 0) r = ViewRect(0, 0, 800, 600);
    window_->setClientSize((uint32_t)r.getWidth(), (uint32_t)r.getHeight());
    if (view_->attached(window_->nativeHandle(), vst3PlatformType()) != kResultOk) {
        error = "plugin refused the editor window";
        return false;
    }
    attached_ = true;
    window_->show();
    return true;
}

// ---- scanning ----

}  // namespace

std::unique_ptr<PluginInstance> createVst3Instance(HostThread& host, PluginHostListener& listener,
                                                   const std::string& pathUtf8, const std::string& pluginId,
                                                   std::string& error) {
    auto module = loadModule(pathUtf8, error);
    if (!module) return nullptr;
    std::vector<PluginDescription> descs;
    if (!module->describe(descs, error)) return nullptr;
    auto it = std::find_if(descs.begin(), descs.end(), [&](auto& d) { return pluginId.empty() || d.id == pluginId; });
    if (it == descs.end()) {
        error = pluginId.empty() ? "no audio processors in the file" : "plugin " + pluginId + " not found";
        return nullptr;
    }
    PluginDescription desc = *it;
    std::string name = desc.name;
    return std::make_unique<Vst3Instance>(host, listener, std::move(module), std::move(desc), name);
}

std::vector<PluginDescription> describeVst3File(const std::filesystem::path& file, std::string& error) {
    auto module = loadModule(pathToUtf8(file), error);
    if (!module) return {};
    std::vector<PluginDescription> result;
    if (!module->describe(result, error)) return {};
    if (result.empty()) error = "no audio processors in the file";
    return result;
}

}  // namespace brack
