namespace Brack;

public enum BrackLogLevel
{
    Debug = 0,
    Info = 1,
    Warning = 2,
    Error = 3,
}

public enum ResamplerQuality
{
    Standard = 0,
    High = 1,
    Ultra = 2,
}

public enum MidiSourceKind
{
    /// <summary>An existing MIDI input port, opened by name.</summary>
    Hardware = 0,
    /// <summary>A virtual port published by Brack, visible to other applications.</summary>
    Virtual = 1,
    /// <summary>Fed only through <see cref="BrackEngine.SendMidiToSource"/>.</summary>
    Api = 2,
}

/// <summary>What became of a message given to <see cref="BrackEngine.SendMidi"/> and the like.</summary>
public enum MidiSendResult
{
    Sent = 0,
    /// <summary>Not one complete MIDI message, a note port past 65535, or a time not after 0.</summary>
    InvalidArgument = -2,
    /// <summary>No plugin or MIDI source with that id.</summary>
    NotFound = -3,
    /// <summary>The plugin's or source's queue is full; the message was dropped.</summary>
    QueueFull = -5,
}

public enum PluginFormat
{
    Clap,
    Vst2,
    Vst3,
}

public enum EngineMode
{
    Stopped,
    Device,
    Manual,
}

public enum BrackEventType
{
    /// <summary><see cref="BrackEngine.ChangeCount"/> went up. At most one is queued.</summary>
    Changed = 1,
    /// <summary>Id: the plugin. Message: where it crashed.</summary>
    PluginCrashed = 2,
    /// <summary>Id: the plugin whose editor closed.</summary>
    EditorClosed = 3,
    /// <summary>Id: the plugin. Width and height: its editor's size, in physical pixels.</summary>
    EditorResized = 4,
    /// <summary>The output device stopped calling back.</summary>
    DeviceStalled = 5,
    /// <summary>The output device is back.</summary>
    DeviceResumed = 6,
    /// <summary>Id: the MIDI source whose port went away. Message: why.</summary>
    MidiSourceLost = 7,
    /// <summary>Id: the MIDI source, open again.</summary>
    MidiSourceReconnected = 8,
}

/// <summary>Something that happened in the engine without a call (see <see cref="BrackEngine.TryPollEvent"/>).</summary>
public readonly record struct BrackEvent(BrackEventType Type, string Id, string Message, uint Width, uint Height);

/// <summary>The engine's configuration (brack_config).</summary>
public sealed record BrackConfig
{
    /// <summary>Output device name; null or empty for the system default.</summary>
    public string? AudioDevice { get; init; }
    /// <summary>Exclusive mode only; 0 for the device's native rate.</summary>
    public uint DeviceSampleRate { get; init; }
    public uint Channels { get; init; } = 2;
    /// <summary>Device period in frames.</summary>
    public uint BufferFrames { get; init; } = 256;
    /// <summary>WASAPI exclusive mode (Windows; ignored elsewhere).</summary>
    public bool Exclusive { get; init; }
    /// <summary>The rate plugins run at; 0 for the output's (no conversion).</summary>
    public uint ProcessSampleRate { get; init; }
    /// <summary>Most frames per plugin process call.</summary>
    public uint BlockSize { get; init; } = 256;
    public ResamplerQuality ResamplerQuality { get; init; } = ResamplerQuality.High;
    /// <summary>Plugins run inside this process rather than in a plugin host process each (brack-host-*.exe beside
    /// brack.dll). Faster to load, but a plugin that crashes outside a call from Brack, or hangs, takes the process
    /// down. Plugins built for another architecture run in a plugin host process anyway.</summary>
    public bool PluginsInProcess { get; init; }
}
