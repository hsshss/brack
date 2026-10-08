using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;

namespace Brack.Native;

// brack.dll's C API one to one: the names, parameters and structs of include/brack/brack.h.
// test_api_parity checks that every function of the header is declared here.
internal static unsafe partial class NativeMethods
{
    internal const string Library = "brack";

    static NativeMethods() => NativeLibraryResolver.Register();

    internal const int BRACK_OK = 0;
    internal const int BRACK_ERR_BUFFER_TOO_SMALL = -4;
    internal const int BRACK_ID_MAX = 64;

    [StructLayout(LayoutKind.Sequential)]
    internal struct brack_config
    {
        public uint struct_size;
        public byte* audio_device;
        public uint device_sample_rate;
        public uint channels;
        public uint buffer_frames;
        public int exclusive;
        public uint process_sample_rate;
        public uint block_size;
        public int resampler_quality;
        public int plugins_in_process;
    }

    [StructLayout(LayoutKind.Sequential)]
    internal struct brack_event
    {
        public uint struct_size;
        public int type;
        public fixed byte id[256];
        public fixed byte message[1024];
        public uint width;
        public uint height;
    }

    [LibraryImport(Library)]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    internal static partial uint brack_api_version();

    [LibraryImport(Library)]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    internal static partial byte* brack_version_string();

    [LibraryImport(Library)]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    internal static partial byte* brack_last_error();

    [LibraryImport(Library)]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    internal static partial void brack_set_log_callback(delegate* unmanaged[Cdecl]<nint, int, byte*, void> fn, nint user);

    [LibraryImport(Library)]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    internal static partial BrackEngineHandle brack_engine_create();

    [LibraryImport(Library)]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    internal static partial void brack_engine_destroy(nint e);

    [LibraryImport(Library)]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    internal static partial int brack_set_config(BrackEngineHandle e, brack_config* cfg);

    [LibraryImport(Library)]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    internal static partial int brack_get_config(BrackEngineHandle e, brack_config* cfg);

    [LibraryImport(Library)]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    internal static partial int brack_start(BrackEngineHandle e);

    [LibraryImport(Library)]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    internal static partial int brack_start_manual(BrackEngineHandle e, uint sample_rate, uint channels, uint max_frames);

    [LibraryImport(Library)]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    internal static partial void brack_stop(BrackEngineHandle e);

    [LibraryImport(Library)]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    internal static partial void brack_render(BrackEngineHandle e, float** outputs, uint channels, uint frames);

    [LibraryImport(Library)]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    internal static partial ulong brack_get_render_position(BrackEngineHandle e);

    [LibraryImport(Library, StringMarshalling = StringMarshalling.Utf8)]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    internal static partial int brack_add_plugin(BrackEngineHandle e, string? id, string plugin_path, string? plugin_id,
                                                 byte* id_out, nuint id_out_size);

    [LibraryImport(Library, StringMarshalling = StringMarshalling.Utf8)]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    internal static partial int brack_add_plugin_ex(BrackEngineHandle e, string? id, string plugin_path, string? plugin_id,
                                                    uint flags, byte* id_out, nuint id_out_size);

    [LibraryImport(Library, StringMarshalling = StringMarshalling.Utf8)]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    internal static partial int brack_remove_plugin(BrackEngineHandle e, string plugin_id);

    [LibraryImport(Library, StringMarshalling = StringMarshalling.Utf8)]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    internal static partial int brack_reload_plugin(BrackEngineHandle e, string plugin_id);

    [LibraryImport(Library, StringMarshalling = StringMarshalling.Utf8)]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    internal static partial int brack_move_plugin(BrackEngineHandle e, string plugin_id, uint index);

    [LibraryImport(Library, StringMarshalling = StringMarshalling.Utf8)]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    internal static partial int brack_show_plugin_gui(BrackEngineHandle e, string plugin_id, int visible);

    [LibraryImport(Library, StringMarshalling = StringMarshalling.Utf8)]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    internal static partial int brack_show_plugin_gui_in(BrackEngineHandle e, string plugin_id, nint parent_window);

    [LibraryImport(Library, StringMarshalling = StringMarshalling.Utf8)]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    internal static partial int brack_set_plugin_name(BrackEngineHandle e, string plugin_id, string? name);

    [LibraryImport(Library, StringMarshalling = StringMarshalling.Utf8)]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    internal static partial int brack_get_plugin_state(BrackEngineHandle e, string plugin_id, byte* buf, nuint size, nuint* needed);

    [LibraryImport(Library, StringMarshalling = StringMarshalling.Utf8)]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    internal static partial int brack_set_plugin_state(BrackEngineHandle e, string plugin_id, byte* data, nuint size);

    [LibraryImport(Library, StringMarshalling = StringMarshalling.Utf8)]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    internal static partial int brack_add_midi_source(BrackEngineHandle e, string? id, int kind, string? name, byte* id_out,
                                                      nuint id_out_size);

    [LibraryImport(Library, StringMarshalling = StringMarshalling.Utf8)]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    internal static partial int brack_remove_midi_source(BrackEngineHandle e, string source_id);

