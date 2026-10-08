#pragma once
// Windows: audio threads join MMCSS's "Pro Audio" task, which runs them in the real-time priority
// range. Brack's output thread joins through miniaudio (config.wasapi.usage), a plugin host's
// audio thread by itself (BrackLink::raiseAudioThreadPriority()); Brack logs what came of it.

#include <string>

namespace brack::realtime {

// Logs what came of it for `what` ("audio output thread", say) whenever that differs from the
// last time: real-time when `why` is empty, else normal priority because of `why`.
void report(const char* what, const std::string& why);

}  // namespace brack::realtime
