// Virtual MIDI 1.0 port published through the in-box Windows MIDI Services service (MidiSrv), using its
// client COM interfaces directly (no SDK / WinRT projection, no loopMIDI-style kernel driver).
//
// Mirrors the Windows.Devices.Midi2 SDK's MidiVirtualDeviceManager / MidiVirtualDevice
// (microsoft/MIDI src/in-box/Client/WinRT/core):
//   1. Register a client session (IMidiSessionTracker).
//   2. Send a JSON "create" command to the Virtual Device App transport
//      ({8FEAAD91-70E1-4A19-997A-377720A719C1}) through IMidiTransportConfigurationManager. The service
//      creates the *device-side* UMP endpoint (MIDIU_APPDEV_<uniqueId>) and returns its interface id.
//   3. Open the device side with IMidiBidirectional. The service then creates the client-visible
//      endpoint (MIDIU_APPPUB_<uniqueId>) and runs MIDI 2.0 endpoint discovery, which we answer. Our
//      one function block (group 1, direction "input" = receives from host, MIDI 1.0) makes the
//      service create one WinMM / WinRT MIDI 1.0 *output* port, named after the block.
//   4. What other apps send to that port arrives as UMP and is translated back to MIDI 1.0 bytes.
//   5. Closing the device-side connection tears down both endpoints.
//
// All COM work runs on short-lived MTA worker threads with bounded waits, so a wedged service cannot
// hang the caller (microsoft/MIDI#1047: before the late-2026 servicing fix, MidiSrv can
// deadlock tearing down a virtual device).

#include "midi/win/midisrv_virtual_port.h"
#include "midi/win/midisrv_abi.h"

#include <windows.h>
#include <combaseapi.h>
#include <mmsystem.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cwctype>
#include <mutex>
#include <thread>
#include <vector>

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "winmm.lib")

