using System.Runtime.InteropServices;
using Brack;

/// <summary>
/// An ALSA sequencer port of this program's, which Brack opens as a MIDI input by name. The
/// sequencer stamps a message when it delivers it, on the queue of Brack's port, so a message sent
/// at once (<see cref="SendNow"/>) carries that moment and none sent is ever without a stamp:
/// <see cref="SendUnstamped"/> is the same send. A message for a later moment is scheduled on a
/// queue of this program's, which the sequencer delivers at that moment (to a tick of the queue's
/// timer, <see cref="TicksPerSecond"/>), as CoreMIDI does with a future time stamp.
/// </summary>
sealed class AlsaSeqSource : IMidiSource
{
    private const string Alsa = "libasound.so.2";
    private const uint TicksPerSecond = 6250;  // the most the sequencer allows
    private const int EventBytes = 28;  // snd_seq_event_t
    private const byte QueueDirect = 253, AddressSubscribers = 254, AddressUnknown = 253;
    private const byte TimeStampReal = 1, EventStart = 30, EventStop = 31;
    private readonly nint _seq, _encoder;
    private readonly int _port, _queue;
    private readonly long _queueLessBrackNs;  // the queue's time (from 0 when started) less Brack's clock

    public AlsaSeqSource(string name)
    {
        Name = name;
        if (snd_seq_open(out _seq, "default", 3, 0) < 0) throw new InvalidOperationException("no ALSA sequencer");
        snd_seq_set_client_name(_seq, "midi-timing");
        _port = snd_seq_create_simple_port(_seq, name, 1 | 32, (1 << 1) | (1 << 20));  // READ | SUBS_READ, MIDI_GENERIC | APPLICATION
        _queue = snd_seq_alloc_named_queue(_seq, "midi-timing");
        if (_port < 0 || _queue < 0 || snd_midi_event_new(16, out _encoder) < 0)
            throw new InvalidOperationException("the ALSA sequencer would not make the port");
        nint timer = Marshal.AllocHGlobal((int)snd_seq_queue_timer_sizeof());
        snd_seq_get_queue_timer(_seq, _queue, timer);
        snd_seq_queue_timer_set_resolution(timer, TicksPerSecond);
        snd_seq_set_queue_timer(_seq, _queue, timer);
        Marshal.FreeHGlobal(timer);
        snd_seq_control_queue(_seq, _queue, EventStart, 0, 0);
        snd_seq_drain_output(_seq);
        // The queue's clock is an hrtimer tick plus the time since it; a late tick reads early
        // until the next, so the least of samples over several ticks is the offset.
        _queueLessBrackNs = long.MaxValue;
        for (int i = 0; i < 50; i++)
        {
            long before = BrackLibrary.NowNs, queue = QueueNs(), after = BrackLibrary.NowNs;
            _queueLessBrackNs = Math.Min(_queueLessBrackNs, queue - (before + after) / 2);
            Thread.Sleep(TimeSpan.FromMicroseconds(200));
        }
    }

    public string Name { get; }
    public double? ClockLessBrackUs => (QueueNs() - _queueLessBrackNs - BrackLibrary.NowNs) / 1e3;

    public void SendNow(byte[] message) => Send(message, null);
    public void SendAt(byte[] message, long ns) => Send(message, ns + _queueLessBrackNs);
    public void SendUnstamped(byte[] message) => Send(message, null);

    private long QueueNs()
    {
        nint status = Marshal.AllocHGlobal((int)snd_seq_queue_status_sizeof());
        snd_seq_get_queue_status(_seq, _queue, status);
        nint time = snd_seq_queue_status_get_real_time(status);  // { uint tv_sec; uint tv_nsec; }
        long ns = (long)(uint)Marshal.ReadInt32(time) * 1_000_000_000 + (uint)Marshal.ReadInt32(time, 4);
        Marshal.FreeHGlobal(status);
        return ns;
    }

    /// <summary>One message to the port's subscribers, at once, or at <paramref name="queueNs"/> on the queue.</summary>
    private unsafe void Send(byte[] message, long? queueNs)
    {
        byte* ev = stackalloc byte[EventBytes];
        new Span<byte>(ev, EventBytes).Clear();
        snd_midi_event_reset_encode(_encoder);
        fixed (byte* bytes = message)
            if (snd_midi_event_encode(_encoder, bytes, message.Length, ev) != message.Length)
                throw new InvalidOperationException("not one whole MIDI message");
        ev[13] = (byte)_port;  // source.port; dest: the subscribers
        ev[14] = AddressSubscribers;
        ev[15] = AddressUnknown;
        if (queueNs is long at)
        {
            ev[1] |= TimeStampReal;  // flags: real time, absolute
            ev[3] = (byte)_queue;
            *(uint*)(ev + 4) = (uint)(at / 1_000_000_000);
            *(uint*)(ev + 8) = (uint)(at % 1_000_000_000);
        }
        else
        {
            ev[3] = QueueDirect;
        }
        if (snd_seq_event_output_direct(_seq, ev) < 0) throw new InvalidOperationException("the ALSA sequencer would not send");
    }

    public void Dispose()
    {
        snd_seq_control_queue(_seq, _queue, EventStop, 0, 0);
        snd_seq_drain_output(_seq);
        snd_seq_free_queue(_seq, _queue);
        snd_midi_event_free(_encoder);
        snd_seq_close(_seq);
    }

    [DllImport(Alsa)] private static extern int snd_seq_open(out nint seq, string name, int streams, int mode);
    [DllImport(Alsa)] private static extern int snd_seq_close(nint seq);
    [DllImport(Alsa)] private static extern int snd_seq_set_client_name(nint seq, string name);
    [DllImport(Alsa)] private static extern int snd_seq_create_simple_port(nint seq, string name, uint caps, uint type);
    [DllImport(Alsa)] private static extern int snd_seq_alloc_named_queue(nint seq, string name);
    [DllImport(Alsa)] private static extern int snd_seq_free_queue(nint seq, int queue);
    [DllImport(Alsa)] private static extern int snd_seq_control_queue(nint seq, int queue, int type, int value, nint ev);
    [DllImport(Alsa)] private static extern int snd_seq_drain_output(nint seq);
    [DllImport(Alsa)] private static extern nuint snd_seq_queue_timer_sizeof();
    [DllImport(Alsa)] private static extern int snd_seq_get_queue_timer(nint seq, int queue, nint timer);
    [DllImport(Alsa)] private static extern int snd_seq_set_queue_timer(nint seq, int queue, nint timer);
    [DllImport(Alsa)] private static extern void snd_seq_queue_timer_set_resolution(nint timer, uint resolution);
    [DllImport(Alsa)] private static extern nuint snd_seq_queue_status_sizeof();
    [DllImport(Alsa)] private static extern int snd_seq_get_queue_status(nint seq, int queue, nint status);
    [DllImport(Alsa)] private static extern nint snd_seq_queue_status_get_real_time(nint status);
    [DllImport(Alsa)] private static extern int snd_midi_event_new(nuint bufferSize, out nint encoder);
    [DllImport(Alsa)] private static extern void snd_midi_event_free(nint encoder);
    [DllImport(Alsa)] private static extern void snd_midi_event_reset_encode(nint encoder);
    [DllImport(Alsa)] private static extern unsafe nint snd_midi_event_encode(nint encoder, byte* bytes, nint count, byte* ev);
    [DllImport(Alsa)] private static extern unsafe int snd_seq_event_output_direct(nint seq, byte* ev);
}
