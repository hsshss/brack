// Minimal VST2 effect used by brack's tests: two inputs times a gain on two outputs. The gain
// (0.5 until set) is the state, a 4-byte chunk.

#include <vst.h>

#include <cstring>

#include "../../src/core/vst2/vst2_abi.h"

namespace {

struct Gain {
    vst_effect_t effect{};
    float gain = 0.5f;
};

Gain* G(vst_effect_t* e) { return static_cast<Gain*>(e->effect_internal); }

void copyString(void* dst, const char* src) { std::strcpy(static_cast<char*>(dst), src); }

intptr_t VST_FUNCTION_INTERFACE control(vst_effect_t* e, int32_t opcode, int32_t, intptr_t value, void* ptr, float) {
    Gain* g = G(e);
    switch (opcode) {
        case VST_EFFECT_OPCODE_DESTROY: delete g; return 0;
        case VST_EFFECT_OPCODE_EFFECT_NAME: copyString(ptr, "brack test gain vst2"); return 1;
        case VST_EFFECT_OPCODE_VENDOR_NAME: copyString(ptr, "brack"); return 1;
        case VST_EFFECT_OPCODE_PRODUCT_NAME: copyString(ptr, "brack test gain"); return 1;
        case VST_EFFECT_OPCODE_VENDOR_VERSION: return 1000;
        case VST_EFFECT_OPCODE_VST_VERSION: return 2400;
        case VST_EFFECT_OPCODE_CATEGORY: return VST_EFFECT_CATEGORY_EFFECT;
        case VST_EFFECT_OPCODE_GET_CHUNK_DATA: *static_cast<void**>(ptr) = &g->gain; return sizeof(float);
        case VST_EFFECT_OPCODE_SET_CHUNK_DATA:
            if (value == sizeof(float)) std::memcpy(&g->gain, ptr, sizeof(float));
            return 1;
        default: return 0;
    }
}

void VST_FUNCTION_INTERFACE process(vst_effect_t* e, const float* const* inputs, float** outputs, int32_t frames) {
    const float gain = G(e)->gain;
    for (int c = 0; c < 2; ++c)
        for (int32_t i = 0; i < frames; ++i) outputs[c][i] = inputs[c][i] * gain;
}

void VST_FUNCTION_INTERFACE setParameter(vst_effect_t*, uint32_t, float) {}
float VST_FUNCTION_INTERFACE getParameter(vst_effect_t*, uint32_t) { return 0.0f; }

}  // namespace

#ifdef _WIN32
#define EXPORTED __declspec(dllexport)
#else
#define EXPORTED __attribute__((visibility("default")))
#endif

extern "C" EXPORTED vst_effect_t* VSTPluginMain(Vst2HostCallback callback) {
    if (!callback || callback(nullptr, VST_HOST_OPCODE_VST_VERSION, 0, 0, nullptr, 0) < 2400) return nullptr;
    auto* g = new Gain();
    vst_effect_t& e = g->effect;
    e.magic_number = VST_MAGICNUMBER;
    e.control = &control;
    e.process = &process;
    e.process_float = &process;
    e.set_parameter = &setParameter;
    e.get_parameter = &getParameter;
    e.num_programs = 1;
    e.num_inputs = 2;
    e.num_outputs = 2;
    e.flags = VST_EFFECT_FLAG_SUPPORTS_FLOAT | VST_EFFECT_FLAG_CHUNKS;
    e.input_output_ratio = 1.0f;
    e.effect_internal = g;
    e.unique_id = (int32_t)VST_FOURCC('B', 'r', 'G', '2');
    e.version = 1000;
    return &e;
}