namespace brack::win {

namespace detail {

using Microsoft::WRL::ComPtr;
namespace ms = brack::win::midisrv;

// ------------------------------------------------------------------------------------------------
// Text helpers

std::wstring utf8ToWide(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    if (n <= 0) return {};
    std::wstring w((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
    return w;
}

std::string wideToUtf8(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    if (n <= 0) return {};
    std::string s((size_t)n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}

// Truncates to at most maxBytes without splitting a UTF-8 sequence.
std::string truncateUtf8(const std::string& s, size_t maxBytes) {
    if (s.size() <= maxBytes) return s;
    size_t cut = maxBytes;
    while (cut > 0 && (static_cast<unsigned char>(s[cut]) & 0xC0) == 0x80) --cut;
    return s.substr(0, cut);
}

std::string hresultText(HRESULT hr) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "0x%08lX", static_cast<unsigned long>(hr));
    std::string out = buf;
    wchar_t* msg = nullptr;
    DWORD len = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                               nullptr, (DWORD)hr, 0, (LPWSTR)&msg, 0, nullptr);
    if (len && msg) {
        std::wstring m(msg, len);
        while (!m.empty() && (m.back() == L'\r' || m.back() == L'\n' || m.back() == L' ' || m.back() == L'.')) m.pop_back();
        out += " (" + wideToUtf8(m) + ")";
    }
    if (msg) LocalFree(msg);
    return out;
}

std::wstring guidToString(const GUID& g) {
    wchar_t buf[64] = {};
    StringFromGUID2(g, buf, 64);
    return buf;
}

// Deterministic endpoint unique id: lowercase ASCII alphanumerics of the name (max 23) followed by the
// 8-hex-digit FNV-1a hash of the full UTF-8 name. Always <= 31 characters, [a-z0-9] only, so it passes
// the service's SWD instance id validation and stays stable across runs.
std::string uniqueIdFromName(const std::string& utf8Name) {
    std::string prefix;
    for (unsigned char c : utf8Name) {
        if (prefix.size() >= 23) break;
        if (c >= 'A' && c <= 'Z') prefix.push_back(static_cast<char>(c - 'A' + 'a'));
        else if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) prefix.push_back(static_cast<char>(c));
    }
    if (prefix.empty()) prefix = "port";
    uint32_t h = 2166136261u;
    for (unsigned char c : utf8Name) {
        h ^= c;
        h *= 16777619u;
    }
    char hex[9];
    std::snprintf(hex, sizeof(hex), "%08x", h);
    return prefix + hex;
}

// ------------------------------------------------------------------------------------------------
// Minimal JSON helpers (we only build one fixed document and read two fields of the response)

std::wstring jsonEscape(const std::wstring& s) {
    std::wstring o;
    o.reserve(s.size() + 8);
    for (wchar_t c : s) {
        switch (c) {
            case L'"': o += L"\\\""; break;
            case L'\\': o += L"\\\\"; break;
            case L'\b': o += L"\\b"; break;
            case L'\f': o += L"\\f"; break;
            case L'\n': o += L"\\n"; break;
            case L'\r': o += L"\\r"; break;
            case L'\t': o += L"\\t"; break;
            default:
                if (c < 0x20) {
                    wchar_t buf[8];
                    swprintf(buf, 8, L"\\u%04x", (unsigned)c);
                    o += buf;
                } else {
                    o.push_back(c);
                }
        }
    }
    return o;
}

// Finds `"key"` followed by ':' and returns the position just after the ':' (whitespace skipped).
size_t jsonFindValue(const std::wstring& json, const std::wstring& key, size_t from = 0) {
    const std::wstring quoted = L"\"" + key + L"\"";
    size_t p = from;
    while ((p = json.find(quoted, p)) != std::wstring::npos) {
        size_t q = p + quoted.size();
        while (q < json.size() && iswspace(json[q])) ++q;
        if (q < json.size() && json[q] == L':') {
            ++q;
            while (q < json.size() && iswspace(json[q])) ++q;
            return q;
        }
        p = q;
    }
    return std::wstring::npos;
}

bool jsonGetBool(const std::wstring& json, const std::wstring& key, bool& value) {
    size_t q = jsonFindValue(json, key);
    if (q == std::wstring::npos) return false;
    if (json.compare(q, 4, L"true") == 0) { value = true; return true; }
    if (json.compare(q, 5, L"false") == 0) { value = false; return true; }
    return false;
}

bool jsonGetString(const std::wstring& json, const std::wstring& key, std::wstring& value, size_t from = 0) {
    size_t q = jsonFindValue(json, key, from);
    if (q == std::wstring::npos || q >= json.size() || json[q] != L'"') return false;
    value.clear();
    for (size_t i = q + 1; i < json.size(); ++i) {
        wchar_t c = json[i];
        if (c == L'"') return true;
        if (c != L'\\') { value.push_back(c); continue; }
        if (++i >= json.size()) return false;
        switch (json[i]) {
            case L'"': value.push_back(L'"'); break;
            case L'\\': value.push_back(L'\\'); break;
            case L'/': value.push_back(L'/'); break;
            case L'b': value.push_back(L'\b'); break;
            case L'f': value.push_back(L'\f'); break;
            case L'n': value.push_back(L'\n'); break;
            case L'r': value.push_back(L'\r'); break;
            case L't': value.push_back(L'\t'); break;
            case L'u': {
                if (i + 4 >= json.size()) return false;
                value.push_back(static_cast<wchar_t>(std::wcstoul(json.substr(i + 1, 4).c_str(), nullptr, 16)));
                i += 4;
                break;
            }
            default: return false;
        }
    }
    return false;
}

// ------------------------------------------------------------------------------------------------
// UMP helpers

using Ump128 = std::array<uint32_t, 4>;

inline unsigned umpWordCount(uint32_t word0) {
    static constexpr uint8_t kCounts[16] = {1, 1, 1, 2, 2, 4, 1, 1, 2, 2, 2, 3, 3, 4, 4, 4};
    return kCounts[word0 >> 28];
}

inline uint32_t streamWord0(unsigned form, unsigned status, uint32_t low16) {
    return 0xF0000000u | ((form & 0x3u) << 26) | ((status & 0x3FFu) << 16) | (low16 & 0xFFFFu);
}

// Splits UTF-8 text into stream text packets (Endpoint Name / Product Instance Id: 14 bytes per packet,
// 2 of them in word 0; Function Block Name: 13 bytes per packet, 1 in word 0 after the block number).
void appendStreamText(unsigned status, uint32_t word0Prefix, unsigned bytesInWord0, const std::string& text,
                      std::vector<Ump128>& out) {
    const size_t perPacket = bytesInWord0 + 12;
    const size_t packets = text.empty() ? 1 : (text.size() + perPacket - 1) / perPacket;
    size_t idx = 0;
    for (size_t p = 0; p < packets; ++p) {
        unsigned form = packets == 1 ? ms::kStreamFormComplete
                        : p == 0     ? ms::kStreamFormStart
                        : p + 1 == packets ? ms::kStreamFormEnd
                                           : ms::kStreamFormContinue;
        std::array<uint8_t, 14> bytes{};
        for (size_t i = 0; i < perPacket && idx < text.size(); ++i) bytes[i] = static_cast<uint8_t>(text[idx++]);
        Ump128 u{};
        uint32_t low16 = word0Prefix;
        size_t b = 0;
        if (bytesInWord0 == 2) {
            low16 |= (uint32_t)bytes[0] << 8 | bytes[1];
            b = 2;
        } else {
            low16 |= bytes[0];
            b = 1;
        }
        u[0] = streamWord0(form, status, low16);
        for (int w = 1; w <= 3; ++w, b += 4)
            u[w] = (uint32_t)bytes[b] << 24 | (uint32_t)bytes[b + 1] << 16 | (uint32_t)bytes[b + 2] << 8 | bytes[b + 3];
        out.push_back(u);
    }
}

struct EndpointIdentity {
    std::string endpointName;        // UTF-8, <= 98 bytes
    std::string functionBlockName;   // UTF-8, <= 91 bytes
    std::string productInstanceId;   // ASCII, <= 42 bytes
};

void appendEndpointInfo(std::vector<Ump128>& out) {
    // UMP 1.1, static function blocks, 1 block, MIDI 1.0 protocol only, no jitter-reduction timestamps.
    out.push_back({streamWord0(0, ms::kStreamEndpointInfoNotification, 0x0101), 0x80000000u | (1u << 24) | 0x0100u, 0, 0});
}

void appendStreamConfigNotification(std::vector<Ump128>& out) {
    out.push_back({streamWord0(0, ms::kStreamConfigurationNotification, ms::kStreamProtocolMidi1 << 8), 0, 0, 0});
}

void appendFunctionBlockInfo(std::vector<Ump128>& out) {
    uint32_t low16 = 0x8000u                                      // active
                     | (0u << 8)                                  // block number 0
                     | (ms::kFunctionBlockUiHintReceiver << 4)    // UI hint: receiver
                     | (ms::kFunctionBlockMidi10Unrestricted << 2)  // represents a MIDI 1.0 connection
                     | ms::kFunctionBlockDirectionInput;          // receives from the host
    uint32_t w1 = (0u << 24)    // first group (0 = group 1)
                  | (1u << 16)  // groups spanned
                  | (0u << 8)   // MIDI-CI version: none
                  | 0u;         // max SysEx8 streams
    out.push_back({streamWord0(0, ms::kStreamFunctionBlockInfoNotification, low16), w1, 0, 0});
}

// Answers one received stream message (4 words). Sets answeredFunctionBlockName when the name of our
// function block was sent (the last thing the service's discovery waits for).
void respondToStreamMessage(const uint32_t* ump, const EndpointIdentity& id, std::vector<Ump128>& out,
                            bool& answeredFunctionBlockName) {
    const unsigned status = (ump[0] >> 16) & 0x3FF;
    switch (status) {
        case ms::kStreamEndpointDiscovery: {
            const unsigned filter = ump[1] & 0xFF;
            if (filter & 0x01) appendEndpointInfo(out);
            if (filter & 0x02) out.push_back({streamWord0(0, ms::kStreamDeviceIdentityNotification, 0), 0, 0, 0});
            if (filter & 0x04) appendStreamText(ms::kStreamEndpointNameNotification, 0, 2, id.endpointName, out);
            if (filter & 0x08) appendStreamText(ms::kStreamProductInstanceIdNotification, 0, 2, id.productInstanceId, out);
            if (filter & 0x10) appendStreamConfigNotification(out);
            break;
        }
        case ms::kStreamConfigurationRequest:
            // We only speak MIDI 1.0 protocol without JR timestamps; report that whatever was requested.
            appendStreamConfigNotification(out);
            break;
        case ms::kStreamFunctionBlockDiscovery: {
            const unsigned block = (ump[0] >> 8) & 0xFF;
            const unsigned filter = ump[0] & 0xFF;
            if (block == 0xFF || block == 0) {
                if (filter & 0x01) appendFunctionBlockInfo(out);
                if (filter & 0x02) {
                    appendStreamText(ms::kStreamFunctionBlockNameNotification, 0u << 8, 1, id.functionBlockName, out);
                    answeredFunctionBlockName = true;
                }
            }
            break;
        }
        default:
            break;  // notifications sent to us (e.g. stream configuration), MIDI-CI etc.: ignore
    }
}

// Translates UMP back to MIDI 1.0 byte-stream messages.
//   types 1 (system common / realtime) and 2 (MIDI 1.0 channel voice): 1:1, nothing rewritten
//   (note-on velocity 0 stays a note-on).
//   type 3 (7-bit SysEx): reassembled per group into one F0..F7 message.
//   type 4 (MIDI 2.0 channel voice): dropped. We declare MIDI 1.0 protocol only, so the service never
//   makes it from MIDI 1.0 apps; only a UMP-native app sends it, and converting it would be lossy,
//   which this interface promises not to be.
//   everything else (utility/JR timestamps, SysEx8/mixed data, flex data, stream): ignored.
class Midi1Translator {
public:
    static constexpr size_t kMaxSysEx = 64 * 1024;

