using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using Brack.Native;

namespace Brack;

/// <summary>
/// A Brack engine: plugins, MIDI sources, routing, audio output. Every member is thread safe; changes run on
/// the engine's own thread, which also hosts plugin editors, so callers need no message loop. On macOS that
/// thread is the process's main thread instead, whose run loop the application keeps running (a console
/// program does its work on another thread and calls CoreFoundation's CFRunLoopRun() on the main one, as
/// Brack.Tests does); calls from other threads wait for it.
/// Failures throw <see cref="BrackException"/>, except in the members meant for audio and MIDI threads
/// (<see cref="Render(float[][], int)"/>, the SendMidi family), which do not allocate and report a failure as a
/// <see cref="MidiSendResult"/>; Render throws only for arrays shorter than the frames asked for.
/// JSON the C API returns comes back as typed records (<see cref="GetStatus"/>).
/// </summary>
/// <example>
/// <code>
/// using var engine = new BrackEngine();
/// string id = engine.AddPlugin(@"C:\path\Synth.clap");  // routed to outputs 1/2 (routeAudio: false: not)
/// engine.SetConfig(new BrackConfig { ProcessSampleRate = 96000 });
/// engine.Start();                                        // or StartManual() and Render()
/// engine.SendMidi(id, 0, [0x90, 60, 100]);               // from any thread
/// engine.EventsQueued += (_, _) => { /* on Brack's thread: take them with TryPollEvent */ };
/// </code>
/// </example>
public sealed unsafe class BrackEngine : IDisposable
{
    private readonly BrackEngineHandle _handle;
    private readonly Lock _notifyLock = new();
    private EventHandler? _eventsQueued;
    private GCHandle _self;  // weak: given to brack.dll for the event notification
    private float[][]? _scratch;  // RenderInterleaved

    public BrackEngine()
    {
        BrackLibrary.CheckApiVersion();
        _handle = NativeMethods.brack_engine_create();
        if (_handle.IsInvalid) throw Interop.LastError((int)BrackErrorCode.Failed);
    }

    /// <summary>Stops the engine and releases every plugin and port. Virtual MIDI ports go too (see
    /// <see cref="BrackLibrary.VirtualMidiRemovalHangs"/>).</summary>
    public void Dispose()
    {
        lock (_notifyLock)
        {
            if (_handle.IsClosed) return;
            if (_eventsQueued != null) NativeMethods.brack_set_event_notify(_handle, null, 0);
            _handle.Dispose();
            if (_self.IsAllocated) _self.Free();
        }
    }

    // ---- configuration and transport ----

    public void SetConfig(BrackConfig config)
    {
        ArgumentNullException.ThrowIfNull(config);
        nint device = Marshal.StringToCoTaskMemUTF8(config.AudioDevice);
        try
        {
            var c = new NativeMethods.brack_config
            {
                struct_size = (uint)sizeof(NativeMethods.brack_config),
                audio_device = (byte*)device,
                device_sample_rate = config.DeviceSampleRate,
                channels = config.Channels,
                buffer_frames = config.BufferFrames,
                exclusive = config.Exclusive ? 1 : 0,
                process_sample_rate = config.ProcessSampleRate,
                block_size = config.BlockSize,
                resampler_quality = (int)config.ResamplerQuality,
                plugins_in_process = config.PluginsInProcess ? 1 : 0,
                load_plugins_serially = config.LoadPluginsSerially ? 1 : 0,
            };
            Interop.Check(NativeMethods.brack_set_config(_handle, &c));
        }
        finally
        {
            Marshal.ZeroFreeCoTaskMemUTF8(device);
        }
    }

    /// <summary>The configuration last set (what the device ended up with is in <see cref="GetStatus"/>).</summary>
    public BrackConfig GetConfig()
    {
        var c = new NativeMethods.brack_config { struct_size = (uint)sizeof(NativeMethods.brack_config) };
        Interop.Check(NativeMethods.brack_get_config(_handle, &c));
        return new BrackConfig
        {
            AudioDevice = Interop.Utf8(c.audio_device),
            DeviceSampleRate = c.device_sample_rate,
            Channels = c.channels,
            BufferFrames = c.buffer_frames,
            Exclusive = c.exclusive != 0,
            ProcessSampleRate = c.process_sample_rate,
            BlockSize = c.block_size,
            ResamplerQuality = (ResamplerQuality)c.resampler_quality,
            PluginsInProcess = c.plugins_in_process != 0,
            LoadPluginsSerially = c.load_plugins_serially != 0,
        };
    }

