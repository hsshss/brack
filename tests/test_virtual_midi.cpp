// MANUAL test (not run by ctest): publishes a virtual MIDI port through the
// engine, sends to it through WinMM like any other application would, and
// checks the test synth received the bytes unchanged.
//
// Warning: on Windows MIDI Services builds before the late-2026 servicing fix,
// removing a virtual device can deadlock MidiSrv until the next reboot.
#include <windows.h>
#include <mmsystem.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include "engine.h"
#include "util/common.h"

using namespace brack;

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: test_virtual_midi <brack-test-synth.clap> [port name]\n");
        return 2;
    }
    const std::string portName = argc > 2 ? argv[2] : "brack test port";
    auto logPath = std::filesystem::temp_directory_path() / ("brack_test_virtual-" + std::string(buildArchitecture()) + ".log");
    std::filesystem::remove(logPath);
    _putenv(("BRACK_TESTSYNTH_LOG=" + pathToUtf8(logPath)).c_str());

    int failures = 0;
    {
        Engine e;
        std::string err;
        std::string pid = e.addPlugin({"synth", argv[1], "brack.test.synth", "", std::nullopt}, true, err);
        std::string sid = e.addMidiSource({"vport", MidiSourceKind::Virtual, portName}, err);
        auto snap = e.snapshot();
        if (sid.empty() || !snap.sources[0].ok) {
            std::fprintf(stderr, "virtual port failed: %s\n", sid.empty() ? err.c_str() : snap.sources[0].status.c_str());
            return 1;
        }
        e.addMidiRoute({sid, pid, 0}, err);
        e.startManual(48000, 2, 256, err);

        // Render on a background thread, like an audio callback would.
        std::atomic<bool> stop{false};
        std::thread audio([&] {
            float l[256], r[256];
            float* out[2] = {l, r};
            while (!stop) {
                e.render(out, 2, 256);
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
        });

        UINT dev = UINT_MAX;
        for (UINT i = 0; i < midiOutGetNumDevs(); ++i) {
            MIDIOUTCAPSW caps{};
            if (midiOutGetDevCapsW(i, &caps, sizeof caps) == MMSYSERR_NOERROR && narrow(caps.szPname) == portName) dev = i;
        }
        if (dev == UINT_MAX) {
            std::fprintf(stderr, "WinMM output \"%s\" not found\n", portName.c_str());
            ++failures;
        } else {
            HMIDIOUT h = nullptr;
            if (midiOutOpen(&h, dev, 0, 0, CALLBACK_NULL) != MMSYSERR_NOERROR) {
                std::fprintf(stderr, "midiOutOpen failed\n");
                ++failures;
            } else {
                midiOutShortMsg(h, 0x643C90);  // 90 3C 64
                midiOutShortMsg(h, 0x003C90);  // 90 3C 00 (vel 0 note on stays a note on)
                midiOutShortMsg(h, 0x7F07B0);  // B0 07 7F
                BYTE sx[] = {0xF0, 0x7D, 0x01, 0x02, 0x03, 0xF7};
                MIDIHDR hdr{};
                hdr.lpData = reinterpret_cast<LPSTR>(sx);
                hdr.dwBufferLength = hdr.dwBytesRecorded = sizeof sx;
                midiOutPrepareHeader(h, &hdr, sizeof hdr);
                midiOutLongMsg(h, &hdr, sizeof hdr);
                std::this_thread::sleep_for(std::chrono::milliseconds(300));
                midiOutUnprepareHeader(h, &hdr, sizeof hdr);
                midiOutClose(h);
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        stop = true;
        audio.join();
        e.stop();
    }  // engine destroyed: virtual port removed, synth writes its log

    std::ifstream f(logPath);
    std::vector<std::string> expected = {"midi port=0: 90 3C 64", "midi port=0: 90 3C 00", "midi port=0: B0 07 7F",
                                         "sysex port=0: F0 7D 01 02 03 F7"};
    size_t k = 0;
    for (std::string l; std::getline(f, l);) {
        std::printf("  log: %s\n", l.c_str());
        if (k < expected.size() && l == expected[k]) ++k;
    }
    if (k != expected.size()) ++failures;
    std::printf(failures ? "FAILED\n" : "PASS\n");
    return failures ? 1 : 0;
}
