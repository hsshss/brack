using System.Text.Json;
using System.Text.Json.Serialization;

namespace Brack;

/// <summary>The engine as brack_get_status reports it.</summary>
public sealed record EngineStatus(
    EngineMode Mode,
    string Device,
    bool DeviceStalled,
    uint OutputSampleRate,
    uint ProcessSampleRate,
    uint OutputChannels,
    uint PeriodFrames,
    uint BlockSize,
    bool Resampling,
    ResamplerQuality ResamplerQuality,
    double ResamplerLatencyMs,
    float MasterGain,
    float CpuLoad,
    IReadOnlyList<float> OutputPeaks,
    IReadOnlyList<PluginStatus> Plugins,
    IReadOnlyList<MidiSourceStatus> MidiSources,
    IReadOnlyList<MidiRoute> MidiRoutes,
    IReadOnlyList<AudioRoute> AudioRoutes);

public sealed record PluginStatus(
    string Id,
    string Name,
    PluginFormat Format,
    string PluginId,
    string Path,
    string Status,
    bool Loaded,
    bool Active,
    bool Failed,
    bool Crashed,
    string Architecture,
    bool SeparateProcess,
    bool HasGui,
    bool GuiOpen,
    uint Latency,
    IReadOnlyList<NotePortInfo> NotePorts,
    IReadOnlyList<AudioPortInfo> AudioOutputs);

/// <summary>A note input port and the dialects it accepts.</summary>
public sealed record NotePortInfo(string Name, bool Midi, bool Clap);

public sealed record AudioPortInfo(string Name, uint Channels, bool Main);

public sealed record MidiSourceStatus(string Id, MidiSourceKind Kind, string Name, bool Ok, string Status, ulong Messages,
                                      ulong Dropped);

public sealed record MidiRoute(string Source, string Plugin, uint NotePort);

public sealed record AudioRoute(string Plugin, uint Port, uint Channel, uint Output, float Gain);

/// <summary>A plugin a scan found.</summary>
public sealed record PluginDescription(
    PluginFormat Format,
    string Path,
    string Id,
    string Name,
    string Vendor,
    string Version,
    bool Instrument,
    IReadOnlyList<string> Features,
    string Architecture);

public sealed record AudioDeviceInfo(
    string Name,
    [property: JsonPropertyName("default")] bool IsDefault,
    IReadOnlyList<uint> SampleRates,
    uint MaxChannels);

// Source-generated (trimming and AOT safe). The library writes camelCase names and enum values.
[JsonSourceGenerationOptions(PropertyNamingPolicy = JsonKnownNamingPolicy.CamelCase,
                             UseStringEnumConverter = true)]
[JsonSerializable(typeof(EngineStatus))]
[JsonSerializable(typeof(PluginDescription[]))]
[JsonSerializable(typeof(AudioDeviceInfo[]))]
[JsonSerializable(typeof(string[]))]
internal sealed partial class BrackJson : JsonSerializerContext
{
    internal static T Parse<T>(string json, System.Text.Json.Serialization.Metadata.JsonTypeInfo<T> type) =>
        JsonSerializer.Deserialize(json, type) ?? throw new BrackException(BrackErrorCode.Failed, "empty JSON from brack.dll");
}
