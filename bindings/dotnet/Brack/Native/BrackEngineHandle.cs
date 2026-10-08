using Microsoft.Win32.SafeHandles;

namespace Brack.Native;

// Owns a brack_engine*: destroys it when released, also from the finalizer of an engine that
// was never disposed.
internal sealed class BrackEngineHandle : SafeHandleZeroOrMinusOneIsInvalid
{
    public BrackEngineHandle() : base(ownsHandle: true) { }

    protected override bool ReleaseHandle()
    {
        NativeMethods.brack_engine_destroy(handle);
        return true;
    }
}