    template <class Emit>
    void feed(const uint32_t* w, unsigned wordCount, Emit&& emit) {
        const unsigned type = w[0] >> 28;
        if (type == 0x1) {
            const uint8_t status = (w[0] >> 16) & 0xFF;
            if (status < 0xF0) return;
            const uint8_t msg[3] = {status, static_cast<uint8_t>((w[0] >> 8) & 0xFF), static_cast<uint8_t>(w[0] & 0xFF)};
            size_t len = 1;
            if (status == 0xF1 || status == 0xF3) len = 2;
            else if (status == 0xF2) len = 3;
            else if (status == 0xF0 || status == 0xF7) return;  // never valid in type 1
            emit(msg, len);
        } else if (type == 0x2) {
            const uint8_t status = (w[0] >> 16) & 0xFF;
            if (status < 0x80 || status >= 0xF0) return;
            const uint8_t msg[3] = {status, static_cast<uint8_t>((w[0] >> 8) & 0xFF), static_cast<uint8_t>(w[0] & 0xFF)};
            const unsigned hi = status & 0xF0;
            emit(msg, (hi == 0xC0 || hi == 0xD0) ? 2u : 3u);
        } else if (type == 0x3 && wordCount >= 2) {
            feedSysEx7(w, emit);
        }
    }

private:
    struct SysExState {
        std::vector<uint8_t> buf;
        bool active = false;
        bool overflow = false;
    };

    template <class Emit>
    void feedSysEx7(const uint32_t* w, Emit&& emit) {
        SysExState& s = sysex_[(w[0] >> 24) & 0xF];
        const unsigned status = (w[0] >> 20) & 0xF;
        const unsigned n = (w[0] >> 16) & 0xF;
        if (n > 6) {  // malformed: abandon whatever was in progress on this group
            reset(s);
            return;
        }
        const uint8_t bytes[6] = {static_cast<uint8_t>(w[0] >> 8), static_cast<uint8_t>(w[0]),
                                  static_cast<uint8_t>(w[1] >> 24), static_cast<uint8_t>(w[1] >> 16),
                                  static_cast<uint8_t>(w[1] >> 8), static_cast<uint8_t>(w[1])};
        switch (status) {
            case 0x0: {  // complete in one packet
                reset(s);
                uint8_t msg[8];
                msg[0] = 0xF0;
                std::copy(bytes, bytes + n, msg + 1);
                msg[n + 1] = 0xF7;
                emit(msg, n + 2);
                break;
            }
            case 0x1:  // start
                reset(s);
                s.active = true;
                s.buf.push_back(0xF0);
                append(s, bytes, n);
                break;
            case 0x2:  // continue
                if (s.active) append(s, bytes, n);
                break;
            case 0x3:  // end
                if (!s.active) break;
                append(s, bytes, n);
                if (!s.overflow) {
                    s.buf.push_back(0xF7);
                    emit(s.buf.data(), s.buf.size());
                }
                reset(s);
                break;
            default:
                reset(s);
                break;
        }
    }

