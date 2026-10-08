namespace Brack;

/// <summary>The BRACK_ERR_* codes of the C API.</summary>
public enum BrackErrorCode
{
    Failed = -1,
    InvalidArgument = -2,
    NotFound = -3,
    BufferTooSmall = -4,
    QueueFull = -5,
}

/// <summary>A call into brack.dll failed; <see cref="Exception.Message"/> is the library's own description.</summary>
public sealed class BrackException(BrackErrorCode code, string message) : Exception(message)
{
    public BrackErrorCode Code { get; } = code;
}
