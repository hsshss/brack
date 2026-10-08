// Render positions to process frames across changes of the output's rate (a new device).
#include <cstdio>

#include "util/frame_map.h"

using namespace brack;

namespace {

int g_failures = 0;
#define CHECK(cond)                                                       \
    do {                                                                  \
        if (!(cond)) {                                                    \
            std::fprintf(stderr, "CHECK failed: %s (line %d)\n", #cond, __LINE__); \
            ++g_failures;                                                 \
        }                                                                 \
    } while (0)

}  // namespace

int main() {
    {
        FrameMap m;
        m.setRates(0, 48000, 48000);
        CHECK(m.processFrame(0) == 0);
        CHECK(m.processFrame(123457) == 123457);
    }
    {
        // Plugins at 44.1 kHz: 10 s on a 44.1 kHz device, 29 s on a 192 kHz one, then back.
        FrameMap m;
        m.setRates(0, 44100, 44100);
        const uint64_t first = 10ull * 44100;
        CHECK(m.processFrame(first) == first);
        m.setRates(first, 192000, 44100);
        const uint64_t second = first + 29ull * 192000;
        CHECK(m.processFrame(first + 192000) == first + 44100);
        CHECK(m.processFrame(second) == first + 29ull * 44100);
        m.setRates(second, 44100, 44100);
        CHECK(m.processFrame(second + 256) == first + 29ull * 44100 + 256);
        CHECK(m.processFrame(first) == first + 29ull * 44100);  // from before the change: due now
    }
    {
        // Plugins at 96 kHz through the SRC, the device going from 44.1 kHz to 48 kHz.
        FrameMap m;
        m.setRates(0, 44100, 96000);
        CHECK(m.processFrame(44100) == 96000);
        m.setRates(44100, 48000, 96000);
        CHECK(m.processFrame(44100 + 48000) == 2 * 96000);
    }
    if (g_failures) std::fprintf(stderr, "%d failure(s)\n", g_failures);
    else std::printf("frame map: ok\n");
    return g_failures ? 1 : 0;
}