    static void append(SysExState& s, const uint8_t* b, unsigned n) {
        if (s.overflow) return;
        if (s.buf.size() + n + 1 > kMaxSysEx) {  // +1 for the trailing F7
            s.overflow = true;
            s.buf.clear();
            return;
        }
        s.buf.insert(s.buf.end(), b, b + n);
    }

    static void reset(SysExState& s) {
        s.buf.clear();
        s.active = false;
        s.overflow = false;
    }

    std::array<SysExState, 16> sysex_;
};

// ------------------------------------------------------------------------------------------------
// Bounded execution on an MTA worker thread.
//
// Runs `work` on a new thread with COM initialized (MTA) and waits at most timeoutMs. On timeout the
// thread is detached and keeps running; `work` must therefore only capture shared ownership.
template <class F>
bool runOnMtaThread(F&& work, DWORD timeoutMs) {
    struct Sync {
        std::mutex m;
        std::condition_variable cv;
        bool done = false;
    };
    auto sync = std::make_shared<Sync>();
    try {
        std::thread t([sync, work = std::forward<F>(work)]() mutable {
            HRESULT hrInit = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
            try {
                work();
            } catch (...) {
            }
            if (SUCCEEDED(hrInit)) CoUninitialize();
            std::lock_guard<std::mutex> lk(sync->m);
            sync->done = true;
            sync->cv.notify_all();
        });
        std::unique_lock<std::mutex> lk(sync->m);
        const bool finished = sync->cv.wait_for(lk, std::chrono::milliseconds(timeoutMs), [&] { return sync->done; });
        lk.unlock();
        if (finished) t.join();
        else t.detach();
        return finished;
    } catch (...) {
        return false;
    }
}

void debugLog(const std::string& s) {
    OutputDebugStringW(utf8ToWide("[brack midisrv] " + s + "\n").c_str());
}

// A message's position (QPC ticks, stamped by the service or the sender) on Brack's clock.
// MSVC's steady_clock is QPC in nanoseconds (__msvc_chrono.hpp: no offset, the whole seconds
// scaled, then the rest), so the same arithmetic gives the same value. 0 (no time stamp) stays 0.
int64_t positionToNs(LONGLONG position) {
    if (!position) return 0;
    static const LONGLONG freq = [] {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return f.QuadPart;
    }();
    return (position / freq) * 1000000000 + (position % freq) * 1000000000 / freq;
}

// ------------------------------------------------------------------------------------------------
// Client session shared by all ports of this process.

class Session {
public:
    // Must be called on a thread with COM initialized.
    static std::shared_ptr<Session> acquire(std::string& error) {
        static std::mutex s_mutex;
        static std::weak_ptr<Session> s_current;
        std::lock_guard<std::mutex> lk(s_mutex);
        if (auto existing = s_current.lock()) return existing;

        auto s = std::shared_ptr<Session>(new Session());
        HRESULT hr = CoIncrementMTAUsage(&s->mtaCookie_);
        if (FAILED(hr)) {
            error = "CoIncrementMTAUsage failed: " + hresultText(hr);
            return nullptr;
        }
        s->hasMtaCookie_ = true;
        hr = CoCreateInstance(ms::CLSID_Midi2MidiSrvTransport, nullptr, CLSCTX_ALL, __uuidof(ms::IMidiTransport),
                              reinterpret_cast<void**>(s->transport_.GetAddressOf()));
        if (FAILED(hr)) {
            error = "Windows MIDI Services client (Midi2.MidiSrvTransport) not available: " + hresultText(hr);
            return nullptr;
        }
        hr = s->transport_->Activate(__uuidof(ms::IMidiSessionTracker), reinterpret_cast<void**>(s->tracker_.GetAddressOf()));
        if (FAILED(hr) || !s->tracker_) {
            error = "IMidiSessionTracker activation failed: " + hresultText(hr);
            return nullptr;
        }
        hr = s->tracker_->Initialize();
        if (FAILED(hr)) {
            error = "IMidiSessionTracker::Initialize failed: " + hresultText(hr);
            s->tracker_.Reset();
            return nullptr;
        }
        s->trackerInitialized_ = true;
        if (!s->tracker_->VerifyConnectivity()) {
            error = "the MIDI service (midisrv) is not reachable";
            return nullptr;
        }
        hr = CoCreateGuid(&s->id_);
        if (FAILED(hr)) {
            error = "CoCreateGuid failed: " + hresultText(hr);
            return nullptr;
        }
        std::wstring sessionName = L"Brack";
        wchar_t exePath[MAX_PATH] = {};
        if (GetModuleFileNameW(nullptr, exePath, MAX_PATH)) {
            std::wstring p = exePath;
            size_t slash = p.find_last_of(L"\\/");
            sessionName = (slash == std::wstring::npos ? p : p.substr(slash + 1)) + L" (virtual MIDI)";
        }
        hr = s->tracker_->AddClientSession(s->id_, sessionName.c_str());
        if (FAILED(hr)) {
            error = "IMidiSessionTracker::AddClientSession failed: " + hresultText(hr);
            return nullptr;
        }
        s->sessionAdded_ = true;
        s_current = s;
        return s;
    }

