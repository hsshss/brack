using System.Diagnostics;
using System.Runtime.InteropServices;
using System.Runtime.Versioning;
using System.Text;
using Brack;

/// <summary>A sender into Windows MIDI Services (MidiSrv), through the service's client COM interfaces as
/// Brack's virtual port uses them (src/core/midi/win/midisrv_abi.h), so each message carries a time stamp
/// (a QPC position: the same clock as Brack's steady_clock). The port named <see cref="Name"/> is either a
/// UMP endpoint that already exists (a loopback made with the MIDI Services tools: this sends into it, and
/// Brack opens the WinMM port of that name, <see cref="MidiSourceKind.Hardware"/>) or Brack's own virtual
/// port (<see cref="MidiSourceKind.Virtual"/>: Brack publishes it, then <see cref="Connect"/> attaches to
/// its client side).</summary>
[SupportedOSPlatform("windows")]
sealed class MidiSrvSource : IMidiSource
{
    private readonly Guid _session = Guid.NewGuid();
    private readonly IMidiTransport _transport;
    private readonly IMidiSessionTracker _tracker;
    private readonly Sink _sink = new();
    private IMidiBidirectional? _bidi;
    private string? _endpoint;

    public MidiSrvSource(string name)
    {
        Name = name;
        CoInitializeEx(0, 0);  // the multithreaded apartment; being in one already is fine too
        Guid clsid = new("2BA15E4E-5417-4A66-85B8-2B2260EFBC84"), iid = typeof(IMidiTransport).GUID;
        Check(CoCreateInstance(ref clsid, 0, 1 | 4, ref iid, out nint p), "Midi2.MidiSrvTransport");
        _transport = (IMidiTransport)Marshal.GetObjectForIUnknown(p);
        Marshal.Release(p);
        _tracker = Activate<IMidiSessionTracker>();
        Check(_tracker.Initialize(), "IMidiSessionTracker.Initialize");
        if (_tracker.VerifyConnectivity() == 0) throw new InvalidOperationException("the MIDI service (midisrv) is not reachable");
        Check(_tracker.AddClientSession(_session, "midi-timing"), "AddClientSession");
        _endpoint = FindEndpoint(path => FriendlyName(path) == name);
        Kind = _endpoint is null ? MidiSourceKind.Virtual : MidiSourceKind.Hardware;
        if (_endpoint is not null) Open();
    }

    public string Name { get; }
    public MidiSourceKind Kind { get; }
    public double? ClockLessBrackUs => (Ns(Stopwatch.GetTimestamp()) - BrackLibrary.NowNs) / 1e3;

    /// <summary>Attaches to the client side of Brack's virtual port once Brack has published it.</summary>
    public void Connect()
    {
        if (_bidi is not null) return;
        // The client-side endpoint is MIDIU_APPPUB_<unique id>, the id Brack derives from the name.
        string id = "#midiu_apppub_" + UniqueId(Name) + "#";
        for (int i = 0; i < 100 && _endpoint is null; i++)
        {
            _endpoint = FindEndpoint(path => path.Contains(id, StringComparison.OrdinalIgnoreCase));
            if (_endpoint is null) Thread.Sleep(50);
        }
        if (_endpoint is null) throw new InvalidOperationException($"no UMP endpoint for the virtual port \"{Name}\" appeared");
        Open();
    }

    public void SendNow(byte[] message) => Send(message, Stopwatch.GetTimestamp());
    public void SendAt(byte[] message, long ns) => Send(message, Ticks(ns));
    public void SendUnstamped(byte[] message) => Send(message, 0);

    // QPC ticks to and from Brack's clock, as MSVC's steady_clock counts them (__msvc_chrono.hpp).
    private static long Ns(long ticks) =>
        ticks / Stopwatch.Frequency * 1_000_000_000 + ticks % Stopwatch.Frequency * 1_000_000_000 / Stopwatch.Frequency;
    private static long Ticks(long ns) =>
        ns / 1_000_000_000 * Stopwatch.Frequency + ns % 1_000_000_000 * Stopwatch.Frequency / 1_000_000_000;

