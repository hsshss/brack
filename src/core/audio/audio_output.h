#pragma once
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace brack {

struct AudioDeviceInfo {
    std::string name;
    bool isDefault = false;
    std::vector<uint32_t> sampleRates;  // native rates reported by the backend (may be empty)
    uint32_t maxChannels = 0;
};

struct AudioOutputConfig {
    std::string deviceName;     // empty = system default
    uint32_t sampleRate = 0;    // 0 = device native rate. Honoured in exclusive mode only:
                                // in shared mode the device always runs at its mix rate and
                                // brack's own resampler converts to it (never the OS one).
    uint32_t channels = 2;
    uint32_t bufferFrames = 256;
    bool exclusive = false;  // WASAPI's exclusive mode; elsewhere ignored (always the sound server's)
};

// Planar float output. out[c] points to `frames` samples.
using AudioRenderFn = std::function<void(float* const* out, uint32_t channels, uint32_t frames)>;

std::vector<AudioDeviceInfo> listAudioOutputDevices();

// Identifies the system's current default output device (the WASAPI endpoint id
// on Windows). Empty when there is no output device at all.
std::string defaultAudioOutputKey();

class AudioOutput {
public:
    AudioOutput();
    ~AudioOutput();
    bool open(const AudioOutputConfig& cfg, AudioRenderFn render, std::string& error);
    void close();
    bool start(std::string& error);
    void stop();
    bool isOpen() const;
    bool isRunning() const;

    uint32_t sampleRate() const;
    uint32_t channels() const;
    uint32_t periodFrames() const;
    const std::string& deviceName() const;
    std::string backendName() const;  // miniaudio's name for the backend the device is open on
    // Same form as defaultAudioOutputKey(), for the device that is open.
    const std::string& deviceKey() const;

    // Whether the sound server may move the stream along with the system default itself: the default
    // opened through PulseAudio's interface (PipeWire's too), not a device named. PipeWire, and
    // PulseAudio from 15, do.
    bool movesWithDefault() const;
    // Whether the sound server moved the stream to another device since the last call.
    bool takeMoved();
    // The device it was moved to (the default's key, which on Linux is its name).
    void movedTo(const std::string& key);

    struct Impl;

private:
    std::unique_ptr<Impl> impl_;
};

}  // namespace brack
