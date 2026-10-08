// Sends notes to the test synth on a real output device every 7.3 ms, out of step with its
// periods, and reports how steadily they land against the moments they were for: the synth logs
// each one's frame (BRACK_TESTSYNTH_FRAMES). Silent: the master gain is 0.
//
// How they are sent:
//   now         brack_send_midi at each moment
//   ahead       brack_send_midi_at_time for each moment, 20 ms before it
//   midi        from a MIDI port of this program's, stamped with the moment it is sent
//   midi-ahead  from a MIDI port of this program's, stamped with each moment, 20 ms before it
//   midi-zero   from a MIDI port of this program's, unstamped, at each moment
// The MIDI port is a CoreMIDI source on macOS, an ALSA sequencer port on Linux, and on Windows a
// Windows MIDI Services endpoint of that name, else a WinMM output of that name (a loopMIDI port:
// sent at once, unstamped, so no midi-ahead), else a virtual port Brack publishes (IMidiSource.Open).
using System.Runtime.InteropServices;
using Brack;

// --exclusive: WASAPI's exclusive mode (Windows). --port=<name>: the MIDI port of the midi modes.
bool exclusive = args.Contains("--exclusive");
string port = args.FirstOrDefault(a => a.StartsWith("--port="))?["--port=".Length..] ?? "brack midi timing";
args = [.. args.Where(a => !a.StartsWith("--"))];
if (args.Length < 2)
{
    Console.Error.WriteLine("usage: MidiTiming <libbrack> <brack-test-synth.clap> [mode] [buffer frames] [count] [device | list] [--exclusive] [--port=<MIDI port>]");
    return 2;
}
string lib = args[0], synth = args[1];
string mode = args.Length > 2 ? args[2] : "now";
uint buffer = args.Length > 3 ? uint.Parse(args[3]) : 480;
int count = args.Length > 4 ? int.Parse(args[4]) : 1500;
string? device = args.Length > 5 ? args[5] : null;
const long IntervalNs = 7_300_000, AheadNs = 20_000_000;

// macOS: the engine works on the main thread, whose run loop the application runs (brack.h).
if (!OperatingSystem.IsMacOS()) return Run();
int result = 0;
var measuring = new Thread(() =>
{
    result = Run();
    CFRunLoopStop(CFRunLoopGetMain());
});
measuring.Start();
CFRunLoopRun();
measuring.Join();
return result;

[DllImport("/System/Library/Frameworks/CoreFoundation.framework/CoreFoundation")]
static extern void CFRunLoopRun();
[DllImport("/System/Library/Frameworks/CoreFoundation.framework/CoreFoundation")]
static extern nint CFRunLoopGetMain();
[DllImport("/System/Library/Frameworks/CoreFoundation.framework/CoreFoundation")]
static extern void CFRunLoopStop(nint loop);
[DllImport("libc", EntryPoint = "setenv")]
static extern int SetEnv(string name, string value, int overwrite);