    /// <summary>One MIDI 1.0 channel voice message as a UMP (type 2, group 0) at <paramref name="position"/>.</summary>
    private unsafe void Send(byte[] message, long position)
    {
        if (_bidi is null) throw new InvalidOperationException("not connected");
        if (message.Length is < 2 or > 3 || message[0] < 0x80 || message[0] >= 0xF0)
            throw new ArgumentException("a channel voice message of 2 or 3 bytes", nameof(message));
        uint word = 0x2000_0000u | (uint)message[0] << 16 | (uint)message[1] << 8 | (message.Length == 3 ? message[2] : 0u);
        Check(_bidi.SendMidiMessage(0, (nint)(&word), 4, position), "SendMidiMessage");
    }

    private void Open()
    {
        _bidi = Activate<IMidiBidirectional>();
        var parameters = new TransportCreationParams { DataFormat = 2 /* UMP */, CallingComponent = Guid.NewGuid() };
        uint mmcss = 0;
        Check(_bidi.Initialize(_endpoint!, ref parameters, ref mmcss, _sink, 0, _session), $"opening {_endpoint}");
    }

    private T Activate<T>() where T : class
    {
        Guid iid = typeof(T).GUID;
        Check(_transport.Activate(ref iid, out nint p), "Activate " + typeof(T).Name);
        var o = (T)Marshal.GetObjectForIUnknown(p);
        Marshal.Release(p);
        return o;
    }

    public void Dispose()
    {
        if (_bidi is not null)
        {
            _bidi.Shutdown();
            Marshal.ReleaseComObject(_bidi);
            _bidi = null;
        }
        _tracker.RemoveClientSession(_session);
        _tracker.Shutdown();
        Marshal.ReleaseComObject(_tracker);
        Marshal.ReleaseComObject(_transport);
    }

    private static void Check(int hr, string what)
    {
        if (hr < 0) throw new InvalidOperationException($"{what} failed: 0x{hr:X8}");
    }

    // The endpoint's unique id as Brack's virtual port derives it from the name (midisrv_virtual_port.cpp):
    // its lowercase ASCII alphanumerics (at most 23) and the FNV-1a hash of its UTF-8.
    private static string UniqueId(string name)
    {
        byte[] utf8 = Encoding.UTF8.GetBytes(name);
        var prefix = new StringBuilder();
        foreach (byte c in utf8)
        {
            if (prefix.Length >= 23) break;
            if (c is >= (byte)'A' and <= (byte)'Z') prefix.Append((char)(c - 'A' + 'a'));
            else if (c is >= (byte)'a' and <= (byte)'z' or >= (byte)'0' and <= (byte)'9') prefix.Append((char)c);
        }
        if (prefix.Length == 0) prefix.Append("port");
        uint h = 2166136261;
        foreach (byte c in utf8)
        {
            h ^= c;
            h *= 16777619;
        }
        return prefix + h.ToString("x8");
    }

    // ---- the present UMP endpoints, as device interfaces (cfgmgr32) ----

    private static string? FindEndpoint(Func<string, bool> take)
    {
        Guid bidi = new("E7CCE071-3C03-423F-88D3-F1045D02552B");  // DEVINTERFACE_UNIVERSALMIDIPACKET_BIDI
        if (CM_Get_Device_Interface_List_SizeW(out uint length, ref bidi, null, 0) != 0) return null;
        var list = new char[length];
        if (CM_Get_Device_Interface_ListW(ref bidi, null, list, length, 0) != 0) return null;
        foreach (string path in new string(list).Split('\0', StringSplitOptions.RemoveEmptyEntries))
            if (take(path)) return path.ToLowerInvariant();
        return null;
    }

