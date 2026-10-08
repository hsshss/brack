// Drives brack.dll through the .NET binding: the same ground as tests/test_dll.c, from C#.
using System.Runtime.InteropServices;
using Brack;

if (args.Length < 2)
{
    Console.Error.WriteLine("usage: Brack.Tests <brack.dll | libbrack.so | libbrack.dylib> <brack-test-synth.clap>");
    return 2;
}
// macOS: the engine works on the main thread, whose run loop the application runs (brack.h).
if (!OperatingSystem.IsMacOS()) return RunTests();
int result = 0;
var tests = new Thread(() =>
{
    result = RunTests();
    CFRunLoopStop(CFRunLoopGetMain());
});
tests.Start();
CFRunLoopRun();
tests.Join();
return result;

[DllImport("/System/Library/Frameworks/CoreFoundation.framework/CoreFoundation")]
static extern void CFRunLoopRun();
[DllImport("/System/Library/Frameworks/CoreFoundation.framework/CoreFoundation")]
static extern nint CFRunLoopGetMain();
[DllImport("/System/Library/Frameworks/CoreFoundation.framework/CoreFoundation")]
static extern void CFRunLoopStop(nint loop);

int RunTests()
{
    string synth = args[1];
    string log = Path.Combine(Path.GetTempPath(), $"brack_test_dotnet-{Environment.ProcessId}.log");
    // The test synth logs what it receives there, in the plugin host process. .NET keeps an environment of its
    // own on Unix: the C library's, which the plugin host inherits, is set too.
    Environment.SetEnvironmentVariable("BRACK_TESTSYNTH_LOG", log);
    if (!OperatingSystem.IsWindows()) SetEnv("BRACK_TESTSYNTH_LOG", log, 1);
    BrackLibrary.LoadNativeLibrary(args[0]);

    [DllImport("libc", EntryPoint = "setenv")]
    static extern int SetEnv(string name, string value, int overwrite);

    int failures = 0;
    void Check(bool condition, string what)
    {
        if (condition) return;
        Console.Error.WriteLine($"CHECK failed: {what}");
        ++failures;
    }
    void Throws(BrackErrorCode code, Action action, string what)
    {
        try
        {
            action();
            Check(false, $"{what} did not throw");
        }
        catch (BrackException e)
        {
            Check(e.Code == code, $"{what}: {e.Code} ({e.Message})");
        }
    }
    static bool ThrowsArgument(Action action)
    {
        try
        {
            action();
            return false;
        }
        catch (ArgumentException)
        {
            return true;
        }
    }
    static float Peak(BrackEngine engine, int frames)
    {
        var outs = new[] { new float[256], new float[256] };
        float peak = 0;
        for (int done = 0; done < frames; done += 256)
        {
            engine.Render(outs, 256);
            foreach (var o in outs) peak = Math.Max(peak, o.Max(Math.Abs));
        }
        return peak;
    }
    static bool WaitFor(BrackEngine engine, BrackEventType type, out BrackEvent found)
    {
        for (int i = 0; i < 200; ++i)
        {
            while (engine.TryPollEvent(out found))
                if (found.Type == type) return true;
            Thread.Sleep(10);
        }
        found = default;
        return false;
    }

    var logged = new List<string>();
    void OnLog(BrackLogLevel level, string message)
    {
        lock (logged) logged.Add(message);
    }
    BrackLibrary.Log += OnLog;

    Check(BrackLibrary.ApiVersion >= BrackLibrary.BindingApiVersion, "API version");
    Console.WriteLine($"{BrackLibrary.VersionString}, API {BrackLibrary.ApiVersion}");

    // Playing a note, with and without sample rate conversion.
    using (var engine = new BrackEngine())
    {
        string id = engine.AddPlugin(synth);
        Console.WriteLine($"plugin id: {id}");
        Throws(BrackErrorCode.Failed, () => engine.AddPlugin(synth, id: id), "duplicate id");
        Throws(BrackErrorCode.Failed, () => engine.AddPlugin("does-not-exist.clap"), "missing file");
        foreach (uint rate in new uint[] { 0, 96000 })
        {
            engine.SetConfig(new BrackConfig { ProcessSampleRate = rate, ResamplerQuality = ResamplerQuality.Ultra });
            engine.StartManual(48000, 2, 256);
            Check(Peak(engine, 4800) == 0, "silent before a note");
            Check(engine.SendMidi(id, 0, [0x90, 69, 127]) == MidiSendResult.Sent, "note on");
            float peak = Peak(engine, 24000);
            Console.WriteLine($"peak at process rate {rate}: {peak:F3}");
            Check(peak > 0.2f, "the note sounds");
            Check(engine.SendMidi(id, 0, [0x80, 69, 0]) == MidiSendResult.Sent, "note off");
            Peak(engine, 4800);
            Check(Peak(engine, 4800) < 1e-3f, "released");
            Check(engine.SendMidi(id, 0, [0x45]) == MidiSendResult.InvalidArgument, "a message without a status byte is rejected");
            Check(engine.SendMidi("nope", 0, [0x90, 60, 1]) == MidiSendResult.NotFound, "an unknown plugin is rejected");
            Check(engine.SendMidi(id, -1, [0x90, 60, 1]) == MidiSendResult.InvalidArgument, "a negative note port is rejected");
            engine.Stop();
        }

        // Interleaved rendering, as audio APIs want it.
        engine.StartManual(48000, 2, 128);
        engine.SendMidi(id, 0, [0x90, 69, 127]);
        var interleaved = new float[2 * 1000];
        engine.RenderInterleaved(interleaved, 2);
        Check(interleaved.Max(Math.Abs) > 0.1f && interleaved[100] == interleaved[101], "interleaved stereo");

        EngineStatus status = engine.GetStatus();
        Check(status.Mode == EngineMode.Manual && status.Plugins.Count == 1, "status");
        PluginStatus plugin = status.Plugins[0];
        Check(plugin.Format == PluginFormat.Clap && plugin.PluginId == "brack.test.synth" && plugin.HasGui, "plugin status");
        Check(plugin.SeparateProcess && plugin.Architecture == RuntimeInformation.ProcessArchitecture switch
        {
            Architecture.X64 => "x64",
            Architecture.X86 => "x86",
            Architecture.Arm64 => "arm64",
            var other => other.ToString(),
        }, "plugin host process");
        Check(plugin.NotePorts.Count == 1 && plugin.NotePorts[0].Midi && plugin.AudioOutputs[0].Channels == 2, "ports");
        Check(status.AudioRoutes.Count == 2 && status.AudioRoutes[1].Output == 1, "routed to outputs 1 and 2");

        engine.SetPluginName(id, "My synth");
        Check(engine.GetStatus().Plugins[0].Name == "My synth", "renamed");
        Throws(BrackErrorCode.NotFound, () => engine.SetPluginName("nope", "x"), "renaming an unknown plugin");
        engine.MasterGain = 0.25f;
        Check(engine.MasterGain == 0.25f, "master gain");
        Throws(BrackErrorCode.InvalidArgument, () => engine.MasterGain = -1, "a negative gain");
    }

    // Changes, events, and a plugin crashing on the audio thread.
    using (var engine = new BrackEngine())
    {
        int notified = 0;
        engine.EventsQueued += (_, _) => Interlocked.Increment(ref notified);
        ulong before = engine.ChangeCount;
        engine.AddPlugin(synth, id: "a");
        Check(engine.ChangeCount > before, "adding counts as a change");
        Check(WaitFor(engine, BrackEventType.Changed, out _), "changed event");
        engine.StartManual(48000, 2, 256);
        engine.SendMidi("a", 0, [0xF0, 0x7D, 0x63, 0x01, 0xF7]);  // the test synth crashes on this
        Peak(engine, 512);
        Check(WaitFor(engine, BrackEventType.PluginCrashed, out BrackEvent crash), "crash event");
        Console.WriteLine($"crash: {crash.Id}: {crash.Message}");
        Check(crash.Id == "a" && crash.Message.Contains("crashed in process"), "crash event contents");
        engine.ReloadPlugin("a");
        engine.SendMidi("a", 0, [0x90, 0x3C, 0x64]);
        Check(Peak(engine, 4800) > 0.1f, "reloaded, it plays");
        Throws(BrackErrorCode.NotFound, () => engine.ReloadPlugin("nope"), "reloading an unknown plugin");
        Check(engine.GetCachedStatus().Plugins.Count == 1, "cached status");
        Check(Volatile.Read(ref notified) > 0, "EventsQueued raised");
    }

    // Rack edits, configuration, state, sessions.
    using (var engine = new BrackEngine())
    {
        engine.SetConfig(new BrackConfig { AudioDevice = "brack test device", BlockSize = 128 });
        BrackConfig config = engine.GetConfig();
        Check(config.AudioDevice == "brack test device" && config.BlockSize == 128 && config.Channels == 2 &&
              !config.PluginsInProcess, "configuration");
        engine.SetConfig(config with { PluginsInProcess = true });
        Check(engine.GetConfig().PluginsInProcess, "plugins in this process");
        engine.SetConfig(config);

        engine.StartManual(48000, 2, 256);
        engine.AddPlugin(synth, id: "a");
        engine.AddPlugin(synth, id: "b", routeAudio: false);
        engine.SendMidi("b", 0, [0x90, 69, 127]);
        Check(Peak(engine, 4800) == 0, "unrouted plugin is silent");
        engine.ConnectAudio("b", 0, 0, 0);
        Check(Peak(engine, 4800) > 0.1f, "routed");
        Check(engine.DisconnectAudio("b", 0, 0, 0) && !engine.DisconnectAudio("b", 0, 0, 0), "disconnect once");
        Check(ThrowsArgument(() => engine.ConnectAudio("b", -1, 0, 0)), "a negative port is refused");
        Check(ThrowsArgument(() => engine.Render([new float[16], new float[16]], 17)), "frames past the arrays are refused");

        engine.MovePlugin("b", 0);
        string src = engine.AddMidiSource(MidiSourceKind.Api, id: "keys");
        engine.ReopenMidiSource(src);
        engine.ConnectMidi(src, "a");
        Check(engine.DisconnectMidi(src, "a") && !engine.DisconnectMidi(src, "a"), "MIDI route once");
        EngineStatus status = engine.GetStatus();
        Check(status.Plugins[0].Id == "b" && status.MidiSources[0].Kind == MidiSourceKind.Api, "order and source");

        byte[] state = engine.GetPluginState("a");
        Check(state.Length == 4, "plugin state");
        engine.SetPluginState("a", BitConverter.GetBytes(0.5f));
        Check(BitConverter.ToSingle(engine.GetPluginState("a")) == 0.5f, "state round trip");
        Throws(BrackErrorCode.Failed, () => engine.SetPluginState("a", [1, 2]), "state the plugin rejects");

        string session = engine.SaveSessionJson();
        engine.Clear();
        Check(engine.GetStatus().Plugins.Count == 0, "cleared");
        engine.LoadSessionJson(session);
        Check(engine.GetStatus().Plugins.Count == 2, "session restored");
        Throws(BrackErrorCode.Failed, () => engine.LoadSessionJson("{}"), "not a session");
    }

    // MIDI timed to the frame (the test synth logs frame offsets).
    using (var engine = new BrackEngine())
    {
        engine.StartManual(48000, 2, 256);  // plugin blocks of 256 frames
        engine.AddPlugin(synth, id: "t");
        ulong pos = engine.RenderPosition;
        engine.SendMidiAt("t", 0, pos + 300, [0x90, 62, 100]);
        engine.SendMidiAt("t", 0, pos + 100, [0x90, 60, 100]);
        Peak(engine, 512);
        Check(engine.RenderPosition == pos + 512, "render position");
        // Manual mode has no real time to go by: a time on Brack's clock is as soon as possible.
        Check(engine.SendMidiAtTime("t", 0, BrackLibrary.NowNs + 1_000_000_000, [0x90, 64, 100]) == MidiSendResult.Sent, "sent for a time");
        Check(engine.SendMidiAtTime("t", 0, 0, [0x90, 64, 100]) == MidiSendResult.InvalidArgument, "no time");
        Peak(engine, 256);
        engine.RemovePlugin("t");
    }
    string[] lines = File.Exists(log) ? File.ReadAllLines(log) : [];
    Check(lines.Contains("midi port=0 @100: 90 3C 64") && lines.Contains("midi port=0 @44: 90 3E 64"), "timed MIDI");
    Check(lines.Contains("midi port=0: 90 40 64"), "MIDI for a time, in manual mode");

    // A cached scan of the folder holding the test synth only.
    string cache = Path.Combine(Path.GetTempPath(), $"brack_test_dotnet_cache-{Environment.ProcessId}.json");
    for (int pass = 0; pass < 2; ++pass)
    {
        var found = BrackLibrary.ScanPlugins(cache, [Path.GetDirectoryName(Path.GetFullPath(synth))!], standardLocations: false);
        Check(found.Any(p => p.Id == "brack.test.synth" && p.Format == PluginFormat.Clap && p.Instrument), "scan finds the synth");
    }
    Check(File.Exists(cache), "scan cache written");
    File.Delete(cache);

    Check(BrackLibrary.ListAudioDevices() != null && BrackLibrary.ListMidiInputs() != null, "device lists");
    Console.WriteLine($"virtual MIDI: {BrackLibrary.IsVirtualMidiAvailable(out string reason)} {reason}; " +
                      $"removal hangs: {BrackLibrary.VirtualMidiRemovalHangs}");
    lock (logged) Check(logged.Any(m => m.Contains("crashed in process")), "log callback");
    BrackLibrary.Log -= OnLog;
    File.Delete(log);

    Console.WriteLine(failures == 0 ? "PASS" : $"FAILED ({failures})");
    return failures == 0 ? 0 : 1;
}