    /// <summary>Opens the configured audio device and starts rendering.</summary>
    public void Start() => Interop.Check(NativeMethods.brack_start(_handle));

    /// <summary>Starts without a device: the application calls <see cref="Render(float[][], int)"/> from its audio
    /// thread, at most <paramref name="maxFrames"/> frames at a time. On macOS, with plugins in this process, that
    /// thread must be in an audio workgroup, or heavy plugins may miss their deadlines (brack.h, brack_render).</summary>
    public void StartManual(int sampleRate, int channels, int maxFrames)
    {
        ArgumentOutOfRangeException.ThrowIfNegative(sampleRate);
        ArgumentOutOfRangeException.ThrowIfNegative(channels);
        ArgumentOutOfRangeException.ThrowIfNegative(maxFrames);
        Interop.Check(NativeMethods.brack_start_manual(_handle, (uint)sampleRate, (uint)channels, (uint)maxFrames));
        _scratch = new float[channels][];
        for (int c = 0; c < channels; ++c) _scratch[c] = new float[maxFrames];
    }

    public void Stop() => NativeMethods.brack_stop(_handle);

    /// <summary>Manual mode: renders <paramref name="frames"/> frames into one array per channel, each at least that
    /// long. Real-time safe: no allocation.</summary>
    public void Render(float[][] outputs, int frames)
    {
        int channels = outputs.Length;
        ArgumentOutOfRangeException.ThrowIfNegative(frames);
        for (int c = 0; c < channels; ++c)
            if (outputs[c].Length < frames) throw new ArgumentException("an array is shorter than frames", nameof(outputs));
        float** pointers = stackalloc float*[channels];
        GCHandle* pins = stackalloc GCHandle[channels];
        for (int c = 0; c < channels; ++c)
        {
            pins[c] = GCHandle.Alloc(outputs[c], GCHandleType.Pinned);
            pointers[c] = (float*)pins[c].AddrOfPinnedObject();
        }
        try
        {
            NativeMethods.brack_render(_handle, pointers, (uint)channels, (uint)frames);
        }
        finally
        {
            for (int c = 0; c < channels; ++c) pins[c].Free();
        }
    }

    /// <summary>Manual mode, for interleaved buffers (frames × channels, as most audio APIs want). Real-time safe
    /// after <see cref="StartManual"/>.</summary>
    public void RenderInterleaved(Span<float> interleaved, int channels)
    {
        float[][] scratch = _scratch ?? throw new InvalidOperationException("not started in manual mode");
        if (channels != scratch.Length) throw new ArgumentException("channel count differs from StartManual's", nameof(channels));
        int frames = interleaved.Length / channels, done = 0;
        while (done < frames)
        {
            int n = Math.Min(frames - done, scratch[0].Length);
            Render(scratch, n);
            for (int i = 0; i < n; ++i)
                for (int c = 0; c < channels; ++c) interleaved[(done + i) * channels + c] = scratch[c][i];
            done += n;
        }
    }

    /// <summary>Manual mode, for native buffers.</summary>
    public void Render(float** outputs, int channels, int frames) =>
        NativeMethods.brack_render(_handle, outputs, (uint)channels, (uint)frames);

    /// <summary>Output frames rendered since the engine started: the clock <see cref="SendMidiAt"/> times refer
    /// to. In manual mode, the next <see cref="Render(float[][], int)"/> starts at the value read before it.</summary>
    public ulong RenderPosition => NativeMethods.brack_get_render_position(_handle);

    // ---- plugins ----

    /// <summary>Adds a plugin and returns its id.</summary>
    /// <param name="path">A .clap file, a .vst3 bundle or file, or a VST2 .dll.</param>
    /// <param name="pluginId">Which plugin in the file (CLAP id, VST3 class id, VST2 unique id); null for the first.</param>
    /// <param name="id">The instance id; null to make one from the plugin's name.</param>
    /// <param name="routeAudio">Whether to route its main output to outputs 1 and 2.</param>
    public string AddPlugin(string path, string? pluginId = null, string? id = null, bool routeAudio = true)
    {
        ArgumentException.ThrowIfNullOrEmpty(path);
        uint flags = routeAudio ? 0u : 1u;  // BRACK_ADD_PLUGIN_NO_AUDIO_ROUTES
        if (!string.IsNullOrEmpty(id))
        {
            Interop.Check(NativeMethods.brack_add_plugin_ex(_handle, id, path, pluginId, flags, null, 0));
            return id;
        }
        byte* idOut = stackalloc byte[NativeMethods.BRACK_ID_MAX];
        Interop.Check(NativeMethods.brack_add_plugin_ex(_handle, null, path, pluginId, flags, idOut, NativeMethods.BRACK_ID_MAX));
        return Interop.Utf8(idOut) ?? "";
    }

