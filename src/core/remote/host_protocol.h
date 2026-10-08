#pragma once
// How Brack talks to a plugin host process (brack-host-<arch>.exe), which runs one plugin
// instance, or describes plugin files for a scan, so that a plugin that crashes or hangs takes
// only that process down, and a plugin built for another architecture than Brack's can run.
//
//   pipe           CBOR messages, each a 32-bit little-endian length and the bytes. Brack
//                  sends a request and waits for its reply; the host also sends notes at any
//                  time (a log line, the editor closed or resized, a restart wanted).
//   control block  shared memory Brack creates with the process: one block's MIDI in, the
//                  result out, a count of the plugin's state changes, and the record of a
//                  crash, which the host writes just before it ends itself.
//   outputs        shared memory the host creates at each activation: the plugin's output
//                  channels, maxFrames samples each, port by port.
//   process, done  from Brack, and from the host when it has processed the block: two events on
//                  Windows; elsewhere the control block's `turn`, waited on with a futex.
//
// The shared layouts use fixed-width fields only, so 32-bit and 64-bit processes agree on
// them. Handles cross as 64-bit numbers (Windows handle values fit in 32 bits either way).

#include <cstdint>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "plugin/plugin.h"

namespace brack::remote {

// Raised whenever a message or a shared layout changes: a host from another build refuses to
// start (exit code kExitWrongVersion) rather than misread them.
constexpr uint32_t kProtocolVersion = 1;

constexpr int kExitBadArguments = 100;
constexpr int kExitWrongVersion = 101;

constexpr uint32_t kMaxEvents = 4096;
constexpr uint32_t kMaxMessageBytes = 1u << 30;  // a plugin state can be large; a length past this is garbage
constexpr uint32_t kSysexBytes = 256 * 1024;

enum class BlockCommand : uint32_t {
    Process = 1,  // process `frames` frames with the events
    Stop = 2,     // stop processing (the plugin is about to be deactivated)
};

enum class BlockResult : uint32_t {
    Audio = 1,    // the outputs hold `frames` new frames
    Failed = 2,   // the plugin failed to start or to process, and is not called again
    Stopped = 3,  // answer to Stop
};

// POSIX: ControlBlock::turn, who has the block. Zero, as the shared memory starts, is Brack's.
enum class BlockTurn : uint32_t {
    Brack = 0,  // "done": the result is in, and Brack may fill the next block
    Host = 1,   // "process"
};

struct MidiEvent {
    uint32_t time;      // frame within the block
    uint16_t port;      // note port
    uint16_t size;      // bytes
    uint32_t offset;    // SysEx: where in ControlBlock::sysex
    uint8_t bytes[4];   // other messages
};
static_assert(sizeof(MidiEvent) == 16);

struct ControlBlock {
    uint32_t turn;  // BlockTurn, atomically (std::atomic_ref); not used on Windows
    // Brack -> host, before "process"
    uint32_t command;
    uint32_t frames;
    alignas(8) uint64_t steadyTime;  // explicitly: 32-bit x86 Linux aligns uint64_t to 4 in a struct
    uint32_t eventCount;
    // host -> Brack, before "done"
    uint32_t result;
    // Goes up whenever the plugin says its state changed, from any of its threads (atomically:
    // std::atomic_ref). Brack looks after each block and each request.
    uint32_t stateChanges;
    // Written by the host before it ends itself after a crash. crashCode is the exception code,
    // or 0 if only crashReport says what happened.
    uint32_t crashed;
    uint32_t crashCode;
    char crashReport[1024];
    MidiEvent events[kMaxEvents];
    uint8_t sysex[kSysexBytes];
};
static_assert(sizeof(ControlBlock) == 328752, "the same layout in 32-bit and 64-bit builds");

using Message = nlohmann::json;

// Frames a message for the pipe.
std::vector<uint8_t> encodeMessage(const Message& m);
// Reads the message in a frame's payload; false if it is not one.
bool decodeMessage(const std::vector<uint8_t>& payload, Message& out);

Message toMessage(const PluginDescription& d);
PluginDescription descriptionFromMessage(const Message& m);
Message portsToMessage(const std::vector<NotePortInfo>& notes, const std::vector<AudioPortInfo>& ins,
                       const std::vector<AudioPortInfo>& outs);
void portsFromMessage(const Message& m, std::vector<NotePortInfo>& notes, std::vector<AudioPortInfo>& ins,
                      std::vector<AudioPortInfo>& outs);

}  // namespace brack::remote