int Run()
{
    string log = Path.Combine(Path.GetTempPath(), $"brack_midi_timing-{Environment.ProcessId}.log");
    // The synth logs in the plugin host process, which inherits the C library's environment.
    foreach ((string name, string value) in new[] { ("BRACK_TESTSYNTH_LOG", log), ("BRACK_TESTSYNTH_FRAMES", "1") })
    {
        Environment.SetEnvironmentVariable(name, value);
        if (!OperatingSystem.IsWindows()) SetEnv(name, value, 1);
    }
    BrackLibrary.LoadNativeLibrary(lib);
    if (device == "list")
    {
        foreach (AudioDeviceInfo d in BrackLibrary.ListAudioDevices())
            Console.WriteLine($"{d.Name} ({string.Join(", ", d.SampleRates)}){(d.IsDefault ? ", default" : "")}");
        return 0;
    }

    using var engine = new BrackEngine();
    engine.SetConfig(engine.GetConfig() with { BufferFrames = buffer, AudioDevice = device, Exclusive = exclusive });
    engine.MasterGain = 0;
    engine.AddPlugin(synth, id: "t");
    using IMidiSource? source = mode.StartsWith("midi") ? IMidiSource.Open(port) : null;
    if (source is not null)
    {
        engine.AddMidiSource(source.Kind, source.Name, "in");
        source.Connect();
        engine.ConnectMidi("in", "t");
        if (source.ClockLessBrackUs is double us) Console.WriteLine($"  the port's clock less Brack's: {us:F1} us");
    }
    engine.Start();
    EngineStatus status = engine.GetStatus();
    if (status.ProcessSampleRate != status.OutputSampleRate)
    {
        Console.Error.WriteLine("the plugins run at another rate than the device: pick a device at their rate");
        return 1;
    }

    Thread.CurrentThread.Priority = ThreadPriority.Highest;
    bool ahead = mode.EndsWith("ahead");
    // When each message is for, on Brack's clock, and the render position as it was sent.
    var sent = new (long Ns, ulong Position)[count];
    long start = BrackLibrary.NowNs + 1_000_000_000;
    for (int i = 0; i < count; i++)
    {
        long moment = start + i * IntervalNs;
        while (BrackLibrary.NowNs < moment - (ahead ? AheadNs : 0))
        {
        }
        ulong position = engine.RenderPosition;
        long now = BrackLibrary.NowNs;
        // The note and velocity number the message.
        byte[] note = [0x90, (byte)(i % 128), (byte)(1 + i / 128 % 127)];
        var result = MidiSendResult.Sent;
        switch (mode)
        {
            case "now": result = engine.SendMidi("t", 0, note); break;
            case "ahead": result = engine.SendMidiAtTime("t", 0, moment, note); break;
            case "midi": source!.SendNow(note); break;
            case "midi-ahead": source!.SendAt(note, moment); break;
            case "midi-zero": source!.SendUnstamped(note); break;
            default: throw new ArgumentException($"no mode {mode}");
        }
        if (result != MidiSendResult.Sent) Console.Error.WriteLine($"message {i} not taken: {result}");
        sent[i] = (ahead ? moment : now, position);
    }
    // Long enough for the last to be rendered: the PipeWire of a VM holds one back up to 0.6 s.
    Thread.Sleep(3000);
    engine.Stop();
    engine.RemovePlugin("t");  // writes the log

    var frames = new long?[count];
    foreach (string line in File.ReadLines(log))
    {
        // "midi port=0 frame=F: 90 KK VV"
        string[] parts = line.Split(' ');
        if (parts.Length != 6 || parts[0] != "midi" || !parts[2].StartsWith("frame=")) continue;
        int i = (Convert.ToInt32(parts[5], 16) - 1) * 128 + Convert.ToInt32(parts[4], 16);
        if (i < count) frames[i] = long.Parse(parts[2]["frame=".Length..^1]);
    }
    File.Delete(log);
    int reached = frames.Count(f => f is not null);
    Console.WriteLine($"  {reached} of {count} messages reached the plugin{(reached < count ? $", {count - reached} lost" : "")}");
    // Sent in order, they should land in order: one landing before the one sent before it is out of order.
    int outOfOrder = 0, worst = 0;
    long? last = null;
    foreach (long? f in frames)
    {
        if (f is not long frame) continue;
        if (last is long before && frame < before)
        {
            outOfOrder++;
            worst = Math.Max(worst, (int)(before - frame));
        }
        last = last is long l ? Math.Max(l, frame) : frame;
    }
    Console.WriteLine($"  {outOfOrder} landed before one sent earlier{(outOfOrder > 0 ? $", by up to {worst} frames" : "")}");

    // From a second in, against the line that fits them best: the device's clock drifts from the stopwatch's.
    var times = new List<double>();
    var landed = new List<double>();
    var after = new List<double>();
    for (int i = 0; i < count; i++)
    {
        double t = (sent[i].Ns - start) / 1e9;
        if (t < 1 || frames[i] is not long frame) continue;
        times.Add(t);
        landed.Add(frame);
        after.Add(frame - (double)sent[i].Position);
    }
    if (times.Count < 2)
    {
        Console.Error.WriteLine("too few messages landed");
        return 1;
    }
    double mt = times.Average(), ml = landed.Average();
    double slope = times.Zip(landed).Sum(p => (p.First - mt) * (p.Second - ml)) / times.Sum(t => (t - mt) * (t - mt));
    double[] off = [.. times.Zip(landed).Select(p => p.Second - (ml + slope * (p.First - mt))).Order()];
    double rate = status.OutputSampleRate;
    double Ms(double f) => f / rate * 1000;

    Console.WriteLine($"{mode} on {status.Device}: {rate} Hz, period {status.PeriodFrames}, block {status.BlockSize}; " +
                      $"{off.Length} of {count} messages, from a second in");
    Console.WriteLine($"  spread {Ms(off[^1] - off[0]):F3} ms (1st to 99th percentile " +
                      $"{Ms(off[(int)(0.99 * (off.Length - 1))] - off[(int)(0.01 * (off.Length - 1))]):F3} ms)");
    Console.WriteLine($"  after the render position when sent{(ahead ? $" ({AheadNs / 1e6} ms ahead)" : "")}: " +
                      $"mean {Ms(after.Average()):F2} ms, {Ms(after.Min()):F2} to {Ms(after.Max()):F2} ms");
    return 0;
}

/// <summary>A MIDI port of this program's, which Brack opens as a MIDI input by <see cref="Name"/>.</summary>
interface IMidiSource : IDisposable
{
    static IMidiSource Open(string name)
    {
        if (OperatingSystem.IsMacOS()) return new CoreMidiSource(name);
        if (OperatingSystem.IsWindows())
        {
            var midiSrv = new MidiSrvSource(name);
            if (midiSrv.Kind != MidiSourceKind.Virtual || WinMmSource.TryOpen(name) is not { } winMm) return midiSrv;
            midiSrv.Dispose();
            return winMm;
        }
        if (OperatingSystem.IsLinux()) return new AlsaSeqSource(name);
        throw new PlatformNotSupportedException("no MIDI port to send from on this OS");
    }

    /// <summary>The name Brack finds the port by.</summary>
    string Name { get; }

    /// <summary>How Brack opens it: an existing port by name, or a virtual port Brack publishes itself.</summary>
    MidiSourceKind Kind => MidiSourceKind.Hardware;

    /// <summary>Called once Brack has opened (or published) the port; a source that sends into Brack's own
    /// virtual port attaches to it here.</summary>
    void Connect() { }

    /// <summary>The port's clock less Brack's (steady_clock), in microseconds, when the port has its own.</summary>
    double? ClockLessBrackUs { get; }

    /// <summary>Sends <paramref name="message"/> stamped with the moment it is sent.</summary>
    void SendNow(byte[] message);

    /// <summary>Sends <paramref name="message"/> stamped with <paramref name="ns"/> on Brack's clock.</summary>
    void SendAt(byte[] message, long ns);

    /// <summary>Sends <paramref name="message"/> without a time stamp.</summary>
    void SendUnstamped(byte[] message);
}
