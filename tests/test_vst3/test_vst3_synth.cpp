// Minimal VST3 instrument used by brack's tests: a single-component sine synth with one
// event input and a stereo output. Parameter 1 (volume, gain = 0.25 * value) is assigned
// to MIDI CC 7 through IMidiMapping and is the state. Events and parameter changes are
// logged (see sine_voices.h). A copy named crash-at-<stage> crashes in InitDll,
// GetPluginFactory, the factory's countClasses or ExitDll; see crash_stage.h. On Linux it runs a
// timer on the run loop its host context gives (Linux::IRunLoop), with no editor, and logs at
// terminate() whether it ticked: "vst3 run loop: ticked".

#include <pluginterfaces/base/ibstream.h>
#include <pluginterfaces/base/ipluginbase.h>
#include <pluginterfaces/gui/iplugview.h>
#include <pluginterfaces/vst/ivstaudioprocessor.h>
#include <pluginterfaces/vst/ivstcomponent.h>
#include <pluginterfaces/vst/ivsteditcontroller.h>
#include <pluginterfaces/vst/ivstevents.h>
#include <pluginterfaces/vst/ivstmidicontrollers.h>
#include <pluginterfaces/vst/ivstparameterchanges.h>

#include <atomic>
#include <cstring>

#include "../test_synth/crash_stage.h"
#include "../test_synth/sine_voices.h"

using namespace Steinberg;
using namespace Steinberg::Vst;

namespace {

const FUID kSynthCid(0x6272616B, 0x54657374, 0x56535433, 0x53796E31);
constexpr ParamID kVolume = 1;

void toUtf16(const char* s, char16* out, size_t size) {
    size_t i = 0;
    for (; s[i] && i + 1 < size; ++i) out[i] = (char16)s[i];
    out[i] = 0;
}

bool same(const TUID a, const FUID& b) { return FUnknownPrivate::iidEqual(a, b.toTUID()); }

#if SMTG_OS_LINUX
// A timer handler that counts; it lives as long as the synth.
class Ticker final : public Linux::ITimerHandler {
public:
    tresult PLUGIN_API queryInterface(const TUID wanted, void** obj) override {
        if (!same(wanted, FUnknown::iid) && !same(wanted, Linux::ITimerHandler::iid)) {
            *obj = nullptr;
            return kNoInterface;
        }
        *obj = static_cast<Linux::ITimerHandler*>(this);
        return kResultOk;
    }
    uint32 PLUGIN_API addRef() override { return 1; }
    uint32 PLUGIN_API release() override { return 1; }
    void PLUGIN_API onTimer() override { ++ticks; }
    std::atomic<int> ticks{0};
};
#endif

class Synth final : public IComponent, public IAudioProcessor, public IEditController, public IMidiMapping {
public:
    // ---- FUnknown ----
    tresult PLUGIN_API queryInterface(const TUID wanted, void** obj) override {
        if (same(wanted, FUnknown::iid) || same(wanted, IPluginBase::iid) || same(wanted, IComponent::iid))
            *obj = static_cast<IComponent*>(this);
        else if (same(wanted, IAudioProcessor::iid)) *obj = static_cast<IAudioProcessor*>(this);
        else if (same(wanted, IEditController::iid)) *obj = static_cast<IEditController*>(this);
        else if (same(wanted, IMidiMapping::iid)) *obj = static_cast<IMidiMapping*>(this);
        else {
            *obj = nullptr;
            return kNoInterface;
        }
        addRef();
        return kResultOk;
    }
    uint32 PLUGIN_API addRef() override { return ++refs_; }
    uint32 PLUGIN_API release() override {
        uint32 r = --refs_;
        if (r == 0) {
            voices_.flushLog();
            delete this;
        }
        return r;
    }

