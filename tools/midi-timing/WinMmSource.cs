using System.Runtime.InteropServices;
using System.Runtime.Versioning;

/// <summary>A sender through an existing WinMM output (a loopMIDI port, say), which Brack opens as the WinMM
/// input of the same name: the port turns what is sent out into its input. WinMM sends at once and carries
/// no time stamp, so <see cref="SendAt"/> has no way through it.</summary>
[SupportedOSPlatform("windows")]
sealed class WinMmSource : IMidiSource
{
    private nint _out;

    private WinMmSource(string name, nint handle)
    {
        Name = name;
        _out = handle;
    }

    /// <summary>The WinMM output named <paramref name="name"/>, opened; null when there is none.</summary>
    public static WinMmSource? TryOpen(string name)
    {
        uint count = midiOutGetNumDevs();
        for (uint i = 0; i < count; i++)
        {
            if (midiOutGetDevCapsW(i, out MidiOutCaps caps, (uint)Marshal.SizeOf<MidiOutCaps>()) != 0 || caps.Name != name) continue;
            uint result = midiOutOpen(out nint handle, i, 0, 0, 0);
            if (result != 0) throw new InvalidOperationException($"midiOutOpen(\"{name}\") failed: {result}");
            return new WinMmSource(name, handle);
        }
        return null;
    }

    public string Name { get; }
    public double? ClockLessBrackUs => null;

    public void SendNow(byte[] message)
    {
        if (message.Length is < 2 or > 3) throw new ArgumentException("a short message of 2 or 3 bytes", nameof(message));
        uint word = message[0] | (uint)message[1] << 8 | (message.Length == 3 ? (uint)message[2] << 16 : 0u);
        uint result = midiOutShortMsg(_out, word);
        if (result != 0) throw new InvalidOperationException($"midiOutShortMsg failed: {result}");
    }

    public void SendAt(byte[] message, long ns) =>
        throw new NotSupportedException("WinMM sends at once: midi-ahead needs a MIDI Services endpoint");

    public void SendUnstamped(byte[] message) => SendNow(message);

    public void Dispose()
    {
        if (_out == 0) return;
        midiOutClose(_out);
        _out = 0;
    }

    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
    private struct MidiOutCaps
    {
        public ushort Mid, Pid;
        public uint DriverVersion;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 32)] public string Name;
        public ushort Technology, Voices, Notes, ChannelMask;
        public uint Support;
    }

    [DllImport("winmm.dll")]
    private static extern uint midiOutGetNumDevs();
    [DllImport("winmm.dll", CharSet = CharSet.Unicode)]
    private static extern uint midiOutGetDevCapsW(nuint id, out MidiOutCaps caps, uint size);
    [DllImport("winmm.dll")]
    private static extern uint midiOutOpen(out nint handle, uint id, nint callback, nint instance, uint flags);
    [DllImport("winmm.dll")]
    private static extern uint midiOutShortMsg(nint handle, uint message);
    [DllImport("winmm.dll")]
    private static extern uint midiOutClose(nint handle);
}