    const GUID& id() const { return id_; }

    ~Session() {
        if (tracker_) {
            if (sessionAdded_) tracker_->RemoveClientSession(id_);
            if (trackerInitialized_) tracker_->Shutdown();
            tracker_.Reset();
        }
        transport_.Reset();
        if (hasMtaCookie_) CoDecrementMTAUsage(mtaCookie_);
    }

private:
    Session() = default;
    GUID id_{};
    ComPtr<ms::IMidiTransport> transport_;
    ComPtr<ms::IMidiSessionTracker> tracker_;
    CO_MTA_USAGE_COOKIE mtaCookie_{};
    bool hasMtaCookie_ = false;
    bool trackerInitialized_ = false;
    bool sessionAdded_ = false;
};

// ------------------------------------------------------------------------------------------------
// Per-port state shared between the port object, the service callback and worker threads.

struct PortCore {
    std::mutex mutex;
    std::condition_variable cv;
    bool ready = false;    // device-side connection initialized; before that, incoming data is queued
    bool closing = false;  // teardown started; no more callbacks into user code
    bool answeredFunctionBlockName = false;
    std::vector<uint32_t> pending;
    ComPtr<ms::IMidiBidirectional> bidi;
    MidiReceiveFn onMessage;
    EndpointIdentity identity;
    Midi1Translator translator;
    std::vector<Ump128> responses;

    // Called with `mutex` held.
    void processWords(const uint32_t* words, size_t count, int64_t whenNs) {
        size_t i = 0;
        while (i < count) {
            const unsigned wc = umpWordCount(words[i]);
            if (i + wc > count) break;  // truncated trailing message
            const uint32_t* u = words + i;
            i += wc;
            if ((u[0] >> 28) == 0xF) {
                responses.clear();
                respondToStreamMessage(u, identity, responses, answeredFunctionBlockName);
                for (auto& r : responses) {
                    if (bidi) bidi->SendMidiMessage(ms::MessageOptionFlags_None, r.data(), (UINT)sizeof(r), 0);
                }
                if (answeredFunctionBlockName) cv.notify_all();
                continue;
            }
            translator.feed(u, wc, [this, whenNs](const uint8_t* data, size_t size) {
                if (!onMessage) return;
                try {
                    onMessage(data, size, whenNs);
                } catch (...) {
                }
            });
        }
    }

    HRESULT onCallback(const void* message, UINT size, LONGLONG position) {
        if (!message || size < sizeof(uint32_t)) return S_OK;
        const uint32_t* words = static_cast<const uint32_t*>(message);
        const size_t count = size / sizeof(uint32_t);
        std::lock_guard<std::mutex> lk(mutex);
        if (closing) return S_OK;
        if (!ready) {
            pending.insert(pending.end(), words, words + count);
            return S_OK;
        }
        processWords(words, count, positionToNs(position));
        return S_OK;
    }
};

class CallbackSink final : public ms::IMidiCallback {
public:
    explicit CallbackSink(std::shared_ptr<PortCore> core) : core_(std::move(core)) {}

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == __uuidof(IUnknown) || riid == __uuidof(ms::IMidiCallback)) {
            *ppv = static_cast<ms::IMidiCallback*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs_; }
    ULONG STDMETHODCALLTYPE Release() override {
        ULONG r = --refs_;
        if (r == 0) delete this;
        return r;
    }
    HRESULT STDMETHODCALLTYPE Callback(ms::MessageOptionFlags, PVOID message, UINT size, LONGLONG position,
                                       LONGLONG) override {
        try {
            return core_->onCallback(message, size, position);
        } catch (...) {
            return S_OK;
        }
    }

private:
    std::atomic<ULONG> refs_{1};
    std::shared_ptr<PortCore> core_;
};

// Everything a live port owns. Torn down on an MTA worker thread.
struct PortResources {
    std::shared_ptr<Session> session;
    ComPtr<ms::IMidiTransport> transport;
    std::shared_ptr<PortCore> core;
    bool deviceConnected = false;
};

// Closes the device-side connection (which makes the service remove both endpoints and the WinMM
// port), then drops the session reference. Must run on a COM-initialized thread.
void teardownResources(PortResources& r) {
    ComPtr<ms::IMidiBidirectional> bidi;
    if (r.core) {
        std::lock_guard<std::mutex> lk(r.core->mutex);
        r.core->closing = true;
        bidi = r.core->bidi;
    }
    // Never hold the core mutex across Shutdown: it waits for in-flight callbacks.
    if (bidi && r.deviceConnected) bidi->Shutdown();
    bidi.Reset();
    if (r.core) {
        std::lock_guard<std::mutex> lk(r.core->mutex);
        r.core->bidi.Reset();  // breaks core -> bidi -> sink -> core
        r.core->onMessage = nullptr;
    }
    r.transport.Reset();
    r.session.reset();
}

constexpr DWORD kCreateTimeoutMs = 15000;
constexpr DWORD kTeardownTimeoutMs = 5000;
constexpr DWORD kTeardownTimeoutWedgedMs = 250;

// Set once a service call has timed out. With microsoft/MIDI#1047 the first teardown leaves
// the service's client-manager lock held forever and every later create or teardown blocks
// on it too, so waiting the full timeout for each would only delay exit.
std::atomic<bool> g_serviceWedged{false};
constexpr DWORD kAvailabilityTimeoutMs = 5000;
constexpr int kAppearTimeoutMs = 5000;