    public void RemovePlugin(string pluginId) => Interop.Check(NativeMethods.brack_remove_plugin(_handle, pluginId));

    /// <summary>Loads the plugin again as a new instance, with the state a session would save for it, keeping its id,
    /// name and routes: for a plugin that crashed (<see cref="BrackEventType.PluginCrashed"/>) or failed to load.</summary>
    public void ReloadPlugin(string pluginId) => Interop.Check(NativeMethods.brack_reload_plugin(_handle, pluginId));

    /// <summary>Moves a plugin to <paramref name="index"/> in the list (clamped): the order status and sessions show.</summary>
    public void MovePlugin(string pluginId, int index) =>
        Interop.Check(NativeMethods.brack_move_plugin(_handle, pluginId, (uint)Math.Max(index, 0)));

    /// <summary>Opens or closes the plugin's editor in a window of its own.</summary>
    public void ShowPluginGui(string pluginId, bool visible) =>
        Interop.Check(NativeMethods.brack_show_plugin_gui(_handle, pluginId, visible ? 1 : 0));

    /// <summary>
    /// Opens the plugin's editor in <paramref name="parentWindow"/> (an HWND), a container window of the
    /// application's, at its top left. Size the container to <see cref="BrackEventType.EditorResized"/>. Close the
    /// editor with <c>ShowPluginGui(id, false)</c> before destroying the window.
    /// </summary>
    public void ShowPluginGuiIn(string pluginId, nint parentWindow) =>
        Interop.Check(NativeMethods.brack_show_plugin_gui_in(_handle, pluginId, parentWindow));

    /// <summary>Changes the display name only; null or empty for the plugin's own name.</summary>
    public void SetPluginName(string pluginId, string? name) =>
        Interop.Check(NativeMethods.brack_set_plugin_name(_handle, pluginId, name));

    /// <summary>The plugin's state as a session saves it, in the plugin's own format.</summary>
    public byte[] GetPluginState(string pluginId) =>
        Interop.ReadBytes((buf, size, needed) => NativeMethods.brack_get_plugin_state(_handle, pluginId, buf, size, needed));

    public void SetPluginState(string pluginId, ReadOnlySpan<byte> state)
    {
        fixed (byte* p = state) Interop.Check(NativeMethods.brack_set_plugin_state(_handle, pluginId, p, (nuint)state.Length));
    }

    // ---- MIDI sources ----

    /// <summary>Adds a MIDI source and returns its id.</summary>
    /// <param name="kind">Where its MIDI comes from.</param>
    /// <param name="name">The port to open (hardware) or publish (virtual); not used for API sources.</param>
    /// <param name="id">The source id; null to make one from the name.</param>
    public string AddMidiSource(MidiSourceKind kind, string? name = null, string? id = null)
    {
        if (!string.IsNullOrEmpty(id))
        {
            Interop.Check(NativeMethods.brack_add_midi_source(_handle, id, (int)kind, name, null, 0));
            return id;
        }
        byte* idOut = stackalloc byte[NativeMethods.BRACK_ID_MAX];
        Interop.Check(NativeMethods.brack_add_midi_source(_handle, null, (int)kind, name, idOut, NativeMethods.BRACK_ID_MAX));
        return Interop.Utf8(idOut) ?? "";
    }

    public void RemoveMidiSource(string sourceId) => Interop.Check(NativeMethods.brack_remove_midi_source(_handle, sourceId));

    public void MoveMidiSource(string sourceId, int index) =>
        Interop.Check(NativeMethods.brack_move_midi_source(_handle, sourceId, (uint)Math.Max(index, 0)));

    /// <summary>Opens the source's port again (hardware ports also reopen by themselves once listed again).</summary>
    public void ReopenMidiSource(string sourceId) => Interop.Check(NativeMethods.brack_reopen_midi_source(_handle, sourceId));

    // ---- routing ----

    public void ConnectMidi(string sourceId, string pluginId, int notePort = 0)
    {
        ArgumentOutOfRangeException.ThrowIfNegative(notePort);
        Interop.Check(NativeMethods.brack_connect_midi(_handle, sourceId, pluginId, (uint)notePort));
    }

