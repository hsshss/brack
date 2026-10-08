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

struct Engine::PluginSlot {
    PluginConfig cfg;  // cfg.id is final; cfg.state is what a failed slot will save back
    std::unique_ptr<HostedPlugin> inst;
    std::unique_ptr<MidiQueue> direct = std::make_unique<MidiQueue>(1 << 16);
    std::unique_ptr<PendingMidi> pending = std::make_unique<PendingMidi>();  // direct, not yet due
    std::unique_ptr<InputEventList> events = std::make_unique<InputEventList>();
    std::string status;
    bool crashReported = false;  // host thread: EngineEvent::PluginCrashed sent
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
        HostedPlugin* plugin;
        MidiQueue* direct;
        PendingMidi* pending;
        InputEventList* events;
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
};

}  // namespace brack