// Creates the virtual device and opens its device side. Runs on an MTA worker thread.
bool createResources(const std::string& name, PortResources& r, std::string& error) {
    r.session = Session::acquire(error);
    if (!r.session) return false;

    HRESULT hr = CoCreateInstance(ms::CLSID_Midi2MidiSrvTransport, nullptr, CLSCTX_ALL, __uuidof(ms::IMidiTransport),
                                  reinterpret_cast<void**>(r.transport.GetAddressOf()));
    if (FAILED(hr)) {
        error = "CoCreateInstance(Midi2MidiSrvTransport) failed: " + hresultText(hr);
        return false;
    }

    // 1. Ask the virtual device transport to create the device-side endpoint.
    std::wstring deviceId;
    {
        ComPtr<ms::IMidiTransportConfigurationManager> config;
        hr = r.transport->Activate(__uuidof(ms::IMidiTransportConfigurationManager), reinterpret_cast<void**>(config.GetAddressOf()));
        if (FAILED(hr) || !config) {
            error = "IMidiTransportConfigurationManager activation failed: " + hresultText(hr);
            return false;
        }
        hr = config->Initialize(ms::TransportId_VirtualMidi, nullptr, nullptr);
        if (FAILED(hr)) {
            error = "IMidiTransportConfigurationManager::Initialize failed: " + hresultText(hr);
            return false;
        }
        GUID association{};
        CoCreateGuid(&association);
        const std::wstring wname = utf8ToWide(name);
        std::wstring json;
        json += L"{\"endpointTransportPluginSettings\":{\"";
        json += ms::TransportIdString_VirtualMidi;
        json += L"\":{\"create\":[{";
        json += L"\"associationIdentifier\":\"" + guidToString(association) + L"\",";
        json += L"\"uniqueIdentifier\":\"" + utf8ToWide(r.core->identity.productInstanceId) + L"\",";
        json += L"\"name\":\"" + jsonEscape(wname) + L"\",";
        json += L"\"description\":\"" + jsonEscape(wname) + L" (virtual MIDI input of Brack)\",";
        json += L"\"manufacturer\":\"Brack\",";
        json += L"\"umpOnly\":false";
        json += L"}]}}}";

        LPWSTR response = nullptr;
        hr = config->UpdateConfiguration(json.c_str(), &response);
        std::wstring resp = response ? response : L"";
        if (response) CoTaskMemFree(response);

        bool success = false;
        jsonGetBool(resp, L"success", success);
        size_t devices = resp.find(L"\"createdDevices\"");
        if (devices != std::wstring::npos) jsonGetString(resp, L"id", deviceId, devices);
        if (FAILED(hr) || !success || deviceId.empty()) {
            error = "the MIDI service refused to create the virtual device (" + hresultText(hr) + ")";
            if (!resp.empty()) error += ", response: " + wideToUtf8(resp.substr(0, 400));
            if (!success) error += ". A port with the same name may already exist (in this or another process)";
            return false;
        }
    }
    std::transform(deviceId.begin(), deviceId.end(), deviceId.begin(), [](wchar_t c) { return (wchar_t)towlower(c); });

    // 2. Open the device side. The service creates the client endpoint and starts discovery while this
    //    call is in progress; those messages are queued by PortCore until `ready`.
    ComPtr<ms::IMidiBidirectional> bidi;
    hr = r.transport->Activate(__uuidof(ms::IMidiBidirectional), reinterpret_cast<void**>(bidi.GetAddressOf()));
    if (FAILED(hr) || !bidi) {
        error = "IMidiBidirectional activation failed: " + hresultText(hr);
        return false;
    }
    {
        std::lock_guard<std::mutex> lk(r.core->mutex);
        r.core->bidi = bidi;
    }
    ComPtr<ms::IMidiCallback> sink;
    sink.Attach(new CallbackSink(r.core));
    ms::TRANSPORTCREATIONPARAMS params{};
    params.MessageOptions = ms::MessageOptionFlags_None;
    params.DataFormat = ms::MidiDataFormats_UMP;
    // {b7ac3c50-6d55-4d4b-9a8c-62f2f4b1e1a7}: identifies this client in service traces only.
    params.CallingComponent = {0xb7ac3c50, 0x6d55, 0x4d4b, {0x9a, 0x8c, 0x62, 0xf2, 0xf4, 0xb1, 0xe1, 0xa7}};
    DWORD mmcssTaskId = 0;
    hr = bidi->Initialize(deviceId.c_str(), &params, &mmcssTaskId, sink.Get(), 0, r.session->id());
    if (FAILED(hr)) {
        error = "opening the virtual device failed: " + hresultText(hr) +
                ". The service keeps the half-created device until it restarts; retrying with the same name may fail";
        return false;
    }
    r.deviceConnected = true;

    // 3. Process anything that arrived during Initialize.
    {
        std::lock_guard<std::mutex> lk(r.core->mutex);
        r.core->ready = true;
        std::vector<uint32_t> pending;
        pending.swap(r.core->pending);
        if (!pending.empty()) r.core->processWords(pending.data(), pending.size(), 0);  // discovery, not data
    }
    return true;
}

