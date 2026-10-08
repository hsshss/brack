using System.Runtime.InteropServices;
using System.Text;

namespace Brack.Native;

// Calling conventions of the C API shared by the wrappers: error codes, and text or bytes
// written into a caller buffer that reports the size it needs.
internal static unsafe class Interop
{
    internal delegate int BufferCall(byte* buf, nuint size, nuint* needed);

    // Throws for a negative result, with brack_last_error() of this thread (read at once: the
    // next call on the thread replaces it).
    internal static void Check(int result)
    {
        if (result < 0) throw LastError(result);
    }

    internal static BrackException LastError(int result) =>
        new((BrackErrorCode)result, Marshal.PtrToStringUTF8((nint)NativeMethods.brack_last_error()) ?? "");

    internal static string? Utf8(byte* text) => text == null ? null : Marshal.PtrToStringUTF8((nint)text);

    internal static string Utf8(ReadOnlySpan<byte> buffer)
    {
        int end = buffer.IndexOf((byte)0);
        return Encoding.UTF8.GetString(end < 0 ? buffer : buffer[..end]);
    }

    // Text written by `call` (UTF-8, NUL terminated). Tries `initialSize` first and grows to
    // what the call asks for, so an expensive call (a scan) usually runs once.
    internal static string ReadText(BufferCall call, int initialSize = 16 * 1024)
    {
        byte[] bytes = ReadBytes(call, initialSize, out int length);
        return Encoding.UTF8.GetString(bytes, 0, Math.Max(length - 1, 0));
    }

    internal static byte[] ReadBytes(BufferCall call, int initialSize = 4096)
    {
        byte[] bytes = ReadBytes(call, initialSize, out int length);
        return length == bytes.Length ? bytes : bytes.AsSpan(0, length).ToArray();
    }

    private static byte[] ReadBytes(BufferCall call, int size, out int length)
    {
        while (true)
        {
            byte[] buffer = new byte[Math.Max(size, 1)];
            nuint needed = 0;
            int result;
            fixed (byte* p = buffer) result = call(p, (nuint)buffer.Length, &needed);
            if (result == NativeMethods.BRACK_OK)
            {
                length = (int)needed;
                return buffer;
            }
            if (result != NativeMethods.BRACK_ERR_BUFFER_TOO_SMALL) throw LastError(result);
            size = checked((int)needed);
        }
    }
}