    // ---- IPluginBase ----
    tresult PLUGIN_API initialize(FUnknown* context) override {
#if SMTG_OS_LINUX
        void* runLoop = nullptr;
        if (context && context->queryInterface(Linux::IRunLoop::iid, &runLoop) == kResultOk && runLoop) {
            runLoop_ = static_cast<Linux::IRunLoop*>(runLoop);
            runLoop_->registerTimer(&ticker_, 10);
        }
#else
        (void)context;
#endif
        return kResultOk;
    }
    tresult PLUGIN_API terminate() override {
#if SMTG_OS_LINUX
        if (runLoop_) {
            runLoop_->unregisterTimer(&ticker_);
            runLoop_->release();
            runLoop_ = nullptr;
            voices_.record("vst3 run loop: %s", ticker_.ticks > 0 ? "ticked" : "silent");
        }
#endif
        return kResultOk;
    }

    // ---- IComponent ----
    tresult PLUGIN_API getControllerClassId(TUID) override { return kNotImplemented; }
    tresult PLUGIN_API setIoMode(IoMode) override { return kResultOk; }
    int32 PLUGIN_API getBusCount(MediaType type, BusDirection dir) override {
        return (type == kEvent && dir == kInput) || (type == kAudio && dir == kOutput) ? 1 : 0;
    }
    tresult PLUGIN_API getBusInfo(MediaType type, BusDirection dir, int32 index, BusInfo& bus) override {
        if (index != 0 || getBusCount(type, dir) == 0) return kInvalidArgument;
        bus.mediaType = type;
        bus.direction = dir;
        bus.channelCount = type == kEvent ? 16 : 2;
        toUtf16(type == kEvent ? "MIDI In" : "Main Out", bus.name, 128);
        bus.busType = kMain;
        bus.flags = BusInfo::kDefaultActive;
        return kResultOk;
    }
    tresult PLUGIN_API getRoutingInfo(RoutingInfo&, RoutingInfo&) override { return kNotImplemented; }
    tresult PLUGIN_API activateBus(MediaType, BusDirection, int32, TBool) override { return kResultOk; }
    tresult PLUGIN_API setActive(TBool state) override {
        voices_.record("vst3 active %d", state ? 1 : 0);
        return kResultOk;
    }
    tresult PLUGIN_API setState(IBStream* s) override { return readVolume(s) ? kResultOk : kResultFalse; }
    tresult PLUGIN_API getState(IBStream* s) override {
        double v = volume_;
        int32 n = 0;
        return s->write(&v, sizeof v, &n) == kResultOk && n == sizeof v ? kResultOk : kResultFalse;
    }