    /// <summary>False if there was no such route.</summary>
    public bool DisconnectMidi(string sourceId, string pluginId, int notePort = 0)
    {
        ArgumentOutOfRangeException.ThrowIfNegative(notePort);
        return CheckFound(NativeMethods.brack_disconnect_midi(_handle, sourceId, pluginId, (uint)notePort));
    }

    /// <summary>Routes channel <paramref name="channel"/> of the plugin's output port <paramref name="port"/> to engine
    /// output <paramref name="output"/>; for a route that exists, sets its gain.</summary>
    public void ConnectAudio(string pluginId, int port, int channel, int output, float gain = 1.0f)
    {
        ThrowIfAnyNegative(port, channel, output);
        Interop.Check(NativeMethods.brack_connect_audio(_handle, pluginId, (uint)port, (uint)channel, (uint)output, gain));
    }

    /// <summary>False if there was no such route.</summary>
    public bool DisconnectAudio(string pluginId, int port, int channel, int output)
    {
        ThrowIfAnyNegative(port, channel, output);
        return CheckFound(NativeMethods.brack_disconnect_audio(_handle, pluginId, (uint)port, (uint)channel, (uint)output));
    }

    private static void ThrowIfAnyNegative(int port, int channel, int output)
    {
        ArgumentOutOfRangeException.ThrowIfNegative(port);
        ArgumentOutOfRangeException.ThrowIfNegative(channel);
        ArgumentOutOfRangeException.ThrowIfNegative(output);
    }

    public void ClearAudioRoutes(string pluginId) => Interop.Check(NativeMethods.brack_clear_audio_routes(_handle, pluginId));

    /// <summary>Linear gain on every output after the sample rate conversion (1 = unity). Ramped; never blocks.</summary>
    public float MasterGain
    {
        get => NativeMethods.brack_get_master_gain(_handle);
        set => Interop.Check(NativeMethods.brack_set_master_gain(_handle, value));
    }

    private static bool CheckFound(int result)
    {
        if (result == (int)BrackErrorCode.NotFound) return false;
        Interop.Check(result);
        return true;
    }

    // ---- MIDI (any thread, including real-time ones: no allocation, no exception) ----

    /// <summary>Sends one complete MIDI message (1 to 3 bytes, or SysEx F0 .. F7) straight to a plugin, unchanged.
    /// With an audio device it lands a device period after the call, where in the period it came; in manual mode,
    /// at the next render.</summary>
    public MidiSendResult SendMidi(string pluginId, int notePort, ReadOnlySpan<byte> message)
    {
        fixed (byte* p = message)
            return (MidiSendResult)NativeMethods.brack_send_midi(_handle, pluginId, (uint)notePort, p, (uint)message.Length);
    }

    /// <summary>As <see cref="SendMidi"/>, delivered at render position <paramref name="time"/>
    /// (<see cref="RenderPosition"/>), frame accurate without sample rate conversion.</summary>
    public MidiSendResult SendMidiAt(string pluginId, int notePort, ulong time, ReadOnlySpan<byte> message)
    {
        fixed (byte* p = message)
            return (MidiSendResult)NativeMethods.brack_send_midi_at(_handle, pluginId, (uint)notePort, time, p,
                                                                    (uint)message.Length);
    }

    /// <summary>Feeds a MIDI source (normally an <see cref="MidiSourceKind.Api"/> one); routing decides who gets it.</summary>
    public MidiSendResult SendMidiToSource(string sourceId, ReadOnlySpan<byte> message)
    {
        fixed (byte* p = message)
            return (MidiSendResult)NativeMethods.brack_send_midi_to_source(_handle, sourceId, p, (uint)message.Length);
    }

    public MidiSendResult SendMidiToSourceAt(string sourceId, ulong time, ReadOnlySpan<byte> message)
    {
        fixed (byte* p = message)
            return (MidiSendResult)NativeMethods.brack_send_midi_to_source_at(_handle, sourceId, time, p, (uint)message.Length);
    }

    /// <summary>As <see cref="SendMidi"/>, for the moment <paramref name="time"/> on Brack's clock
    /// (<see cref="BrackLibrary.NowNs"/>): it lands where a message sent then would, so it can be sent ahead. A
    /// moment past means at once; in manual mode, as <see cref="SendMidi"/>.</summary>
    public MidiSendResult SendMidiAtTime(string pluginId, int notePort, long time, ReadOnlySpan<byte> message)
    {
        fixed (byte* p = message)
            return (MidiSendResult)NativeMethods.brack_send_midi_at_time(_handle, pluginId, (uint)notePort, time, p,
                                                                         (uint)message.Length);
    }