bool winmmOutputExists(const std::wstring& name) {
    const UINT n = midiOutGetNumDevs();
    for (UINT i = 0; i < n; ++i) {
        MIDIOUTCAPSW caps{};
        if (midiOutGetDevCapsW(i, &caps, sizeof(caps)) == MMSYSERR_NOERROR && name == caps.szPname) return true;
    }
    return false;
}

class PortImpl final : public MidiSrvVirtualPort {
public:
    PortImpl(std::string name, PortResources resources) : name_(std::move(name)), res_(std::make_shared<PortResources>(std::move(resources))) {}

    ~PortImpl() override {
        try {
            auto res = res_;
            {
                // Stop delivering to user code right away, even if the service call below stalls.
                std::lock_guard<std::mutex> lk(res->core->mutex);
                res->core->closing = true;
            }
            const DWORD timeout = g_serviceWedged ? kTeardownTimeoutWedgedMs : kTeardownTimeoutMs;
            if (!runOnMtaThread([res] { teardownResources(*res); }, timeout)) {
                g_serviceWedged = true;
                debugLog("teardown of '" + name_ + "' timed out; the MIDI service is not responding "
                         "(known MidiSrv virtual-device teardown deadlock, microsoft/MIDI#1047). Resources leaked.");
            }
        } catch (...) {
        }
    }

    const std::string& name() const override { return name_; }

private:
    std::string name_;
    std::shared_ptr<PortResources> res_;
};

}  // namespace detail

// ================================================================================================

bool MidiSrvVirtualPort::isAvailable(std::string& reason) {
    using namespace detail;
    try {
        // 1. Virtual device transport registered and enabled.
        bool registered = false, enabled = false;
        HKEY plugins = nullptr;
        // The 64-bit view: the service is 64-bit, and a 32-bit brack would otherwise read WOW6432Node.
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows MIDI Services\\Transport Plugins", 0,
                          KEY_READ | KEY_WOW64_64KEY, &plugins) != ERROR_SUCCESS) {
            reason = "Windows MIDI Services is not installed (no transport plugin registry key)";
            return false;
        }
        for (DWORD i = 0;; ++i) {
            wchar_t sub[256];
            DWORD subLen = 256;
            if (RegEnumKeyExW(plugins, i, sub, &subLen, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) break;
            wchar_t clsid[64] = {};
            DWORD cb = sizeof(clsid);
            if (RegGetValueW(plugins, sub, L"CLSID", RRF_RT_REG_SZ, nullptr, clsid, &cb) != ERROR_SUCCESS) continue;
            if (_wcsicmp(clsid, ms::TransportIdString_VirtualMidi) != 0) continue;
            registered = true;
            DWORD en = 1;
            cb = sizeof(en);
            if (RegGetValueW(plugins, sub, L"Enabled", RRF_RT_REG_DWORD, nullptr, &en, &cb) != ERROR_SUCCESS) en = 1;
            enabled = en != 0;
            break;
        }
        RegCloseKey(plugins);
        if (!registered) {
            reason = "the Windows MIDI Services virtual device transport is not registered";
            return false;
        }
        if (!enabled) {
            reason = "the Windows MIDI Services virtual device transport is disabled";
            return false;
        }

        // 2. Service installed.
        SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
        if (!scm) {
            reason = "cannot open the service control manager";
            return false;
        }
        SC_HANDLE svc = OpenServiceW(scm, L"midisrv", SERVICE_QUERY_STATUS);
        DWORD svcErr = svc ? 0 : GetLastError();
        if (svc) CloseServiceHandle(svc);
        CloseServiceHandle(scm);
        if (!svc) {
            reason = svcErr == ERROR_SERVICE_DOES_NOT_EXIST ? "the MIDI service (midisrv) is not installed"
                                                            : "cannot query the MIDI service (midisrv)";
            return false;
        }

        // 3. Service reachable (demand-starts it), bounded so a wedged service cannot hang us.
        struct Probe {
            bool ok = false;
            std::string error;
        };
        auto probe = std::make_shared<Probe>();
        bool finished = runOnMtaThread(
            [probe] {
                ComPtr<ms::IMidiTransport> transport;
                HRESULT hr = CoCreateInstance(ms::CLSID_Midi2MidiSrvTransport, nullptr, CLSCTX_ALL, __uuidof(ms::IMidiTransport),
                                              reinterpret_cast<void**>(transport.GetAddressOf()));
                if (FAILED(hr)) {
                    probe->error = "Windows MIDI Services client (Midi2.MidiSrvTransport) not available: " + hresultText(hr);
                    // Windows registers a 32-bit client but (so far) ships only the 64-bit DLL.
                    if constexpr (sizeof(void*) == 4)
                        probe->error += " (this 32-bit Brack needs a 32-bit client; the 64-bit Brack can publish virtual ports)";
                    return;
                }
                ComPtr<ms::IMidiSessionTracker> tracker;
                hr = transport->Activate(__uuidof(ms::IMidiSessionTracker), reinterpret_cast<void**>(tracker.GetAddressOf()));
                if (FAILED(hr) || !tracker) {
                    probe->error = "IMidiSessionTracker activation failed: " + hresultText(hr);
                    return;
                }
                if (FAILED(hr = tracker->Initialize())) {
                    probe->error = "IMidiSessionTracker::Initialize failed: " + hresultText(hr);
                    return;
                }
                probe->ok = tracker->VerifyConnectivity() != FALSE;
                if (!probe->ok) probe->error = "the MIDI service (midisrv) is not reachable";
                tracker->Shutdown();
            },
            kAvailabilityTimeoutMs);
        if (!finished) {
            reason = "the MIDI service (midisrv) is not responding (it may be wedged; see microsoft/MIDI#1047, a reboot fixes it)";
            return false;
        }
        if (!probe->ok) {
            reason = probe->error;
            return false;
        }
        reason.clear();
        return true;
    } catch (...) {
        reason = "unexpected error while checking Windows MIDI Services";
        return false;
    }
}

