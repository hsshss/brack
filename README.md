# Brack

English | [日本語](README.ja.md)

A simple host for playing many CLAP, VST3 and VST2 software instruments at once. You choose freely which MIDI inputs go to which instruments, and which speaker channels each instrument's outputs go to.

## Features

- **MIDI passed through unchanged** — Brack hands the MIDI it receives to the instruments without converting it (except to VST3, whose format requires conversion).
- **Free routing** — Connect MIDI inputs to instruments, and instrument outputs to output channels, many to many.
- **A sample rate of your choice for the instruments** — Run the instruments at any rate (96 kHz, for example), and Brack converts their output to the output device's rate at high quality.
- **Virtual MIDI ports** — Create ports through which other applications send MIDI to Brack's instruments. Even on Windows, you need no loopMIDI or similar.
- **Keeps playing when an instrument crashes** — Each instrument runs in a process of its own, so when one crashes or hangs, Brack and the other instruments keep playing.
- **Instruments of other architectures** — The 64-bit Brack runs 32-bit instruments, and the ARM Brack runs x64 instruments.
- **Three forms** — Use Brack as a GUI, as a command line (CLI), or as a library inside other applications.

## Requirements

- Windows (x64, x86, ARM64). Virtual MIDI ports need Windows 11.
- macOS 14.4 or later (Apple silicon, Intel)
- Linux (x64, x86, ARM64). The release runs on glibc 2.28 or later (RHEL 8, Debian 10, Ubuntu 20.04 or later, and others). The GUI runs on X11 (through Xwayland on a Wayland desktop).

Which architectures' instruments each Brack can use is in [the details](docs/details.md#plugins-of-other-architectures).

## Installation

The releases are on the GitHub releases page, one per OS and architecture, named `brack-<version>-<OS>-<architecture>`.

- **Windows** — Unpack `brack-<version>-win-x64.zip` (`win-x86` for 32-bit Windows, `win-arm64` for ARM) anywhere you like, and run `brack.exe` in `bin`.
  - Brack is not signed, so the first run shows "Windows protected your PC". Click **More info**, then **Run anyway**. To skip this, check **Unblock** in the zip file's properties before you unpack it.
- **macOS** — Unpack `brack-<version>-osx-universal.zip` (it runs on both Apple silicon and Intel Macs) and move `Brack.app` to the Applications folder.
  - The first time you open it, macOS says it cannot be opened. Open **System Settings → Privacy & Security** and click **Open Anyway** near the bottom. After that it opens normally.
