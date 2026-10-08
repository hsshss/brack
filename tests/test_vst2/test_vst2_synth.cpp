// Minimal VST2 instrument used by brack's tests: a sine synth with a stereo output.
// MIDI CC 7 sets the gain (0.25 * value / 127), which is also the state (a 4-byte chunk),
// and reports the change to the host as an automated parameter.
// Every MIDI and SysEx event is logged (see sine_voices.h).

#include <vst.h>

#include <cstring>

#include "../../src/core/vst2/vst2_abi.h"
#include "../test_synth/sine_voices.h"

namespace {

struct Synth : SineVoices {
    vst_effect_t effect{};
    Vst2HostCallback host = nullptr;
    bool sentEvents = false;
    vst_event_midi_t outEvent{};
    Vst2EventList<1> outEvents;
};

Synth* S(vst_effect_t* e) { return static_cast<Synth*>(e->effect_internal); }

void handleEvents(Synth* s, const Vst2EventListHeader* events) {
    for (int32_t i = 0; i < events->count; ++i) {
        const vst_event_t* ev = events->events[i];
        if (ev->type == VST_EVENT_TYPE_MIDI) {
            auto* m = reinterpret_cast<const vst_event_midi_t*>(ev);
            const auto* d = reinterpret_cast<const unsigned char*>(m->midi.data);
            s->recordBytes("vst2 midi", d, 3);
            const int st = d[0] & 0xF0, ch = d[0] & 0x0F;
            if (st == 0x90 && d[2] > 0) s->noteOn(ch, d[1], d[2] / 127.0);
            else if (st == 0x80 || st == 0x90) s->noteOff(ch, d[1]);
            else if (st == 0xB0 && d[1] == 7) {
                s->gain = 0.25f * d[2] / 127.0f;
                s->host(&s->effect, VST_HOST_OPCODE_AUTOMATE, 0, 0, nullptr, s->gain / 0.25f);
            }
        } else if (ev->type == VST_EVENT_TYPE_MIDI_SYSEX) {
            auto* x = reinterpret_cast<const vst_event_midi_sysex_t*>(ev);
            s->recordBytes("vst2 sysex", reinterpret_cast<const unsigned char*>(x->sysex.data), (size_t)x->sysex.size);
        }
    }
}

void copyString(void* dst, const char* src) { std::strcpy(static_cast<char*>(dst), src); }

intptr_t VST_FUNCTION_INTERFACE control(vst_effect_t* e, int32_t opcode, int32_t, intptr_t value, void* ptr, float opt) {
    Synth* s = S(e);
    switch (opcode) {
        case VST_EFFECT_OPCODE_CREATE: return 0;
        case VST_EFFECT_OPCODE_DESTROY:
            s->flushLog();
            delete s;
            return 0;
        case VST_EFFECT_OPCODE_SET_SAMPLE_RATE: s->sampleRate = opt; return 0;
        case VST_EFFECT_OPCODE_SUSPEND_RESUME: s->record("vst2 resume %d", (int)value); return 0;
        case VST_EFFECT_OPCODE_PROCESS_BEGIN: s->record("vst2 process-begin"); return 0;
        case VST_EFFECT_OPCODE_PROCESS_END: s->record("vst2 process-end"); return 0;
        case VST_EFFECT_OPCODE_EFFECT_NAME: copyString(ptr, "brack test synth vst2"); return 1;
        case VST_EFFECT_OPCODE_VENDOR_NAME: copyString(ptr, "brack"); return 1;
        case VST_EFFECT_OPCODE_PRODUCT_NAME: copyString(ptr, "brack test synth"); return 1;
        case VST_EFFECT_OPCODE_VENDOR_VERSION: return 1000;
        case VST_EFFECT_OPCODE_VST_VERSION: return 2400;
        case VST_EFFECT_OPCODE_CATEGORY: return VST_EFFECT_CATEGORY_INSTRUMENT;
        case VST_EFFECT_OPCODE_SUPPORTS:
            return ptr && (!std::strcmp((const char*)ptr, "receiveVstMidiEvent") ||
                           !std::strcmp((const char*)ptr, "receiveVstEvents"))
                       ? 1
                       : 0;
        case VST_EFFECT_OPCODE_EVENT: handleEvents(s, static_cast<const Vst2EventListHeader*>(ptr)); return 1;
        case VST_EFFECT_OPCODE_GET_CHUNK_DATA: *static_cast<void**>(ptr) = &s->gain; return sizeof(float);
        case VST_EFFECT_OPCODE_SET_CHUNK_DATA:
            if (value == sizeof(float)) std::memcpy(&s->gain, ptr, sizeof(float));
            return 1;
        default: return 0;
    }
}

void VST_FUNCTION_INTERFACE process(vst_effect_t* e, const float* const*, float** outputs, int32_t frames) {
    Synth* s = S(e);
    if (!s->sentEvents) {
        // Once: send a note to the host the way plugins do, and log whether it took it.
        s->sentEvents = true;
        s->outEvent.event.type = VST_EVENT_TYPE_MIDI;
        s->outEvent.event.size = (int32_t)(sizeof(s->outEvent) - 2 * sizeof(int32_t));
        s->outEvent.midi.data[0] = (char)(unsigned char)0x90;
        s->outEvent.midi.data[1] = 0x40;
        s->outEvent.midi.data[2] = 0x40;
        s->outEvents.count = 1;
        s->outEvents.events[0] = &s->outEvent.event;
        s->record("vst2 host took events: %d", (int)s->host(e, kVst2HostProcessEvents, 0, 0, &s->outEvents, 0));
    }
    s->render(outputs[0], outputs[1], frames);
}

void VST_FUNCTION_INTERFACE setParameter(vst_effect_t* e, uint32_t index, float value) {
    if (index == 0) S(e)->gain = value * 0.25f;
}

float VST_FUNCTION_INTERFACE getParameter(vst_effect_t* e, uint32_t index) { return index == 0 ? S(e)->gain / 0.25f : 0.0f; }

}  // namespace

#ifdef _WIN32
#define EXPORTED __declspec(dllexport)
#else
#define EXPORTED __attribute__((visibility("default")))
#endif

extern "C" EXPORTED vst_effect_t* VSTPluginMain(Vst2HostCallback callback) {
    if (!callback || callback(nullptr, VST_HOST_OPCODE_VST_VERSION, 0, 0, nullptr, 0) < 2400) return nullptr;
    auto* s = new Synth();
    s->host = callback;
    vst_effect_t& e = s->effect;
    e.magic_number = VST_MAGICNUMBER;
    e.control = &control;
    e.process = &process;
    e.process_float = &process;
    e.set_parameter = &setParameter;
    e.get_parameter = &getParameter;
    e.num_programs = 1;
    e.num_params = 1;
    e.num_inputs = 0;
    e.num_outputs = 2;
    e.flags = VST_EFFECT_FLAG_INSTRUMENT | VST_EFFECT_FLAG_SUPPORTS_FLOAT | VST_EFFECT_FLAG_CHUNKS;
    e.input_output_ratio = 1.0f;
    e.effect_internal = s;
    e.unique_id = (int32_t)VST_FOURCC('B', 'r', 'T', '2');
    e.version = 1000;
    return &e;
}
