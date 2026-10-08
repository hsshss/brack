// vmidi_probe: exercises brack::win::MidiSrvVirtualPort against the real Windows MIDI Services.
//
//   vmidi_probe [name]            create the port, send a fixed message set to it through WinMM from
//                                 this same process, verify byte-identical delivery, print PASS/FAIL
//   vmidi_probe --listen [name]   create the port and print everything received until Ctrl+C
//   vmidi_probe --check           only run MidiSrvVirtualPort::isAvailable
//
// The port is always torn down cleanly, including on Ctrl+C / console close (never kill this process:
// an uncleanly terminated virtual device can wedge MidiSrv, microsoft/MIDI#1047).

#include "midi/win/midisrv_virtual_port.h"

#include <windows.h>
#include <mmsystem.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <mutex>
#include <string>
#include <vector>

#pragma comment(lib, "winmm.lib")

namespace {

HANDLE g_stopEvent = nullptr;  // set by Ctrl+C / close
HANDLE g_doneEvent = nullptr;  // set by main once the port is torn down

BOOL WINAPI consoleHandler(DWORD type) {
    SetEvent(g_stopEvent);
    if (type == CTRL_CLOSE_EVENT || type == CTRL_LOGOFF_EVENT || type == CTRL_SHUTDOWN_EVENT) {
        // The process is terminated as soon as this returns; give main time to tear down.
        WaitForSingleObject(g_doneEvent, 4500);
    }
    return TRUE;
}

bool stopRequested() { return WaitForSingleObject(g_stopEvent, 0) == WAIT_OBJECT_0; }

std::string hex(const uint8_t* d, size_t n) {
    std::string s;
    char b[4];
    for (size_t i = 0; i < n; ++i) {
        std::snprintf(b, sizeof(b), i ? " %02X" : "%02X", d[i]);
        s += b;
    }
    return s;
}

std::wstring widen(const std::string& s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
    return w;
}

int findWinmmOutput(const std::wstring& name) {
    UINT n = midiOutGetNumDevs();
    for (UINT i = 0; i < n; ++i) {
        MIDIOUTCAPSW caps{};
        if (midiOutGetDevCapsW(i, &caps, sizeof(caps)) == MMSYSERR_NOERROR && name == caps.szPname) return (int)i;
    }
    return -1;
}

void listWinmmOutputs() {
    UINT n = midiOutGetNumDevs();
    std::printf("  WinMM outputs (%u):\n", n);
    for (UINT i = 0; i < n; ++i) {
        MIDIOUTCAPSW caps{};
        if (midiOutGetDevCapsW(i, &caps, sizeof(caps)) == MMSYSERR_NOERROR) std::printf("    [%u] %ls\n", i, caps.szPname);
    }
}

int64_t nowNs() { return std::chrono::steady_clock::now().time_since_epoch().count(); }

struct Received {
    struct Message {
        std::vector<uint8_t> bytes;
        int64_t whenNs;      // the service's time stamp on Brack's clock, 0 for none
        int64_t callbackNs;  // when the callback ran
    };
    std::mutex m;
    std::vector<Message> messages;
};

int runListen(const std::string& name) {
    std::string error;
    auto port = brack::win::MidiSrvVirtualPort::create(
        name,
        [](const uint8_t* d, size_t n, int64_t) {
            static auto t0 = std::chrono::steady_clock::now();
            double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
            std::printf("%10.1f ms  [%zu] %s\n", ms, n, n > 48 ? (hex(d, 48) + " ...").c_str() : hex(d, n).c_str());
            std::fflush(stdout);
        },
        error);
    if (!port) {
        std::printf("create failed: %s\n", error.c_str());
        return 2;
    }
    std::printf("virtual port '%s' is up (WinMM output index %d). Press Ctrl+C to stop.\n", port->name().c_str(),
                findWinmmOutput(widen(name)));
    std::fflush(stdout);
    WaitForSingleObject(g_stopEvent, INFINITE);
    std::printf("tearing down...\n");
    port.reset();
    std::printf("done.\n");
    return 0;
}

int runTest(const std::string& name) {
    auto rx = std::make_shared<Received>();
    std::string error;
    auto t0 = std::chrono::steady_clock::now();
    auto port = brack::win::MidiSrvVirtualPort::create(
        name,
        [rx](const uint8_t* d, size_t n, int64_t whenNs) {
            std::lock_guard<std::mutex> lk(rx->m);
            rx->messages.push_back({{d, d + n}, whenNs, nowNs()});
        },
        error);
    double createMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    if (!port) {
        std::printf("create failed after %.0f ms: %s\nFAIL\n", createMs, error.c_str());
        return 2;
    }
    std::printf("created '%s' in %.0f ms\n", port->name().c_str(), createMs);

    const std::wstring wname = widen(name);
    int dev = -1;
    for (int i = 0; i < 100 && (dev = findWinmmOutput(wname)) < 0 && !stopRequested(); ++i) Sleep(50);
    if (dev < 0) {
        std::printf("WinMM output port '%s' not found\n", name.c_str());
        listWinmmOutputs();
        port.reset();
        std::printf("FAIL\n");
        return 1;
    }
    std::printf("WinMM output index %d\n", dev);

    std::vector<std::vector<uint8_t>> expected = {
        {0x90, 0x3C, 0x64}, {0x80, 0x3C, 0x00}, {0x90, 0x3C, 0x00}, {0xB0, 0x07, 0x7F}, {0xE0, 0x00, 0x40}, {0xF8},
    };
    std::vector<uint8_t> sysex = {0xF0, 0x7D};  // 0x7D: non-commercial manufacturer id
    for (uint8_t i = 1; sysex.size() < 19; ++i) sysex.push_back(i);
    sysex.push_back(0xF7);  // 20 bytes total
    expected.push_back(sysex);

    bool ok = true;
    std::vector<int64_t> sentNs;  // when each message was handed to WinMM
    HMIDIOUT out = nullptr;
    MMRESULT mr = midiOutOpen(&out, (UINT)dev, 0, 0, CALLBACK_NULL);
    if (mr != MMSYSERR_NOERROR) {
        std::printf("midiOutOpen failed: %u\n", mr);
        ok = false;
    } else {
        for (size_t i = 0; i + 1 < expected.size(); ++i) {
            const auto& m = expected[i];
            DWORD packed = 0;
            for (size_t b = 0; b < m.size(); ++b) packed |= (DWORD)m[b] << (8 * b);
            sentNs.push_back(nowNs());
            if ((mr = midiOutShortMsg(out, packed)) != MMSYSERR_NOERROR) {
                std::printf("midiOutShortMsg(%s) failed: %u\n", hex(m.data(), m.size()).c_str(), mr);
                ok = false;
            }
        }
        MIDIHDR hdr{};
        hdr.lpData = reinterpret_cast<LPSTR>(sysex.data());
        hdr.dwBufferLength = hdr.dwBytesRecorded = (DWORD)sysex.size();
        if ((mr = midiOutPrepareHeader(out, &hdr, sizeof(hdr))) == MMSYSERR_NOERROR) {
            sentNs.push_back(nowNs());
            if ((mr = midiOutLongMsg(out, &hdr, sizeof(hdr))) != MMSYSERR_NOERROR) {
                std::printf("midiOutLongMsg failed: %u\n", mr);
                ok = false;
            }
            for (int i = 0; i < 200 && !(hdr.dwFlags & MHDR_DONE); ++i) Sleep(10);
            if (!(hdr.dwFlags & MHDR_DONE)) {
                std::printf("SysEx buffer not returned by WinMM\n");
                ok = false;
                midiOutReset(out);
            }
            midiOutUnprepareHeader(out, &hdr, sizeof(hdr));
        } else {
            std::printf("midiOutPrepareHeader failed: %u\n", mr);
            ok = false;
        }

        for (int i = 0; i < 200 && !stopRequested(); ++i) {
            {
                std::lock_guard<std::mutex> lk(rx->m);
                if (rx->messages.size() >= expected.size()) break;
            }
            Sleep(10);
        }
        Sleep(100);  // catch any unexpected extra messages
        midiOutClose(out);
    }

    std::vector<Received::Message> got;
    {
        std::lock_guard<std::mutex> lk(rx->m);
        got = rx->messages;
    }
    std::printf("sent %zu, received %zu:\n", expected.size(), got.size());
    for (size_t i = 0; i < std::max(expected.size(), got.size()); ++i) {
        std::string e = i < expected.size() ? hex(expected[i].data(), expected[i].size()) : "-";
        std::string g = i < got.size() ? hex(got[i].bytes.data(), got[i].bytes.size()) : "-";
        bool same = i < expected.size() && i < got.size() && expected[i] == got[i].bytes;
        if (!same) ok = false;
        std::printf("  %s expected [%s]\n    %s got     [%s]\n", same ? "ok " : "BAD", e.c_str(), same ? "   " : "   ", g.c_str());
    }
    // The service's time stamp, on Brack's clock: after the message was handed to WinMM and
    // before the callback ran (a millisecond of slack each way). The service stamps each one.
    for (size_t i = 0; i < got.size() && i < sentNs.size(); ++i) {
        if (!got[i].whenNs) {
            std::printf("  BAD message %zu: no time stamp\n", i);
            ok = false;
            continue;
        }
        const bool plausible = got[i].whenNs >= sentNs[i] - 1000000 && got[i].whenNs <= got[i].callbackNs + 1000000;
        if (!plausible) ok = false;
        std::printf("  %s message %zu: stamped %.1f us after it was sent, %.1f us before the callback\n",
                    plausible ? "ok " : "BAD", i, (got[i].whenNs - sentNs[i]) / 1e3, (got[i].callbackNs - got[i].whenNs) / 1e3);
    }

    auto t1 = std::chrono::steady_clock::now();
    port.reset();
    double teardownMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t1).count();
    std::printf("teardown %.0f ms\n", teardownMs);
    if (teardownMs > 4000) {
        std::printf("teardown stalled: the MIDI service may be unresponsive (microsoft/MIDI#1047)\n");
        ok = false;
    }
    std::printf("%s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
    SetConsoleOutputCP(CP_UTF8);
    g_stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    g_doneEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    SetConsoleCtrlHandler(consoleHandler, TRUE);

    bool listen = false, checkOnly = false;
    std::string name = "Brack Probe";
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--listen") listen = true;
        else if (a == "--check") checkOnly = true;
        else name = a;  // note: argv is in the ANSI code page; use ASCII names here
    }

    std::string reason;
    bool available = brack::win::MidiSrvVirtualPort::isAvailable(reason);
    std::printf("isAvailable: %s%s%s\n", available ? "yes" : "no", reason.empty() ? "" : " - ", reason.c_str());
    int rc;
    if (checkOnly) rc = available ? 0 : 1;
    else if (!available) rc = 2;
    else rc = listen ? runListen(name) : runTest(name);

    SetEvent(g_doneEvent);
    return rc;
}