- **Linux** — Unpack `brack-<version>-linux-x64.tar.gz` (`linux-arm64` for ARM. There is no x86 release, so [build it from source](docs/development.md#linux)) anywhere you like, and run `bin/brack`. Do not rearrange the files inside the unpacked folder. The GUI needs the X11 and OpenGL libraries (`libX11.so.6`, `libGL.so.1`). What MIDI ports and plugin editors need is in [the details](docs/details.md#release-contents).

To uninstall Brack, delete the unpacked folder (`Brack.app` on macOS). The settings stay in the place listed under [Saved settings](#saved-settings). Delete them too if you no longer need them.

What the release contains is in [the details](docs/details.md#release-contents), and how to build from source is in [docs/development.md](docs/development.md).

## Usage

### GUI

The menu on the left switches between **Rack**, **Routing**, **Audio settings** and **Log**.

1. In **Rack**, add instruments with "Add instrument". The list shows the instruments found on the computer, and you can filter it by name or vendor. To load one that is not in the list, or an effect, choose its file with "From file...".
2. Under MIDI INPUTS at the bottom of **Rack**, add MIDI inputs (ports of connected devices, or virtual ports that Brack creates).
3. In the **Routing** table, check which inputs go to which instruments. Below it, choose which output channels each instrument output goes to. A new instrument starts connected to outputs 1 and 2.
4. Set the volume of each output channel with the faders in the instrument's panel in **Rack** (double-click for 0 dB).
5. Change the output device and the sample rate in **Audio settings**. Changes take effect when you click "Apply".

- The "Editor" button opens an instrument's own window.
- The "MASTER" fader at the top sets the overall volume (all the way left mutes, double-click for 0 dB).
- To rename an instrument, double-click its name or use "Rename" in the "..." menu. Drag instruments and MIDI inputs to reorder them.
- "Copy all" in **Log** copies the log to the clipboard.

### Saved settings

The GUI saves the rack you built (the instruments and their state, MIDI inputs, routing and audio settings), the window position and similar settings to `settings.json` in the folder below, and restores them at the next start.

| OS | Folder |
|---|---|
| Windows | `%APPDATA%\brack\` |
| macOS | `~/Library/Application Support/brack/` |
| Linux | `~/.config/brack/` (or `XDG_CONFIG_HOME` when it is set) |

- Save also writes the rack to a session file (JSON), and Open opens one. If you give a session file at startup (`brack session.json`), Brack opens it instead of the saved rack.
- If you start Brack with `--config <folder>`, it saves its settings to that folder and reads them from there, instead of the place above. Use it when you want to leave your usual settings alone, for example while you try something out.

The detailed saving rules, and how to take a rack to another OS, are in [the details](docs/details.md#saved-settings).

### When an instrument crashes

A crashed instrument shows "Crashed" and goes silent, and Brack and the other instruments keep playing. The Log shows where it crashed. Click "Reload" to load it again from its state before the crash.

### Virtual MIDI ports

When you add a virtual port under MIDI INPUTS in Rack, other applications (MIDI players and others) see a MIDI output with that name. The MIDI they send to it reaches Brack's instruments.

> **Known issue on Windows:** Windows MIDI Services has a bug where deleting a virtual port stops the MIDI service, and no application can open a MIDI port until you restart Windows (microsoft/MIDI #1047. A Windows update in late November 2026 is expected to fix it). Brack deletes its virtual ports when it quits too, so the same thing happens then. On an affected Windows, Brack shows a warning where you add virtual ports. The 32-bit Brack cannot use virtual ports.

### When the sound drops out on Linux

Brack runs its audio at real-time priority. If you start it from the desktop, you normally need to do nothing. When you start it over SSH, or run many instruments, it may not get real-time priority. The settings for that case are in [the details](docs/details.md#real-time-priority-linux).

### CLI

```bat
brack-cli devices                     :: list output devices
brack-cli midi-inputs                 :: list MIDI inputs
brack-cli scan                        :: list the plugins found
brack-cli play Synth.clap --virtual "Brack" --rate 96000 --gui --save my.json
brack-cli run my.json                 :: play a session (Ctrl+C to quit)
```

`brack-cli help` lists the options.

### More details

[docs/details.md](docs/details.md) covers:

- The plugin formats Brack uses, and where it looks for plugins
- What is protected when an instrument crashes, and the setting that runs instruments in Brack's own process
- The quality and latency of sample rate conversion
- What happens when you unplug the output device, and when the default device changes
- When MIDI is not passed through unchanged (VST3 and others), and how each OS handles MIDI ports

## Embedding in applications

Other applications can use Brack as a library. Through the library, an application sends MIDI straight in through the API, without a MIDI port. Everything the GUI does to a rack, the library can do too.

- **C API.** `brack.dll` (`libbrack.so` on Linux, `libbrack.dylib` on macOS) and the header [`include/brack/brack.h`](include/brack/brack.h). How to use it, how it handles threads, and which plugin hosts to ship with it are in the comments at the top of the header and on each function.
- **.NET binding (.NET 10).** [`bindings/dotnet/Brack`](bindings/dotnet/Brack). It puts C#-style classes (`BrackEngine`, `BrackLibrary`) on top of the C API. Use it as a NuGet package (`dotnet pack bindings/dotnet/Brack`) or as a project reference. Both carry the built native libraries and plugin hosts for each OS and architecture, and load the ones that match the running OS and architecture. How to use it is in the class comments, and what goes into the package is in the comments of `Brack.csproj`.

## For developers

Build details (per-architecture builds, options, tests and tools) and the source layout are in [docs/development.md](docs/development.md). Design decisions and investigation results are in [docs/design-notes.md](docs/design-notes.md) (in Japanese).

## License

Brack is released under the MIT License ([LICENSE](LICENSE)).

The licenses of the third-party software Brack uses are in [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md). The release contains the same file.

VST is a trademark of Steinberg Media Technologies GmbH.
