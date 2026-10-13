#pragma once
// Engine implementation details shared by engine.cpp and session.cpp.
#include <atomic>
#include <memory>
#include <string>
#include <vector>

#include "engine.h"
#include "midi/midi_input.h"
#include "util/midi_queue.h"
#include "util/pending_midi.h"

namespace brack {

// An effect of a chain.
struct Engine::EffectSlot {
    PluginConfig cfg;  // cfg.id is final; cfg.state is what it saves back while not loaded
    bool bypass = false;
    HostedPlugin* inst = nullptr;  // in the chain; null while not loaded
    std::string status;
};

// An instrument and its effects, or the master's effects (master_, with an empty cfg).
struct Engine::PluginSlot {
    PluginConfig cfg;  // the instrument's: cfg.id is final; cfg.state is what a failed slot will save back
    std::vector<EffectSlot> effects;  // in the chain's order, those not loaded too
    std::shared_ptr<HostedChain> chain;  // shared with an addEffect() preparing an effect for it
    std::unique_ptr<MidiQueue> direct = std::make_unique<MidiQueue>(1 << 16);
    // Direct messages not yet due, and its sources' while its chain changes.
    std::unique_ptr<PendingMidi> pending = std::make_unique<PendingMidi>();
    std::unique_ptr<InputEventList> events = std::make_unique<InputEventList>();
    std::string status;
    // Host thread: EngineEvent::PluginCrashed sent for the chain's crash; cleared once the chain is
    // gone or no longer crashed.
    bool crashReported = false;
    // Host thread: how far its chain is into a change (quiesceSlot()), for the graph. Its MIDI waits
    // meanwhile: a plugin asked to stop drops its block's events.
    enum class Change {
        None,
        Stopping,  // processed, with no MIDI, so the block it stops in has none to drop
        Out,       // not processed
        Removed,   // out of the graph: the slot goes
    } change = Change::None;

    HostedPlugin* instrument() const { return chain && chain->hasInstrument() ? &chain->plugin(0) : nullptr; }
    // Where effects[i] is in the chain (once it is `chain`), or would go if it were loaded.
    size_t chainIndex(size_t i) const {
        size_t at = chain && chain->hasInstrument() ? 1 : 0;
        for (size_t e = 0; e < i && e < effects.size(); ++e) at += effects[e].inst != nullptr;
        return at;
    }
    // Its effects are no longer loaded.
    void unloadEffects() {
        for (auto& e : effects) e.inst = nullptr;
    }
    // Before its chain goes, while findAny() still finds its plugins, so that each is reported.
    void closeEditors() {
        if (chain)
            for (size_t i = 0; i < chain->size(); ++i) chain->plugin(i).closeGui();
    }
};

struct Engine::SourceSlot {
    MidiSourceConfig cfg;
    std::unique_ptr<MidiQueue> queue = std::make_unique<MidiQueue>(1 << 16);
    std::unique_ptr<PendingMidi> pending = std::make_unique<PendingMidi>();  // not yet due
    std::unique_ptr<MidiInputPort> port;
    std::string status;
    bool ok = false;
    std::atomic<uint64_t> count{0};
};

struct Engine::RtGraph {
    struct Out {
        uint32_t port, channel, output;
        float gain;
    };
    struct Node {
        HostedChain* chain;  // null while out (PluginSlot::Change::Out)
        MidiQueue* direct;
        PendingMidi* pending;
        InputEventList* events;
        bool holdMidi;  // its chain changes: its MIDI waits in `pending` and `direct`
        std::vector<Out> outs;
    };
    struct Target {
        uint32_t node;
        uint16_t port;
    };
    struct Source {
        MidiQueue* queue;
        PendingMidi* pending;
        std::vector<Target> targets;
    };
    std::vector<Node> nodes;
    std::vector<Source> sources;
    HostedChain* master = nullptr;  // processes the mix
    // The master's chain is out while it changes: silent, not the mix without it.
    bool muteMix = false;
};

}  // namespace brack
