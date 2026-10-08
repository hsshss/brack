// Hardware MIDI input via WinMM (also covers ports published by other apps).
#include <windows.h>
#include <mmsystem.h>

#include <array>
#include <atomic>
#include <mutex>

#include "midi/midi_input.h"
#include "util/common.h"

namespace brack {

namespace {

// Everything WinMM may still touch after midiInClose() fails: the callback context
// and the sysex buffers. Heap-allocated so it can be leaked in that case.
struct Connection {
    static constexpr size_t kBufferSize = 4096;
    static constexpr size_t kBufferCount = 8;
    static constexpr size_t kMaxSysex = 1 << 20;

    MidiReceiveFn fn;
    HMIDIIN handle = nullptr;
    // Guards `fn` against the teardown: once `closing` is set under it, no delivery is
    // under way and none starts, whatever the driver does afterwards.
    std::mutex deliverMutex;
    bool closing = false;
    std::atomic<bool> lost{false};
    std::array<MIDIHDR, kBufferCount> headers{};
    std::array<std::array<uint8_t, kBufferSize>, kBufferCount> buffers{};
    std::vector<uint8_t> sysex;  // driver thread
    bool sysexTooLong = false;   // driver thread

    static void CALLBACK callback(HMIDIIN, UINT msg, DWORD_PTR instance, DWORD_PTR p1, DWORD_PTR) {
        auto* self = reinterpret_cast<Connection*>(instance);
        switch (msg) {
            case MIM_DATA: {
                uint8_t bytes[3] = {uint8_t(p1 & 0xFF), uint8_t((p1 >> 8) & 0xFF), uint8_t((p1 >> 16) & 0xFF)};
                if (size_t len = midiShortMessageLength(bytes[0])) self->deliver(bytes, len);
                break;
            }
            case MIM_LONGDATA:
                self->onLongData(reinterpret_cast<MIDIHDR*>(p1), true);
                break;
            case MIM_LONGERROR:  // an incomplete or broken SysEx: drop it, keep the buffer
                self->onLongData(reinterpret_cast<MIDIHDR*>(p1), false);
                break;
            case MIM_CLOSE:  // the driver closed us: the device went away, or the service dropped us
                self->lost.store(true);
                break;
            default:
                break;
        }
    }

    void deliver(const uint8_t* data, size_t size) {
        std::lock_guard lock(deliverMutex);
        if (!closing) fn(data, size, 0);
    }

    void onLongData(MIDIHDR* h, bool valid) {
        if (!valid) {
            sysex.clear();
            sysexTooLong = false;
        } else {
            const auto* data = reinterpret_cast<const uint8_t*>(h->lpData);
            for (DWORD i = 0; i < h->dwBytesRecorded; ++i) {
                uint8_t b = data[i];
                if (b == 0xF0) sysex.clear(), sysexTooLong = false;
                if (sysex.size() < kMaxSysex) sysex.push_back(b);
                else sysexTooLong = true;  // dropped whole, as on the other systems
                if (b == 0xF7) {
                    if (!sysexTooLong && !sysex.empty() && sysex[0] == 0xF0) deliver(sysex.data(), sysex.size());
                    sysex.clear();
                    sysexTooLong = false;
                }
            }
        }
        // Recycling the buffer from the callback is what every WinMM host does in practice.
        // Every returned buffer goes back, or after enough errors none would be left. Under the
        // lock, so none goes back once closing has begun (after midiInReset, close would fail).
        std::lock_guard lock(deliverMutex);
        if (!closing) midiInAddBuffer(handle, h, sizeof(*h));
    }
};

class WinMmInput final : public MidiInputPort {
public:
    explicit WinMmInput(MidiReceiveFn fn) : c_(std::make_unique<Connection>()) { c_->fn = std::move(fn); }

    bool open(UINT id, std::string& error) {
        MMRESULT r = midiInOpen(&c_->handle, id, (DWORD_PTR)&Connection::callback, (DWORD_PTR)c_.get(), CALLBACK_FUNCTION);
        if (r != MMSYSERR_NOERROR) {
            error = "midiInOpen failed (" + std::to_string(r) + ")";
            c_->handle = nullptr;
            return false;
        }
        for (size_t i = 0; i < Connection::kBufferCount && r == MMSYSERR_NOERROR; ++i) {
            auto& h = c_->headers[i];
            h = {};
            h.lpData = reinterpret_cast<LPSTR>(c_->buffers[i].data());
            h.dwBufferLength = Connection::kBufferSize;
            r = midiInPrepareHeader(c_->handle, &h, sizeof(h));
            if (r == MMSYSERR_NOERROR) r = midiInAddBuffer(c_->handle, &h, sizeof(h));
        }
        if (r == MMSYSERR_NOERROR) r = midiInStart(c_->handle);
        if (r != MMSYSERR_NOERROR) {
            error = "cannot start the MIDI input (" + std::to_string(r) + ")";
            close();
            return false;
        }
        return true;
    }

    ~WinMmInput() override {
        if (c_ && c_->handle) close();
    }

    bool lost() const override { return c_->lost.load(); }

private:
    void close() {
        {
            std::lock_guard lock(c_->deliverMutex);
            c_->closing = true;
        }
        midiInStop(c_->handle);
        midiInReset(c_->handle);
        for (auto& h : c_->headers) midiInUnprepareHeader(c_->handle, &h, sizeof(h));
        if (MMRESULT r = midiInClose(c_->handle); r != MMSYSERR_NOERROR) {
            // A device that went away can refuse to give its buffers back (microsoft/MIDI#1157):
            // the driver may still write to them, so they must outlive us.
            logWarn("midiInClose failed (" + std::to_string(r) + "); leaving its buffers allocated");
            c_.release();
            return;
        }
        c_->handle = nullptr;
    }

    std::unique_ptr<Connection> c_;
};

}  // namespace

std::vector<std::string> listHardwareMidiInputs() {
    std::vector<std::string> names;
    UINT n = midiInGetNumDevs();
    for (UINT i = 0; i < n; ++i) {
        MIDIINCAPSW caps{};
        if (midiInGetDevCapsW(i, &caps, sizeof(caps)) == MMSYSERR_NOERROR) names.push_back(narrow(caps.szPname));
    }
    return names;
}

std::unique_ptr<MidiInputPort> openHardwareMidiInput(const std::string& name, MidiReceiveFn fn, std::string& error) {
    UINT n = midiInGetNumDevs();
    for (UINT i = 0; i < n; ++i) {
        MIDIINCAPSW caps{};
        if (midiInGetDevCapsW(i, &caps, sizeof(caps)) != MMSYSERR_NOERROR) continue;
        if (narrow(caps.szPname) != name) continue;
        auto port = std::make_unique<WinMmInput>(std::move(fn));
        if (!port->open(i, error)) return nullptr;
        return port;
    }
    error = "MIDI input not found: " + name;
    return nullptr;
}

}  // namespace brack