    private static string FriendlyName(string interfacePath)
    {
        var instanceKey = new DevPropKey(new Guid("78C34FC8-104A-4ACA-9EA4-524D52996E57"), 256);  // DEVPKEY_Device_InstanceId
        var nameKey = new DevPropKey(new Guid("A45C254E-DF1C-4EFD-8020-67D146A850E0"), 14);      // DEVPKEY_Device_FriendlyName
        var buffer = new byte[1024];
        uint size = (uint)buffer.Length;
        if (CM_Get_Device_Interface_PropertyW(interfacePath, ref instanceKey, out _, buffer, ref size, 0) != 0) return "";
        string instance = Encoding.Unicode.GetString(buffer, 0, (int)size).TrimEnd('\0');
        if (CM_Locate_DevNodeW(out uint node, instance, 0) != 0) return "";
        size = (uint)buffer.Length;
        if (CM_Get_DevNode_PropertyW(node, ref nameKey, out _, buffer, ref size, 0) != 0) return "";
        return Encoding.Unicode.GetString(buffer, 0, (int)size).TrimEnd('\0');
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct DevPropKey(Guid fmtid, uint pid)
    {
        public Guid Fmtid = fmtid;
        public uint Pid = pid;
    }

    [DllImport("cfgmgr32", CharSet = CharSet.Unicode)]
    private static extern int CM_Get_Device_Interface_List_SizeW(out uint length, ref Guid classGuid, string? deviceId, uint flags);
    [DllImport("cfgmgr32", CharSet = CharSet.Unicode)]
    private static extern int CM_Get_Device_Interface_ListW(ref Guid classGuid, string? deviceId, char[] buffer, uint length, uint flags);
    [DllImport("cfgmgr32", CharSet = CharSet.Unicode)]
    private static extern int CM_Get_Device_Interface_PropertyW(string interfacePath, ref DevPropKey key, out uint type, byte[] buffer, ref uint size, uint flags);
    [DllImport("cfgmgr32", CharSet = CharSet.Unicode)]
    private static extern int CM_Locate_DevNodeW(out uint node, string instanceId, uint flags);
    [DllImport("cfgmgr32", CharSet = CharSet.Unicode)]
    private static extern int CM_Get_DevNode_PropertyW(uint node, ref DevPropKey key, out uint type, byte[] buffer, ref uint size, uint flags);
    [DllImport("ole32")] private static extern int CoInitializeEx(nint reserved, uint apartment);
    [DllImport("ole32")] private static extern int CoCreateInstance(ref Guid clsid, nint outer, uint context, ref Guid iid, out nint result);

    // ---- the service's client COM ABI (midisrv_abi.h), in vtable order ----
    // Transcribed from the MIT-licensed microsoft/MIDI repository (THIRD_PARTY_NOTICES.md).
    // Copyright (c) Microsoft Corporation. Licensed under the MIT License (https://github.com/microsoft/MIDI).

    [StructLayout(LayoutKind.Sequential)]
    private struct TransportCreationParams
    {
        public int MessageOptions;
        public uint DataFormat;
        public Guid CallingComponent;
    }

    [ComImport, Guid("EA264200-3328-49E5-8815-73649A8748BE"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    private interface IMidiTransport
    {
        [PreserveSig] int Activate(ref Guid iid, out nint activated);
    }

    [ComImport, Guid("4D6A29E5-DF4F-4A2D-A923-9B23B3F2D6F6"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    private interface IMidiCallback
    {
        [PreserveSig] int Callback(int optionFlags, nint message, uint size, long position, long context);
    }

    [ComImport, Guid("B89BBB45-7001-4BEA-BBD8-C7CC26E7836C"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    private interface IMidiBidirectional
    {
        [PreserveSig] int Initialize([MarshalAs(UnmanagedType.LPWStr)] string endpointDeviceInterfaceId, ref TransportCreationParams creationParams,
                                     ref uint mmcssTaskId, IMidiCallback callback, long context, Guid sessionId);
        [PreserveSig] int Shutdown();
        [PreserveSig] int SendMidiMessage(int optionFlags, nint message, uint size, long position);
    }

    [ComImport, Guid("194c2746-3ae5-419a-94d9-20416c7dbefe"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    private interface IMidiSessionTracker
    {
        [PreserveSig] int Initialize();
        [PreserveSig] int AddClientSession(Guid sessionId, [MarshalAs(UnmanagedType.LPWStr)] string sessionName);
        [PreserveSig] int UpdateClientSessionName(Guid sessionId, [MarshalAs(UnmanagedType.LPWStr)] string sessionName);
        [PreserveSig] int RemoveClientSession(Guid sessionId);
        [PreserveSig] int GetSessionList(out nint sessionDetailsList);
        [PreserveSig] int Shutdown();
        [PreserveSig] int VerifyConnectivity();  // a BOOL
    }

    /// <summary>Receives what the endpoint sends back (nothing this program cares about).</summary>
    private sealed class Sink : IMidiCallback
    {
        public int Callback(int optionFlags, nint message, uint size, long position, long context) => 0;
    }
}