    // ---- IAudioProcessor ----
    tresult PLUGIN_API setBusArrangements(SpeakerArrangement*, int32, SpeakerArrangement*, int32) override {
        return kResultFalse;
    }
    tresult PLUGIN_API getBusArrangement(BusDirection dir, int32 index, SpeakerArrangement& arr) override {
        if (dir != kOutput || index != 0) return kInvalidArgument;
        arr = SpeakerArr::kStereo;
        return kResultOk;
    }
    tresult PLUGIN_API canProcessSampleSize(int32 size) override { return size == kSample32 ? kResultTrue : kResultFalse; }
    uint32 PLUGIN_API getLatencySamples() override { return 0; }
    tresult PLUGIN_API setupProcessing(ProcessSetup& setup) override {
        voices_.sampleRate = setup.sampleRate;
        return kResultOk;
    }
    tresult PLUGIN_API setProcessing(TBool state) override {
        voices_.record("vst3 processing %d", state ? 1 : 0);
        return kResultOk;
    }
    tresult PLUGIN_API process(ProcessData& data) override {
        if (IParameterChanges* pc = data.inputParameterChanges)
            for (int32 i = 0; i < pc->getParameterCount(); ++i) {
                IParamValueQueue* q = pc->getParameterData(i);
                int32 offset = 0, n = q ? q->getPointCount() : 0;
                ParamValue v = 0;
                if (n > 0 && q->getParameterId() == kVolume && q->getPoint(n - 1, offset, v) == kResultOk) {
                    volume_ = v;
                    voices_.record("vst3 param %u = %.3f", (unsigned)kVolume, v);
                }
            }
        if (IEventList* events = data.inputEvents)
            for (int32 i = 0; i < events->getEventCount(); ++i) {
                Event e{};
                if (events->getEvent(i, e) != kResultOk) continue;
                switch (e.type) {
                    case Event::kNoteOnEvent:
                        voices_.record("vst3 note-on bus=%d ch=%d key=%d vel=%.3f", e.busIndex, e.noteOn.channel,
                                       e.noteOn.pitch, e.noteOn.velocity);
                        voices_.noteOn(e.noteOn.channel, e.noteOn.pitch, e.noteOn.velocity);
                        break;
                    case Event::kNoteOffEvent:
                        voices_.record("vst3 note-off bus=%d ch=%d key=%d", e.busIndex, e.noteOff.channel, e.noteOff.pitch);
                        voices_.noteOff(e.noteOff.channel, e.noteOff.pitch);
                        break;
                    case Event::kPolyPressureEvent:
                        voices_.record("vst3 poly-pressure ch=%d key=%d %.3f", e.polyPressure.channel,
                                       e.polyPressure.pitch, e.polyPressure.pressure);
                        break;
                    case Event::kDataEvent:
                        if (e.data.type == DataEvent::kMidiSysEx) voices_.recordBytes("vst3 sysex", e.data.bytes, e.data.size);
                        break;
                    default: break;
                }
            }
        voices_.gain = 0.25f * (float)volume_;
        if (data.numOutputs > 0 && data.outputs[0].numChannels == 2)
            voices_.render(data.outputs[0].channelBuffers32[0], data.outputs[0].channelBuffers32[1], data.numSamples);
        return kResultOk;
    }
    uint32 PLUGIN_API getTailSamples() override { return 0; }

    // ---- IEditController ----
    tresult PLUGIN_API setComponentState(IBStream* s) override { return readVolume(s) ? kResultOk : kResultFalse; }
    // setState/getState: one pair serves IComponent and IEditController (same signature).
    int32 PLUGIN_API getParameterCount() override { return 1; }
    tresult PLUGIN_API getParameterInfo(int32 index, ParameterInfo& info) override {
        if (index != 0) return kInvalidArgument;
        info = {};
        info.id = kVolume;
        toUtf16("Volume", info.title, 128);
        info.defaultNormalizedValue = 1.0;
        info.flags = ParameterInfo::kCanAutomate;
        return kResultOk;
    }
    tresult PLUGIN_API getParamStringByValue(ParamID, ParamValue, String128 string) override {
        toUtf16("", string, 128);
        return kResultOk;
    }
    tresult PLUGIN_API getParamValueByString(ParamID, TChar*, ParamValue&) override { return kResultFalse; }
    ParamValue PLUGIN_API normalizedParamToPlain(ParamID, ParamValue v) override { return v; }
    ParamValue PLUGIN_API plainParamToNormalized(ParamID, ParamValue v) override { return v; }
    ParamValue PLUGIN_API getParamNormalized(ParamID id) override { return id == kVolume ? volume_ : 0.0; }
    tresult PLUGIN_API setParamNormalized(ParamID id, ParamValue) override { return id == kVolume ? kResultOk : kResultFalse; }
    tresult PLUGIN_API setComponentHandler(IComponentHandler*) override { return kResultOk; }
    IPlugView* PLUGIN_API createView(FIDString) override { return nullptr; }

    // ---- IMidiMapping ----
    tresult PLUGIN_API getMidiControllerAssignment(int32 bus, int16, CtrlNumber ctrl, ParamID& id) override {
        if (bus != 0 || ctrl != kCtrlVolume) return kResultFalse;
        id = kVolume;
        return kResultTrue;
    }

private:
    bool readVolume(IBStream* s) {
        double v = 0;
        int32 n = 0;
        if (!s || s->read(&v, sizeof v, &n) != kResultOk || n != sizeof v) return false;
        volume_ = v;
        return true;
    }