bool MidiSrvVirtualPort::removalHangsService() {
    // With its lock change, the late-2026 fix replaces the virtual transport's OnDeviceDisconnected
    // with OnDeviceDisconnectedAlwaysTeardown, a name the DLL carries (it traces __FUNCTION__;
    // design notes, ch. 9).
    static const bool affected = [] {
        // The DLL the 64-bit MidiSrv loads: a 32-bit process reaches it only through Sysnative.
        wchar_t dir[MAX_PATH] = {};
        std::wstring sys;
        BOOL wow64 = FALSE;
        if (IsWow64Process(GetCurrentProcess(), &wow64) && wow64 && GetWindowsDirectoryW(dir, MAX_PATH))
            sys = std::wstring(dir) + L"\\Sysnative";
        else if (GetSystemDirectoryW(dir, MAX_PATH))
            sys = dir;
        else
            return false;
        HANDLE f = CreateFileW((sys + L"\\Midi2.VirtualMidiTransport.dll").c_str(), GENERIC_READ,
                               FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
        if (f == INVALID_HANDLE_VALUE) return false;  // no virtual transport: nothing to warn about
        std::string data;
        LARGE_INTEGER size{};
        if (GetFileSizeEx(f, &size) && size.QuadPart > 0 && size.QuadPart < (64ll << 20)) {
            data.resize((size_t)size.QuadPart);
            DWORD read = 0;
            if (!ReadFile(f, data.data(), (DWORD)data.size(), &read, nullptr)) read = 0;
            data.resize(read);
        }
        CloseHandle(f);
        return !data.empty() && data.find("OnDeviceDisconnectedAlwaysTeardown") == std::string::npos;
    }();
    return affected;
}

std::unique_ptr<MidiSrvVirtualPort> MidiSrvVirtualPort::create(const std::string& name, MidiReceiveFn onMessage, std::string& error) {
    using namespace detail;
    try {
        error.clear();
        const std::wstring wname = utf8ToWide(name);
        if (name.empty() || wname.empty()) {
            error = "port name must be non-empty valid UTF-8";
            return nullptr;
        }
        if (wname.size() >= MAXPNAMELEN) {
            error = "port name is longer than 31 characters (the WinMM port name limit)";
            return nullptr;
        }
        for (wchar_t c : wname) {
            if (c < 0x20) {
                error = "port name must not contain control characters";
                return nullptr;
            }
        }
        if (g_serviceWedged) {
            error = "the MIDI service stopped responding after a virtual port was removed (microsoft/MIDI#1047); "
                    "restart Windows to use virtual MIDI ports again";
            return nullptr;
        }

        auto core = std::make_shared<PortCore>();
        core->onMessage = std::move(onMessage);
        core->identity.endpointName = truncateUtf8(name, ms::kEndpointNameMaxBytes);
        core->identity.functionBlockName = truncateUtf8(name, ms::kFunctionBlockNameMaxBytes);
        core->identity.productInstanceId = uniqueIdFromName(name);

        struct Job {
            std::mutex m;
            bool finished = false;
            bool abandoned = false;
            bool ok = false;
            std::string error;
            PortResources res;
        };
        auto job = std::make_shared<Job>();
        job->res.core = core;
        const std::string nameCopy = name;
        runOnMtaThread(
            [job, nameCopy] {
                std::string err;
                bool ok = false;
                try {
                    ok = createResources(nameCopy, job->res, err);
                } catch (...) {
                    err = "unexpected error while creating the virtual device";
                }
                std::unique_lock<std::mutex> lk(job->m);
                job->finished = true;
                job->ok = ok;
                job->error = err;
                if (!ok || job->abandoned) {
                    // Failed, or the caller gave up waiting: release whatever was created.
                    PortResources res = std::move(job->res);
                    lk.unlock();
                    teardownResources(res);
                }
            },
            kCreateTimeoutMs);

        {
            std::lock_guard<std::mutex> lk(job->m);
            if (!job->finished) {
                job->abandoned = true;
                g_serviceWedged = true;
                error = "timed out talking to the MIDI service (midisrv); it may be wedged (microsoft/MIDI#1047)";
                return nullptr;
            }
            if (!job->ok) {
                error = job->error.empty() ? "failed to create the virtual device" : job->error;
                return nullptr;
            }
        }

        auto port = std::make_unique<PortImpl>(name, std::move(job->res));

        // Wait (bounded) until discovery answered our function block and the WinMM port is listed, so
        // callers can open it by name right after create() returns. Not finding it is not an error: the
        // UMP endpoint exists regardless and WinMM may simply be slower on some systems.
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(kAppearTimeoutMs);
        {
            std::unique_lock<std::mutex> lk(core->mutex);
            core->cv.wait_until(lk, deadline, [&] { return core->answeredFunctionBlockName; });
        }
        bool listed = false;
        while (!(listed = winmmOutputExists(wname)) && std::chrono::steady_clock::now() < deadline) Sleep(50);
        if (!listed) debugLog("WinMM output port '" + name + "' did not appear within 5 s");
        return port;
    } catch (...) {
        error = "unexpected error while creating the virtual MIDI port";
        return nullptr;
    }
}

}  // namespace brack::win
