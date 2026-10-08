#pragma once
// macOS: audio threads run under the time-constraint policy, Mach's real-time scheduling. Core
// Audio's own threads have it already, Brack's output thread among them; a plugin host's audio
// thread asks for it by itself, and joins a work interval (BrackLink::raiseAudioThreadPriority()).
// Brack logs what came of it.

#include <string>

namespace brack::realtime {

// Puts the calling thread under the time-constraint policy, as Core Audio's I/O threads are for a
// buffer of `periodFrames` at `sampleRate`. Empty, or why it could not.
std::string makeThisThreadRealtime(double sampleRate, unsigned periodFrames);

// The calling thread joins an audio work interval of its own, whose blocks then run between
// beginBlock() and endBlock(): the scheduler knows their deadline, and on Apple silicon gives them
// performance cores, which a time-constraint thread alone does not get. Empty, or why it could not.
// Making the first one takes about 100 ms: prepareWorkInterval() does it before any block.
void prepareWorkInterval();
std::string joinWorkInterval();
void beginBlock(double seconds);  // the block's deadline, from now
void endBlock();

// Whether the calling thread runs under the time-constraint policy.
bool thisThreadIsRealtime();

// Logs what came of it for `what` ("audio output thread", say) whenever that differs from the
// last time: real-time when `why` is empty, else normal priority because of `why`.
void report(const char* what, const std::string& why);

}  // namespace brack::realtime