    public MidiSendResult SendMidiToSourceAtTime(string sourceId, long time, ReadOnlySpan<byte> message)
    {
        fixed (byte* p = message)
            return (MidiSendResult)NativeMethods.brack_send_midi_to_source_at_time(_handle, sourceId, time, p,
                                                                                   (uint)message.Length);
    }

    // ---- sessions ----

    /// <summary>Replaces the rack with the session's, or throws and leaves it untouched. A running engine restarts
    /// with the session's configuration; if that fails the load still succeeds and the engine stays stopped.</summary>
    public void LoadSession(string path) => Interop.Check(NativeMethods.brack_load_session(_handle, path));

    public void SaveSession(string path) => Interop.Check(NativeMethods.brack_save_session(_handle, path));

    public void LoadSessionJson(string json) => Interop.Check(NativeMethods.brack_load_session_json(_handle, json));

    public string SaveSessionJson() =>
        Interop.ReadText((buf, size, needed) => NativeMethods.brack_save_session_json(_handle, buf, size, needed));

    /// <summary>Removes every plugin, MIDI source and route; the configuration and master gain stay.</summary>
    public void Clear() => Interop.Check(NativeMethods.brack_clear(_handle));

    /// <summary>Goes up whenever what a session saves changes (MIDI does not count). Compare with the value at the
    /// last save to know whether to save.</summary>
    public ulong ChangeCount => NativeMethods.brack_change_count(_handle);

    // ---- events ----

    /// <summary>Takes the oldest queued event, if any. Never blocks.</summary>
    public bool TryPollEvent(out BrackEvent ev)
    {
        NativeMethods.brack_event e = default;
        e.struct_size = (uint)sizeof(NativeMethods.brack_event);
        int result = NativeMethods.brack_poll_event(_handle, &e);
        Interop.Check(result);
        if (result == 0)
        {
            ev = default;
            return false;
        }
        ev = new BrackEvent((BrackEventType)e.type, Interop.Utf8(new ReadOnlySpan<byte>(e.id, 256)),
                            Interop.Utf8(new ReadOnlySpan<byte>(e.message, 1024)), e.width, e.height);
        return true;
    }

    /// <summary>Every event queued now.</summary>
    public IReadOnlyList<BrackEvent> PollEvents()
    {
        var events = new List<BrackEvent>();
        while (TryPollEvent(out BrackEvent ev)) events.Add(ev);
        return events;
    }

    /// <summary>
    /// Raised whenever an event is queued, on the engine's thread: take the events with
    /// <see cref="TryPollEvent"/>, preferably after handing over to a thread of your own. Exceptions handlers throw
    /// are dropped.
    /// </summary>
    public event EventHandler? EventsQueued
    {
        add
        {
            lock (_notifyLock)
            {
                ObjectDisposedException.ThrowIf(_handle.IsClosed, this);
                bool first = _eventsQueued == null;
                _eventsQueued += value;
                if (!first) return;
                if (!_self.IsAllocated) _self = GCHandle.Alloc(this, GCHandleType.Weak);
                Interop.Check(NativeMethods.brack_set_event_notify(_handle, &OnEventsQueued, GCHandle.ToIntPtr(_self)));
            }
        }
        remove
        {
            lock (_notifyLock)
            {
                _eventsQueued -= value;
                if (_eventsQueued == null && !_handle.IsClosed) NativeMethods.brack_set_event_notify(_handle, null, 0);
            }
        }
    }

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static void OnEventsQueued(nint user)
    {
        try
        {
            if (GCHandle.FromIntPtr(user).Target is BrackEngine engine) engine._eventsQueued?.Invoke(engine, EventArgs.Empty);
        }
        catch
        {
            // Nothing may unwind into brack.dll.
        }
    }

    // ---- inspection ----

    /// <summary>The engine's state. Waits for the engine's thread, which may be busy (loading a plugin, say).</summary>
    public EngineStatus GetStatus() => BrackJson.Parse(GetStatusJson(), BrackJson.Default.EngineStatus);

    public string GetStatusJson() =>
        Interop.ReadText((buf, size, needed) => NativeMethods.brack_get_status(_handle, buf, size, needed));

    /// <summary>The engine's state as of at most ~50 ms ago (meters current), without waiting.</summary>
    public EngineStatus GetCachedStatus() => BrackJson.Parse(GetCachedStatusJson(), BrackJson.Default.EngineStatus);

    public string GetCachedStatusJson() =>
        Interop.ReadText((buf, size, needed) => NativeMethods.brack_get_status_cached(_handle, buf, size, needed));
}
