using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using Brack.Native;

namespace Brack;

/// <summary>What brack.dll offers without an engine: its version, logging, devices, scans.</summary>
public static unsafe class BrackLibrary
{
    /// <summary>The C API version this binding was written against (BRACK_API_VERSION).</summary>
    public const uint BindingApiVersion = 1;

    private static readonly Lock s_lock = new();
    private static Action<BrackLogLevel, string>? s_log;

    /// <summary>
    /// Loads brack.dll (libbrack.so on Linux) from <paramref name="path"/> instead of the usual places
    /// (runtimes/&lt;rid&gt;/native/ under the application's folder, where a NuGet package or a project reference
    /// puts it, then the application's folder). Call it before anything else in this library.
    /// </summary>
    public static void LoadNativeLibrary(string path) => NativeLibraryResolver.LoadExplicit(path);

    /// <summary>The C API version of the loaded brack.dll (BRACK_API_VERSION).</summary>
    public static uint ApiVersion => NativeMethods.brack_api_version();

    /// <summary>Brack's clock, in nanoseconds: what <see cref="BrackEngine.SendMidiAtTime"/> times are on.
    /// Never blocks.</summary>
    public static long NowNs => NativeMethods.brack_now_ns();

    /// <summary>"Brack 1.0.0".</summary>
    public static string VersionString => Interop.Utf8(NativeMethods.brack_version_string()) ?? "";

    // brack.dll keeps the functions of older versions; a newer binding needs at least its own.
    internal static void CheckApiVersion()
    {
        uint version = ApiVersion;
        if (version < BindingApiVersion)
            throw new BrackException(BrackErrorCode.Failed,
                                     $"brack.dll offers API version {version}; this binding needs {BindingApiVersion}");
    }

    /// <summary>
    /// The library's log. Handlers run on Brack's own threads, so keep them short; exceptions they throw are
    /// dropped. Without handlers, Brack logs to stderr.
    /// </summary>
    public static event Action<BrackLogLevel, string>? Log
    {
        add
        {
            lock (s_lock)
            {
                bool first = s_log == null;
                s_log += value;
                if (first) NativeMethods.brack_set_log_callback(&OnLog, 0);
            }
        }
        remove
        {
            lock (s_lock)
            {
                s_log -= value;
                if (s_log == null) NativeMethods.brack_set_log_callback(null, 0);
            }
        }
    }

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static void OnLog(nint user, int level, byte* message)
    {
        try
        {
            s_log?.Invoke((BrackLogLevel)level, Interop.Utf8(message) ?? "");
        }
        catch
        {
            // Nothing may unwind into brack.dll.
        }
    }

    public static IReadOnlyList<AudioDeviceInfo> ListAudioDevices() =>
        BrackJson.Parse(ListAudioDevicesJson(), BrackJson.Default.AudioDeviceInfoArray);

    public static string ListAudioDevicesJson() =>
        Interop.ReadText((buf, size, needed) => NativeMethods.brack_list_audio_devices(buf, size, needed));

    /// <summary>The names of the MIDI input ports (for <see cref="MidiSourceKind.Hardware"/> sources).</summary>
    public static IReadOnlyList<string> ListMidiInputs() =>
        BrackJson.Parse(Interop.ReadText((buf, size, needed) => NativeMethods.brack_list_midi_inputs(buf, size, needed)),
                        BrackJson.Default.StringArray);

    /// <summary>
    /// Lists the plugins in <paramref name="extraDirectories"/> and, unless <paramref name="standardLocations"/> is
    /// false, in the standard CLAP, VST3 and VST2 locations. Runs on the calling thread and loads every plugin
    /// file (VST2 plugins are even instantiated), except those unchanged since the scan that wrote
    /// <paramref name="cachePath"/> (optional), a file of the application's (one for every architecture).
    /// </summary>
    public static IReadOnlyList<PluginDescription> ScanPlugins(string? cachePath = null,
                                                               IEnumerable<string>? extraDirectories = null,
                                                               bool standardLocations = true)
    {
        string? dirs = JoinDirectories(extraDirectories);
        uint flags = standardLocations ? 0u : 1u;  // BRACK_SCAN_EXTRA_DIRS_ONLY
        string json = Interop.ReadText(
            (buf, size, needed) => NativeMethods.brack_scan_plugins(dirs, cachePath, flags, buf, size, needed),
            256 * 1024);
        return BrackJson.Parse(json, BrackJson.Default.PluginDescriptionArray);
    }

    internal static string? JoinDirectories(IEnumerable<string>? directories) =>
        directories == null ? null : string.Join(';', directories);

    /// <summary>Whether virtual MIDI ports can be published on this system; if not, why.</summary>
    public static bool IsVirtualMidiAvailable(out string reason)
    {
        bool available = NativeMethods.brack_virtual_midi_available() != 0;
        reason = available ? "" : Interop.Utf8(NativeMethods.brack_last_error()) ?? "";
        return available;
    }

    /// <summary>
    /// True if this Windows has the Windows MIDI Services bug (microsoft/MIDI#1047) that wedges the MIDI service
    /// when a virtual port is removed, including when an engine with one is disposed: MIDI may then stop working
    /// until the next reboot. Warn the user before creating virtual ports.
    /// </summary>
    public static bool VirtualMidiRemovalHangs => NativeMethods.brack_virtual_midi_removal_hangs() != 0;
}