    [LibraryImport(Library, StringMarshalling = StringMarshalling.Utf8)]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    internal static partial int brack_move_midi_source(BrackEngineHandle e, string source_id, uint index);

    [LibraryImport(Library, StringMarshalling = StringMarshalling.Utf8)]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    internal static partial int brack_reopen_midi_source(BrackEngineHandle e, string source_id);

    [LibraryImport(Library, StringMarshalling = StringMarshalling.Utf8)]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    internal static partial int brack_connect_midi(BrackEngineHandle e, string source_id, string plugin_id, uint note_port);

    [LibraryImport(Library, StringMarshalling = StringMarshalling.Utf8)]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    internal static partial int brack_disconnect_midi(BrackEngineHandle e, string source_id, string plugin_id, uint note_port);

    [LibraryImport(Library, StringMarshalling = StringMarshalling.Utf8)]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    internal static partial int brack_connect_audio(BrackEngineHandle e, string plugin_id, uint port, uint channel, uint output,
                                                    float gain);

    [LibraryImport(Library, StringMarshalling = StringMarshalling.Utf8)]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    internal static partial int brack_disconnect_audio(BrackEngineHandle e, string plugin_id, uint port, uint channel, uint output);

    [LibraryImport(Library, StringMarshalling = StringMarshalling.Utf8)]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    internal static partial int brack_clear_audio_routes(BrackEngineHandle e, string plugin_id);

    [LibraryImport(Library)]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    internal static partial int brack_set_master_gain(BrackEngineHandle e, float gain);

    [LibraryImport(Library)]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    internal static partial float brack_get_master_gain(BrackEngineHandle e);

    [LibraryImport(Library, StringMarshalling = StringMarshalling.Utf8)]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    internal static partial int brack_send_midi(BrackEngineHandle e, string plugin_id, uint note_port, byte* data, uint size);

    [LibraryImport(Library, StringMarshalling = StringMarshalling.Utf8)]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    internal static partial int brack_send_midi_to_source(BrackEngineHandle e, string source_id, byte* data, uint size);

    [LibraryImport(Library, StringMarshalling = StringMarshalling.Utf8)]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    internal static partial int brack_send_midi_at(BrackEngineHandle e, string plugin_id, uint note_port, ulong time, byte* data,
                                                   uint size);

    [LibraryImport(Library, StringMarshalling = StringMarshalling.Utf8)]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    internal static partial int brack_send_midi_to_source_at(BrackEngineHandle e, string source_id, ulong time, byte* data,
                                                             uint size);

    [LibraryImport(Library)]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    internal static partial long brack_now_ns();

    [LibraryImport(Library, StringMarshalling = StringMarshalling.Utf8)]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    internal static partial int brack_send_midi_at_time(BrackEngineHandle e, string plugin_id, uint note_port, long time,
                                                        byte* data, uint size);

    [LibraryImport(Library, StringMarshalling = StringMarshalling.Utf8)]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    internal static partial int brack_send_midi_to_source_at_time(BrackEngineHandle e, string source_id, long time,
                                                                  byte* data, uint size);

    [LibraryImport(Library, StringMarshalling = StringMarshalling.Utf8)]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    internal static partial int brack_load_session(BrackEngineHandle e, string path);

    [LibraryImport(Library, StringMarshalling = StringMarshalling.Utf8)]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    internal static partial int brack_save_session(BrackEngineHandle e, string path);

    [LibraryImport(Library, StringMarshalling = StringMarshalling.Utf8)]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    internal static partial int brack_load_session_json(BrackEngineHandle e, string json);

    [LibraryImport(Library)]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    internal static partial int brack_save_session_json(BrackEngineHandle e, byte* buf, nuint size, nuint* needed);

    [LibraryImport(Library)]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    internal static partial int brack_clear(BrackEngineHandle e);

    [LibraryImport(Library)]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    internal static partial ulong brack_change_count(BrackEngineHandle e);

    [LibraryImport(Library)]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    internal static partial int brack_poll_event(BrackEngineHandle e, brack_event* out_);

    [LibraryImport(Library)]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    internal static partial int brack_set_event_notify(BrackEngineHandle e, delegate* unmanaged[Cdecl]<nint, void> fn, nint user);

    [LibraryImport(Library)]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    internal static partial int brack_get_status(BrackEngineHandle e, byte* buf, nuint size, nuint* needed);

    [LibraryImport(Library)]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    internal static partial int brack_get_status_cached(BrackEngineHandle e, byte* buf, nuint size, nuint* needed);

    [LibraryImport(Library)]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    internal static partial int brack_list_audio_devices(byte* buf, nuint size, nuint* needed);

    [LibraryImport(Library)]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    internal static partial int brack_list_midi_inputs(byte* buf, nuint size, nuint* needed);

    [LibraryImport(Library, StringMarshalling = StringMarshalling.Utf8)]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    internal static partial int brack_scan_plugins(string? extra_dirs, string? cache_path, uint flags, byte* buf, nuint size,
                                                   nuint* needed);

    [LibraryImport(Library)]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    internal static partial int brack_virtual_midi_available();

    [LibraryImport(Library)]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    internal static partial int brack_virtual_midi_removal_hangs();
}
