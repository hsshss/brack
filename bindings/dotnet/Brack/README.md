# Brack.NET

.NET binding of [Brack](https://github.com/hsshss/brack), a host for CLAP, VST3 and VST2 instruments.
Load instruments, send them MIDI, and play their audio through an output device or render it into your own
buffers.

- Each plugin runs in a plugin host process of its own, so one that crashes or hangs does not take your
  application down. Plugins built for another architecture (32-bit x86 on 64-bit Windows, x64 on Arm)
  run the same way where the system can run them (Rosetta 2 on a Mac, FEX-Emu or qemu-user on Linux).
- MIDI goes to the plugins as it is (VST3 excepted, whose format needs a conversion), from your code, from
  MIDI input ports, or from virtual ports Brack publishes for other applications.
- Plugins can run at their own sample rate, resampled to the device's.

## Requirements

- .NET 10.
- Windows (x64, x86, Arm64), macOS 14.4 or later (Apple silicon, Intel), or Linux (x64, Arm64) with glibc
  2.28 or later.

The package carries the native library and its plugin hosts for each of these, and loads the one for the
system and architecture your application runs on.

## Getting started

```shell
dotnet add package Brack.NET
```

```csharp
using Brack;

using var engine = new BrackEngine();
string synth = engine.AddPlugin("/path/to/Synth.clap");  // its first plugin, routed to outputs 1 and 2
engine.Start();                                          // the default audio output

engine.SendMidi(synth, 0, [0x90, 60, 100]);              // note on, from any thread
Thread.Sleep(1000);
engine.SendMidi(synth, 0, [0x80, 60, 0]);                // note off
```

To render the audio yourself rather than play it, call `StartManual(sampleRate, channels, maxFrames)` instead
of `Start()`, then `Render(outputs, frames)` from your audio callback. `BrackLibrary.ScanPlugins()` lists the
installed plugins, `BrackLibrary.ListAudioDevices()` and `BrackLibrary.ListMidiInputs()` what the system offers, and
`SaveSession()` / `LoadSession()` keep a whole rack in a file.

## Threads

Every member of `BrackEngine` is thread safe. Changes run on the engine's own thread, which also hosts the
plugin editors, so your application needs no message loop. The members meant for audio and MIDI threads
(`Render`, the `SendMidi` family) do not allocate. The `SendMidi` family reports a failure in its result rather
than throwing, and `Render` throws only for arrays shorter than the frames asked for.

On macOS, that thread is the process's main thread, whose run loop the application keeps running. A GUI
application does so anyway. A console program does its work on another thread and calls CoreFoundation's
`CFRunLoopRun()` on the main one.

## Linux

The native library needs only glibc. It loads `libasound.so.2` for MIDI ports and `libX11.so.6` for
plugin editors when they are first used, and without them only those are unavailable.

## More

- [Brack's README](https://github.com/hsshss/brack#readme): what Brack does, and its GUI and command line.
- [`brack.h`](https://github.com/hsshss/brack/blob/main/include/brack/brack.h): the C API this binding wraps,
  with its contracts in detail. The binding's own classes are documented in their XML comments.

Brack is released under the MIT License. The libraries it includes are listed with their licenses in
`THIRD_PARTY_NOTICES.md`, which is in the package.
