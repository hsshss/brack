#pragma once
// Corrections to vst2sdk (v0.4.0) for the parts brack uses, laid out the way VST2
// plugins and hosts actually are. Checked against the FST and VeSTige headers, which
// agree with each other here. Used by the host and by the test plugin, so the test fails
// if the two disagree with real plugins again.
//
//   - vst_events_t declares {int32 count; int32 reserved; vst_event_t** events}, but the
//     pointers follow a pointer-sized reserved field inline: on x64 the first is at
//     offset 16, not a pointer to an array at offset 8. Plugins built against the real
//     layout (Roland's Sound Canvas VA, for one) crash on vst_events_t.
//   - VST_HOST_OPCODE_EVENT is 9, but a plugin sends its events to the host with 8
//     (9 is the long-deprecated set-time request).
//   - vst_host_callback_t passes `value` as int64_t, but it is pointer sized: on 32-bit
//     targets an int64_t shifts every argument after it.

#include <vst.h>

#include <cstddef>
#include <cstdint>

template <size_t N>
struct Vst2EventList {
    int32_t count = 0;
    intptr_t reserved = 0;
    vst_event_t* events[N] = {};
};

// Header part only, for reading a list of any length.
using Vst2EventListHeader = Vst2EventList<1>;
static_assert(offsetof(Vst2EventListHeader, events) == 2 * sizeof(void*), "VST2 events layout");

// Plugin -> host: the plugin's outgoing events (ptr: a Vst2EventList).
constexpr int32_t kVst2HostProcessEvents = 8;

using Vst2HostCallback = intptr_t(VST_FUNCTION_INTERFACE*)(vst_effect_t* effect, int32_t opcode, int32_t index,
                                                            intptr_t value, void* ptr, float opt);
// The plugin's entry point (VSTPluginMain).
using Vst2MainFn = vst_effect_t*(VST_FUNCTION_INTERFACE*)(Vst2HostCallback host);
