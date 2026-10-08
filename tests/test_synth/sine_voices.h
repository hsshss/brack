#pragma once
// The sine synth and event log shared by the VST2 and VST3 test plugins (the CLAP test
// synth has its own copy). Received events are kept as text lines and appended to the file
// named by BRACK_TESTSYNTH_LOG (if set) when the plugin is destroyed.

#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

struct SineVoices {
    struct Voice {
        bool on = false;
        int channel = 0, key = 0;
        double phase = 0, freq = 0, amp = 0;
    };
    Voice voices[16];
    double sampleRate = 48000;
    float gain = 0.25f;
    std::vector<std::string> received;

    SineVoices() { received.reserve(4096); }  // tests only send a few messages

    void noteOn(int ch, int key, double vel) {
        for (auto& v : voices)
            if (!v.on) {
                v = {true, ch, key, 0.0, 440.0 * std::pow(2.0, (key - 69) / 12.0), vel};
                return;
            }
    }
    void noteOff(int ch, int key) {
        for (auto& v : voices)
            if (v.on && v.channel == ch && v.key == key) v.on = false;
    }
    void render(float* l, float* r, int frames) {
        for (int i = 0; i < frames; ++i) {
            double acc = 0;
            for (auto& v : voices) {
                if (!v.on) continue;
                acc += std::sin(v.phase) * v.amp;
                v.phase += 2.0 * 3.14159265358979323846 * v.freq / sampleRate;
                if (v.phase > 6.283185307179586) v.phase -= 6.283185307179586;
            }
            l[i] = (float)(acc * gain);
            if (r) r[i] = l[i];
        }
    }
    void record(const char* fmt, ...) {
        char buf[256];
        va_list ap;
        va_start(ap, fmt);
        std::vsnprintf(buf, sizeof buf, fmt, ap);
        va_end(ap);
        received.emplace_back(buf);
    }
    void recordBytes(const char* kind, const unsigned char* d, size_t n) {
        std::string line = kind;
        line += ":";
        char buf[8];
        for (size_t i = 0; i < n; ++i) {
            std::snprintf(buf, sizeof buf, " %02X", d[i]);
            line += buf;
        }
        received.push_back(std::move(line));
    }
    void flushLog() {
        const char* path = std::getenv("BRACK_TESTSYNTH_LOG");
        if (!path) return;
        if (FILE* f = std::fopen(path, "ab")) {
            for (auto& l : received) std::fprintf(f, "%s\n", l.c_str());
            std::fclose(f);
        }
    }
};
