using System.Reflection;
using System.Runtime.InteropServices;

namespace Brack.Native;

// Where Brack's native library (brack.dll; libbrack.so on Linux, libbrack.dylib on macOS) is loaded
// from, in this order:
//   1. a file named by BrackLibrary.LoadNativeLibrary;
//   2. runtimes/<rid>/native/ under the application's folder, for this process's system and
//      architecture: where a project reference to Brack.csproj copies it (NuGet puts it there too,
//      but then the runtime finds it by itself);
//   3. the runtime's usual search (the application's folder, a NuGet package's native assets).
internal static class NativeLibraryResolver
{
    private static readonly Lock s_lock = new();
    private static nint s_explicit;
    private static bool s_resolved;

    // From NativeMethods' static constructor: before its first call into the library.
    internal static void Register() =>
        NativeLibrary.SetDllImportResolver(typeof(NativeLibraryResolver).Assembly, Resolve);

    internal static void LoadExplicit(string path)
    {
        lock (s_lock)
        {
            if (s_resolved || s_explicit != 0) throw new InvalidOperationException("Brack's native library is already loaded");
            s_explicit = NativeLibrary.Load(path);
        }
    }

    private static nint Resolve(string name, Assembly assembly, DllImportSearchPath? searchPath)
    {
        if (name != NativeMethods.Library) return 0;
        lock (s_lock)
        {
            s_resolved = true;
            if (s_explicit != 0) return s_explicit;
        }
        (string os, string file)? system = OperatingSystem.IsWindows() ? ("win", "brack.dll")
                                         : OperatingSystem.IsLinux() ? ("linux", "libbrack.so")
                                         : OperatingSystem.IsMacOS() ? ("osx", "libbrack.dylib")
                                         : null;
        string? arch = RuntimeInformation.ProcessArchitecture switch
        {
            Architecture.X64 => "x64",
            Architecture.X86 => "x86",
            Architecture.Arm64 => "arm64",
            _ => null,
        };
        if (system is { } s && arch != null)
        {
            string candidate = Path.Combine(AppContext.BaseDirectory, "runtimes", $"{s.os}-{arch}", "native", s.file);
            if (File.Exists(candidate) && NativeLibrary.TryLoad(candidate, out nint handle)) return handle;
        }
        return 0;
    }
}
