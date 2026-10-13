// Minimal VST3 effect used by brack's tests: a single component with a stereo input and output,
// the input times a gain on the output. The gain (0.5 until set) is the state, a double.

#include <pluginterfaces/base/ibstream.h>
#include <pluginterfaces/base/ipluginbase.h>
#include <pluginterfaces/vst/ivstaudioprocessor.h>
#include <pluginterfaces/vst/ivstcomponent.h>
#include <pluginterfaces/vst/ivsteditcontroller.h>

#include <atomic>
#include <cstring>

using namespace Steinberg;
using namespace Steinberg::Vst;

namespace {

const FUID kGainCid(0x6272616B, 0x54657374, 0x56535433, 0x4761696E);

void toUtf16(const char* s, char16* out, size_t size) {
    size_t i = 0;
    for (; s[i] && i + 1 < size; ++i) out[i] = (char16)s[i];
    out[i] = 0;
}

bool same(const TUID a, const FUID& b) { return FUnknownPrivate::iidEqual(a, b.toTUID()); }

class Gain final : public IComponent, public IAudioProcessor, public IEditController {
public:
    // ---- FUnknown ----
    tresult PLUGIN_API queryInterface(const TUID wanted, void** obj) override {
        if (same(wanted, FUnknown::iid) || same(wanted, IPluginBase::iid) || same(wanted, IComponent::iid))
            *obj = static_cast<IComponent*>(this);
        else if (same(wanted, IAudioProcessor::iid)) *obj = static_cast<IAudioProcessor*>(this);
        else if (same(wanted, IEditController::iid)) *obj = static_cast<IEditController*>(this);
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
        if (r == 0) delete this;
        return r;
    }

    // ---- IPluginBase ----
    tresult PLUGIN_API initialize(FUnknown*) override { return kResultOk; }
    tresult PLUGIN_API terminate() override { return kResultOk; }

    // ---- IComponent ----
    tresult PLUGIN_API getControllerClassId(TUID) override { return kNotImplemented; }
    tresult PLUGIN_API setIoMode(IoMode) override { return kResultOk; }
    int32 PLUGIN_API getBusCount(MediaType type, BusDirection) override { return type == kAudio ? 1 : 0; }
    tresult PLUGIN_API getBusInfo(MediaType type, BusDirection dir, int32 index, BusInfo& bus) override {
        if (type != kAudio || index != 0) return kInvalidArgument;
        bus.mediaType = type;
        bus.direction = dir;
        bus.channelCount = 2;
        toUtf16(dir == kInput ? "Main In" : "Main Out", bus.name, 128);
        bus.busType = kMain;
        bus.flags = BusInfo::kDefaultActive;
        return kResultOk;
    }
    tresult PLUGIN_API getRoutingInfo(RoutingInfo&, RoutingInfo&) override { return kNotImplemented; }
    tresult PLUGIN_API activateBus(MediaType, BusDirection, int32, TBool) override { return kResultOk; }
    tresult PLUGIN_API setActive(TBool) override { return kResultOk; }
    tresult PLUGIN_API setState(IBStream* s) override {
        double g = 0;
        int32 n = 0;
        if (!s || s->read(&g, sizeof g, &n) != kResultOk || n != sizeof g) return kResultFalse;
        gain_ = g;
        return kResultOk;
    }
    tresult PLUGIN_API getState(IBStream* s) override {
        double g = gain_;
        int32 n = 0;
        return s->write(&g, sizeof g, &n) == kResultOk && n == sizeof g ? kResultOk : kResultFalse;
    }