    std::atomic<uint32> refs_{1};
    SineVoices voices_;
    double volume_ = 1.0;
#if SMTG_OS_LINUX
    Linux::IRunLoop* runLoop_ = nullptr;
    Ticker ticker_;
#endif
};

class Factory final : public IPluginFactory2 {
public:
    tresult PLUGIN_API queryInterface(const TUID wanted, void** obj) override {
        if (same(wanted, FUnknown::iid) || same(wanted, IPluginFactory::iid) || same(wanted, IPluginFactory2::iid)) {
            *obj = static_cast<IPluginFactory2*>(this);
            return kResultOk;
        }
        *obj = nullptr;
        return kNoInterface;
    }
    uint32 PLUGIN_API addRef() override { return 1; }  // static
    uint32 PLUGIN_API release() override { return 1; }

    tresult PLUGIN_API getFactoryInfo(PFactoryInfo* info) override {
        std::memset(static_cast<void*>(info), 0, sizeof *info);
        std::strcpy(info->vendor, "brack");
        return kResultOk;
    }
    int32 PLUGIN_API countClasses() override {
        crashAt("countClasses");
        return 1;
    }
    tresult PLUGIN_API getClassInfo(int32 index, PClassInfo* info) override {
        if (index != 0) return kInvalidArgument;
        std::memset(static_cast<void*>(info), 0, sizeof *info);
        kSynthCid.toTUID(info->cid);
        info->cardinality = PClassInfo::kManyInstances;
        std::strcpy(info->category, kVstAudioEffectClass);
        std::strcpy(info->name, "brack test synth vst3");
        return kResultOk;
    }
    tresult PLUGIN_API getClassInfo2(int32 index, PClassInfo2* info) override {
        if (index != 0) return kInvalidArgument;
        std::memset(static_cast<void*>(info), 0, sizeof *info);
        kSynthCid.toTUID(info->cid);
        info->cardinality = PClassInfo::kManyInstances;
        std::strcpy(info->category, kVstAudioEffectClass);
        std::strcpy(info->name, "brack test synth vst3");
        std::strcpy(info->subCategories, "Instrument|Synth");
        std::strcpy(info->vendor, "brack");
        std::strcpy(info->version, "1.0.0");
        std::strcpy(info->sdkVersion, kVstVersionString);
        return kResultOk;
    }
    tresult PLUGIN_API createInstance(FIDString cid, FIDString wanted, void** obj) override {
        *obj = nullptr;
        if (!same(cid, kSynthCid)) return kNoInterface;
        auto* s = new Synth;
        tresult r = s->queryInterface(wanted, obj);
        s->release();
        return r;
    }
};

Factory g_factory;

}  // namespace

// Exported through test_vst3_synth.def on Windows: on 32-bit Windows these __stdcall functions
// are decorated (_GetPluginFactory@0), and hosts look for the plain names. macOS has
// bundleEntry / bundleExit for InitDll / ExitDll, Linux ModuleEntry / ModuleExit, required there.
extern "C" {
IPluginFactory* PLUGIN_API GetPluginFactory() {
    crashAt("GetPluginFactory");
    return &g_factory;
}
#ifdef _WIN32
bool PLUGIN_API InitDll() {
    crashAt("InitDll");
    return true;
}
bool PLUGIN_API ExitDll() {
    crashAt("ExitDll");
    return true;
}
#elif defined(__APPLE__)
bool bundleEntry(void*) {
    crashAt("bundleEntry");
    return true;
}
bool bundleExit() {
    crashAt("bundleExit");
    return true;
}
#else
bool ModuleEntry(void*) {
    crashAt("ModuleEntry");
    return true;
}
bool ModuleExit() {
    crashAt("ModuleExit");
    return true;
}
#endif
}
