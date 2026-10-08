using System.Runtime.InteropServices;
using Brack;

/// <summary>A CoreMIDI source of this program's, which Brack opens as a MIDI input by name.</summary>
sealed class CoreMidiSource : IMidiSource
{
    private const string CoreMidi = "/System/Library/Frameworks/CoreMIDI.framework/CoreMIDI";
    private const string CoreFoundation = "/System/Library/Frameworks/CoreFoundation.framework/CoreFoundation";
    private readonly uint _client, _source;
    private readonly double _nsPerTick;

    public CoreMidiSource(string name)
    {
        Name = name;
        nint cfName = CFStringCreateWithCString(0, name, 0x08000100);
        if (MIDIClientCreate(cfName, 0, 0, out _client) != 0 || MIDISourceCreate(_client, cfName, out _source) != 0)
            throw new InvalidOperationException("CoreMIDI would not make the source");
        CFRelease(cfName);
        mach_timebase_info(out Timebase tb);
        _nsPerTick = (double)tb.Numer / tb.Denom;
    }

    public string Name { get; }
    public double? ClockLessBrackUs => (Ns(mach_absolute_time()) - BrackLibrary.NowNs) / 1e3;

    public void SendNow(byte[] message) => Send(message, mach_absolute_time());
    public void SendAt(byte[] message, long ns) => Send(message, Ticks(ns));
    public void SendUnstamped(byte[] message) => Send(message, 0);

    // Host ticks leave out the time asleep, which Brack's clock (steady_clock) counts.
    private static long Asleep() => (long)(mach_continuous_time() - mach_absolute_time());
    private long Ns(ulong ticks) => (long)(((long)ticks + Asleep()) * _nsPerTick);
    private ulong Ticks(long ns) => (ulong)((long)(ns / _nsPerTick) - Asleep());

    /// <summary>One message as a packet stamped <paramref name="timeStamp"/> (host ticks; 0 for none).</summary>
    private unsafe void Send(byte[] message, ulong timeStamp)
    {
        // MIDIPacketList, packed to 4: numPackets, then a MIDIPacket of timeStamp, length, data.
        const int size = 64, header = 14;
        if (message.Length > size - header) throw new ArgumentException("message too long for one packet");
        byte* list = stackalloc byte[size];
        *(uint*)list = 1;
        *(ulong*)(list + 4) = timeStamp;
        *(ushort*)(list + 12) = (ushort)message.Length;
        for (int i = 0; i < message.Length; i++) list[header + i] = message[i];
        if (MIDIReceived(_source, list) != 0) throw new InvalidOperationException("CoreMIDI would not send");
    }

    public void Dispose()
    {
        MIDIEndpointDispose(_source);
        MIDIClientDispose(_client);
    }

    private struct Timebase
    {
        public uint Numer, Denom;
    }

    [DllImport(CoreMidi)] private static extern int MIDIClientCreate(nint name, nint notify, nint refCon, out uint client);
    [DllImport(CoreMidi)] private static extern int MIDISourceCreate(uint client, nint name, out uint source);
    [DllImport(CoreMidi)] private static extern unsafe int MIDIReceived(uint source, byte* packets);
    [DllImport(CoreMidi)] private static extern int MIDIEndpointDispose(uint endpoint);
    [DllImport(CoreMidi)] private static extern int MIDIClientDispose(uint client);
    [DllImport(CoreFoundation)] private static extern nint CFStringCreateWithCString(nint alloc, string s, uint encoding);
    [DllImport(CoreFoundation)] private static extern void CFRelease(nint o);
    [DllImport("libSystem.dylib")] private static extern ulong mach_absolute_time();
    [DllImport("libSystem.dylib")] private static extern ulong mach_continuous_time();
    [DllImport("libSystem.dylib")] private static extern int mach_timebase_info(out Timebase info);
}