    // ---- IAudioProcessor ----
    tresult PLUGIN_API setBusArrangements(SpeakerArrangement*, int32, SpeakerArrangement*, int32) override {
        return kResultFalse;
    }
    tresult PLUGIN_API getBusArrangement(BusDirection, int32 index, SpeakerArrangement& arr) override {
        if (index != 0) return kInvalidArgument;
        arr = SpeakerArr::kStereo;
        return kResultOk;
    }
    tresult PLUGIN_API canProcessSampleSize(int32 size) override { return size == kSample32 ? kResultTrue : kResultFalse; }
    uint32 PLUGIN_API getLatencySamples() override { return 0; }
    tresult PLUGIN_API setupProcessing(ProcessSetup&) override { return kResultOk; }
    tresult PLUGIN_API setProcessing(TBool) override { return kResultOk; }
    tresult PLUGIN_API process(ProcessData& data) override {
        if (data.numInputs < 1 || data.numOutputs < 1) return kResultOk;
        const AudioBusBuffers& in = data.inputs[0];
        AudioBusBuffers& out = data.outputs[0];
        for (int32 c = 0; c < 2 && c < in.numChannels && c < out.numChannels; ++c)
            for (int32 i = 0; i < data.numSamples; ++i)
                out.channelBuffers32[c][i] = in.channelBuffers32[c][i] * (float)gain_;
        out.silenceFlags = 0;
        return kResultOk;
    }
    uint32 PLUGIN_API getTailSamples() override { return 0; }

    // ---- IEditController ----
    tresult PLUGIN_API setComponentState(IBStream*) override { return kResultOk; }
    int32 PLUGIN_API getParameterCount() override { return 0; }
    tresult PLUGIN_API getParameterInfo(int32, ParameterInfo&) override { return kInvalidArgument; }
    tresult PLUGIN_API getParamStringByValue(ParamID, ParamValue, String128) override { return kResultFalse; }
    tresult PLUGIN_API getParamValueByString(ParamID, TChar*, ParamValue&) override { return kResultFalse; }
    ParamValue PLUGIN_API normalizedParamToPlain(ParamID, ParamValue v) override { return v; }
    ParamValue PLUGIN_API plainParamToNormalized(ParamID, ParamValue v) override { return v; }
    ParamValue PLUGIN_API getParamNormalized(ParamID) override { return 0.0; }
    tresult PLUGIN_API setParamNormalized(ParamID, ParamValue) override { return kResultFalse; }
    tresult PLUGIN_API setComponentHandler(IComponentHandler*) override { return kResultOk; }
    IPlugView* PLUGIN_API createView(FIDString) override { return nullptr; }

private:
    std::atomic<uint32> refs_{1};
    double gain_ = 0.5;
};

class Factory final : public IPluginFactory {
public:
    tresult PLUGIN_API queryInterface(const TUID wanted, void** obj) override {
        if (same(wanted, FUnknown::iid) || same(wanted, IPluginFactory::iid)) {
            *obj = static_cast<IPluginFactory*>(this);
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
    int32 PLUGIN_API countClasses() override { return 1; }
    tresult PLUGIN_API getClassInfo(int32 index, PClassInfo* info) override {
        if (index != 0) return kInvalidArgument;
        std::memset(static_cast<void*>(info), 0, sizeof *info);
        kGainCid.toTUID(info->cid);
        info->cardinality = PClassInfo::kManyInstances;
        std::strcpy(info->category, kVstAudioEffectClass);
        std::strcpy(info->name, "brack test gain vst3");
        return kResultOk;
    }
    tresult PLUGIN_API createInstance(FIDString cid, FIDString wanted, void** obj) override {
        *obj = nullptr;
        if (!same(cid, kGainCid)) return kNoInterface;
        auto* g = new Gain;
        tresult r = g->queryInterface(wanted, obj);
        g->release();
        return r;
    }
};

Factory g_factory;

}  // namespace

// Exported through test_gain_vst3.def on Windows (see test_vst3_synth.cpp).
extern "C" {
IPluginFactory* PLUGIN_API GetPluginFactory() { return &g_factory; }
#ifdef _WIN32
bool PLUGIN_API InitDll() { return true; }
bool PLUGIN_API ExitDll() { return true; }
#elif defined(__APPLE__)
bool bundleEntry(void*) { return true; }
bool bundleExit() { return true; }
#else
bool ModuleEntry(void*) { return true; }
bool ModuleExit() { return true; }
#endif
}
