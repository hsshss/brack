# Brack in detail

English | [日本語](details.ja.md)

This document adds to the [README](../README.md). It covers the details of the requirements, the release contents, the rules for saved settings, and how Brack handles plugins, audio and MIDI. How to build Brack is in [development.md](development.md), and the design decisions are in [design-notes.md](design-notes.md) (in Japanese).

## Requirements

| OS | Architectures | Audio | MIDI |
|---|---|---|---|
| Windows | x64, x86, ARM64 | WASAPI (shared mode, exclusive mode) | MIDI input ports (WinMM), virtual ports (Windows MIDI Services on Windows 11) |
| Linux | x64, x86, ARM64 | PulseAudio (also on PipeWire), ALSA, JACK | ALSA sequencer |
| macOS 14.4 or later | Apple silicon, Intel (x64) | Core Audio | CoreMIDI |

- On Linux, the GUI and the plugin editors run on X11 (through Xwayland on a Wayland desktop).
- The Intel macOS build has been tried under Rosetta 2 on an Apple silicon Mac. It has not been tried on an Intel Mac yet.

### Plugins of other architectures

The Brack of each architecture runs these plugins.

| Brack | Plugins it runs |
|---|---|
| Windows x64, x86 | x86, x64 |
| Windows ARM64 | x86, x64 (through Windows' emulation), ARM64 |
| macOS Apple silicon | ARM64, x64 (when Rosetta 2 is installed) |
| macOS Intel | x64 |
| Linux x64 | x64, x86 (when the 32-bit C library is present) |
| Linux x86 | x86 |
| Linux ARM64 | ARM64, x64 and x86 (when FEX-Emu or qemu-user is present) |

- If you load a plugin of an architecture the computer cannot run, Brack reports it, for example "this computer does not run x64 programs". A scan skips such plugins silently.
- The Linux ARM64 build runs x64 and x86 plugins with FEX-Emu registered with the kernel (through binfmt_misc). Without the registration, it starts them with `FEX` (formerly `FEXInterpreter`) from the PATH. It also uses the qemu-user registrations (`qemu-x86_64`, `qemu-i386`) when the loader for x64 or x86 programs is under `/` or `QEMU_LD_PREFIX`.
- FEX-Emu reads its settings from `$XDG_CONFIG_HOME/fex-emu`. If you start Brack with a different `XDG_CONFIG_HOME`, x64 and x86 plugins disappear from the list (a link to `fex-emu` in that folder brings them back).
- A plugin of another architecture runs in the plugin host of that architecture. The release contains the plugin hosts in the table above (to build from source, see "Bundled plugin hosts" in [development.md](development.md#bundled-plugin-hosts)).

## Release contents

- **Windows** — `bin` contains the GUI (`brack.exe`), the CLI (`brack-cli.exe`), the library (`brack.dll`), and the plugin hosts that run plugins of other architectures. `include` and `lib` contain the header and the import library for embedding Brack in applications.
- **macOS** — `Brack.app` carries the plugin hosts inside itself (`Contents/MacOS`). The CLI and the library (`libbrack.dylib`) are in `bin` and `lib` of the same zip.
- **Linux** — `bin` contains the GUI (`brack`) and the CLI (`brack-cli`), `lib` the library (`libbrack.so`), and `libexec/brack` the plugin hosts. The programs and `libbrack.so` look for the plugin hosts next to themselves and in `../libexec/brack`.
  - It runs on Linux with glibc 2.28 or later (not on musl Linux). The C++ runtime library is linked statically, so it is not needed. The GUI needs `libX11.so.6` and `libGL.so.1`.
  - Brack loads `libasound.so.2` and `libX11.so.6` when it uses them. So the library (`libbrack.so`) and the CLI run on servers and containers without them. Without `libasound.so.2`, MIDI ports ("Linux" under "MIDI ports") are unavailable. Without `libX11.so.6`, plugin editors are unavailable.
  - The x86 plugin host of the x64 build needs the 32-bit C library (on Ubuntu, `libc6:i386` after `sudo dpkg --add-architecture i386`). The editors of x86 plugins also need the 32-bit `libX11.so.6` (`libx11-6:i386`).
  - The x64 and x86 plugin hosts of the ARM64 build are the same as the x64 build's. The editors of x64 and x86 plugins open only when the x86 system that FEX-Emu or qemu-user uses has libX11 for that architecture (otherwise "plugin editors are not available without libX11.so.6").
  - `share/icons` and `share/applications/brack.desktop` put Brack in the desktop menu when you unpack the release somewhere that puts `bin/brack` on the `PATH`, such as `/usr/local`.
- The license (`LICENSE`) and the licenses of the included libraries (`THIRD_PARTY_NOTICES.md`) are at the top of the unpacked folder on Windows and macOS, and in `share/doc/brack` on Linux.

## Concepts

| Element | Description |
|---|---|
| Plugin | A CLAP, VST3 or VST2 instance. An ID (for example `lead`) identifies it. It has note input ports and audio output ports. The file extension decides the format (`.clap`, `.vst3`, and for VST2 `.dll` on Windows, `.so` on Linux and `.vst` on macOS). |
| MIDI source | `hardware` (opens an existing MIDI input port by name), `virtual` (a virtual port that Brack publishes), `api` (input only from the DLL or API) |
| MIDI route | Connects a source to a plugin's note port. Connections can be many to many. |
| Audio route | Connects a channel of a plugin's output port to an output channel, with a gain. |
| Master volume | A gain applied to all output channels after sample rate conversion (SRC). |
| Session | Saves all of the above, the plugins' state (base64) and the audio settings to a JSON file. |

## GUI

- Renaming an instrument changes only its display name. The ID that routing, sessions and the API use stays the same. An empty name returns to the plugin's own name.
- The order of instruments and MIDI inputs is only for display and sessions. It does not affect routing or processing.
- On macOS, the exclusive mode in Audio settings is ignored.

The "Add instrument" list shows only the instruments that the scan found, each marked with its format (CLAP, VST3, VST2). Effects are not listed.

- "From file..." lets you choose a `.clap`, `.vst3` or VST2 file directly. It loads effects too.
- A VST3 bundle is a folder. If you choose the file inside it (`Contents\x86_64-win\*.vst3`, or `Contents/x86_64-linux/*.so` on Linux), Brack loads the bundle. On macOS, you can choose the bundle itself.
- Plugins of an architecture other than Brack's are marked with their architecture (`x86` and so on) in both the list and the rack.

On Linux, the file dialog opens through the desktop portal (xdg-desktop-portal). It works inside sandboxes such as Flatpak. Without the portal, Brack uses zenity (GNOME), and without that, kdialog (KDE). If none is present, the dialog does not open, and the Log says so. It opens in the folder where you last chose a file of the same kind (or your home folder the first time).

## CLI

```bat
brack-cli devices                     :: list output devices
brack-cli midi-inputs                 :: list MIDI inputs
brack-cli scan [--dir D]              :: list plugins (each format's standard paths and environment variable, and D)
brack-cli virtual-check               :: whether virtual MIDI ports can be published
brack-cli play Synth.clap --virtual "Brack" --rate 96000 --gui --save my.json
brack-cli play "C:\Program Files\Common Files\VST3\Synth.vst3" --plugin-id 0123456789ABCDEF0123456789ABCDEF
brack-cli run my.json                 :: run a session (Ctrl+C to quit)
```

- The audio options (`--device`, `--exclusive`, `--buffer`, `--rate`, `--block`, `--quality`, `--in-process`, `--load-serially`, `--duration`) are shared by `run` and `play`. `brack-cli help` lists them.
- `--gui` on `play` opens the plugin's editor. On Linux it needs `DISPLAY`.
- `scan` marks plugins of an architecture other than Brack's with their architecture.

## Saved settings

Brack saves the window position, size and maximized state, the extra plugin folders, the path of the last opened or saved session file, and the state of the rack. The state of the rack includes the plugins and their state, the MIDI inputs, the routing and the audio settings.

- If you give a session file at startup, Brack opens that file instead of the saved rack.
- If Brack cannot read the saved rack, it does not overwrite that rack. It does not save the rack built in that run, and saves only the window position and similar settings.
- The folder given to `--config` also holds the plugin scan cache (`plugin-cache.json`). If the folder is unusable, Brack does not fall back to the usual settings folder and does not start. This keeps it from overwriting the usual settings.
- Brack restores the session file's path only when it restores the rack. When the restore fails, it starts as "untitled". So Save never overwrites the previous session file with an empty rack.
- Brack saves when it quits, and also automatically when 3 seconds pass after a change with no other change. Even while changes keep coming, it saves 30 seconds after the first unsaved change. If Brack is killed, you lose only the last few seconds (at most 30). MIDI that plugins receive does not count as a change.
- Brack sets a broken settings file aside with `.broken` added to its name, and ignores it.
- The builds of every architecture use the same file. A rack saved by the 32-bit build opens as it is in the 64-bit build.
- You can take a rack (a session file, or `session` in `settings.json`) to another OS. The plugins' state holds no paths, so you only change the plugin paths to the ones on that OS.
  - A MIDI input port you used on Windows (loopMIDI and others) can be replaced on Linux and macOS with a virtual port of the same name (`kind` set to `virtual`). Other programs can then send MIDI to it.
  - The window position in `settings.json` and `plugin-cache.json` fit the original computer's screen and paths, so do not take them along.
- Only the GUI saves and loads settings. The CLI and the DLL neither read nor write them.

Brack saves the plugin scan results to `plugin-cache.json` in the same folder. The next scan uses these results, without loading the files whose size and modification time have not changed. Brack does not save files that failed to load, and checks them again every time.

## Plugin formats and search paths

| Format | File | Plugin ID within the file |
|---|---|---|
| CLAP | `.clap` (a bundle folder on macOS) | The CLAP ID (for example `com.vendor.synth`) |
| VST3 | `.vst3` (a bundle folder. On Windows, also a single file) | The class ID (32 hexadecimal digits) |
| VST2 | `.dll` on Windows, `.so` on Linux, `.vst` on macOS (a bundle folder) | The unique ID (4 characters, or `0xXXXXXXXX` when it is not printable) |

The standard search paths are these.

| Format | Windows | Linux | macOS |
|---|---|---|---|
| CLAP | `%CommonProgramFiles%\CLAP`, `%LOCALAPPDATA%\Programs\Common\CLAP` | `~/.clap`, `/usr/lib/clap` | `/Library/Audio/Plug-Ins/CLAP`, `~/Library/Audio/Plug-Ins/CLAP` |
| VST3 | `%CommonProgramFiles%\VST3`, `%LOCALAPPDATA%\Programs\Common\VST3` | `~/.vst3`, `/usr/lib/vst3` | `/Library/Audio/Plug-Ins/VST3`, `~/Library/Audio/Plug-Ins/VST3` |
| VST2 | `VSTPluginsPath` in the registry (`HKLM\SOFTWARE\VST`, `HKCU\SOFTWARE\VST`), `%ProgramFiles%\VSTPlugins`, `%ProgramFiles%\Steinberg\VSTPlugins`, `%CommonProgramFiles%\VST2`, `%CommonProgramFiles%\Steinberg\VST2` | `~/.vst`, `/usr/lib/vst` | `/Library/Audio/Plug-Ins/VST`, `~/Library/Audio/Plug-Ins/VST` |

- On every OS, Brack also searches the folders listed in the environment variables `CLAP_PATH`, `VST3_PATH` and `VST_PATH` (separated by `;` on Windows, `:` on Linux and macOS).
- On Windows, Brack searches both the 64-bit (`C:\Program Files`) and the 32-bit (`C:\Program Files (x86)`) `%ProgramFiles%` and `%CommonProgramFiles%`. It also reads both the 64-bit and the 32-bit (`WOW6432Node`) sides of the `HKLM` registry. The builds of every architecture search the same places.
- VST2 shell plugins (several plugins in one DLL) are supported. For the state, Brack saves the bank chunk if the plugin supports chunks, and otherwise the current program number and all parameters.
- In VST3, Brack passes the parameters changed in the editor to the processor, and the processor's changes (MIDI CC and others) to the editor.
- Brack has no transport. VST3 plugins receive stopped, 120 BPM, 4/4 as the playback information. Brack does not answer VST2 time-info queries.

## Plugin crashes

Each plugin runs in a process of its own, separate from Brack (the plugin host `brack-host-<architecture>`). Whatever way a plugin crashes, and even if it hangs, only that process ends. Brack and the other plugins keep running.

- A crashed plugin shows "Crashed", goes silent, and its editor closes. The Log records in which call, and where in which module, it crashed (for example `crashed in process: access violation (0xC0000005) at Plugin.dll+0x1234`, or `stopped responding in process` when it hangs).
- Brack gives up on a call that does not return after 2 seconds for audio processing, 60 seconds for saving or loading state and for creation, 30 seconds for activation and opening the editor, and 10 seconds for anything else.
- The session gets the last state Brack could save from the plugin. "Reload" in the GUI (`brack_reload_plugin` in the DLL) loads the plugin again from that state in a new process. Its ID, name and routing stay the same.
- When Brack ends, all plugin hosts end too, including when Brack is killed.
- Opening a session loads and activates all plugins at once. A slow plugin does not hold up the others.
  - Some plugins fail when two of them load at the same time. With SOUND Canvas VA, the second shows a "Parameter file1 read error" dialog and waits, or crashes while taking its state. "Load plugins one at a time" (a setting in the GUI, `--load-serially` in the CLI, `load_plugins_serially` in the DLL) then loads and activates them one after another, at the cost of opening more slowly.
- Each plugin adds a process, so loading takes longer by the time a process takes to start. Audio passes between the processes block by block, and the plugins run in parallel.

"Run plugins inside Brack" (a setting in the GUI, `--in-process` in the CLI, `plugins_in_process` in the DLL) runs plugins in Brack's own process (except plugins of other architectures). Loading is faster, but the containment covers only the following.

- Brack catches a crash during a call from Brack (an access violation, a segmentation fault on Linux, and others, including stack overflow), and never calls that plugin again. To keep using it, save the session and restart Brack.
- Brack also contains crashes while it loads a plugin file, lists its contents, or unloads it. Brack then refuses to load that file until it restarts.
- A crash inside a thread or window handling that the plugin created itself, an immediate exit (`abort()` and others), and a call that hangs end Brack as a whole. On Linux, so does a C++ exception that escapes the plugin. To limit the damage, the GUI saves its settings automatically.

## Sample rate conversion

Brack converts with r8brain-free-src (linear phase).

When `processSampleRate` (Audio settings in the GUI, `--rate` in the CLI) is not 0, plugins run at that rate, and the output is converted to the device's rate (for example plugins at 96 kHz, the device at 48 kHz). In WASAPI shared mode, Brack always opens the device at its mix rate, so the OS's resampler is not used.

| Quality | Stopband attenuation | −3 dB point | Typical latency (96→48 kHz / 48→44.1 kHz) |
|---|---|---|---|
| `standard` | about 136 dB | 20.5 kHz | 4.6 ms / 9 ms |
| `high` (default) | about 180 dB | 21 kHz | 4.2 ms / 18 ms |
| `ultra` | about 207 dB | 21.5 kHz | 9 ms / 17 ms |

At every quality, the deviation up to 18 kHz is under 0.01 dB. Conversion by an integer ratio (96→48 kHz and others) has low latency. Conversion between the 44.1 kHz and 48 kHz families has higher latency.

The latency does not follow the order of quality because r8brain applies the filter in FFT blocks. The latency is roughly "a power-of-two block length set by the filter length, minus half the filter length". While a longer filter still fits in the same block length, the latency shrinks a little. When the block length doubles, the latency nearly doubles too.

## Disconnecting and switching output devices

- Processing does not stop when the output device is disconnected. Brack keeps processing in real time on its own clock, and discards the output in the meantime. When the device comes back, sound resumes from that moment. MIDI received while the device was gone does not play all at once either.
- If the output is "the system default", Brack switches to the new default device within about 1 second when the OS's default changes. The same happens when a disconnection moves the default to another device. On Linux with PipeWire, the sound server switches at once, so Brack does not reopen the device.
- If the device is chosen by name, Brack waits for that device to come back, and reopens it then.
- Switching does not stop the plugins. If the new device has a different sample rate, the plugins keep running at their current rate, and Brack's SRC converts to the new rate.

On Windows, Brack puts its audio threads in MMCSS's "Pro Audio" task. On macOS it runs them as time-constraint (real-time) threads. Linux is as described in "[Real-time priority (Linux)](#real-time-priority-linux)" below.

## Audio (Linux)

- Brack tries PulseAudio, ALSA and JACK in that order, and outputs to the first one that opens. On PipeWire, it outputs to PipeWire's PulseAudio-compatible server. The environment variable `BRACK_AUDIO_BACKEND` (`pulseaudio`, `alsa`, `jack`) also chooses one.
- The device runs at the rate of the sound server (PipeWire and others). There is no WASAPI exclusive mode, so Brack ignores the exclusive mode in sessions saved on Windows.

## Real-time priority (Linux)

Brack runs its audio threads (Brack's playback thread and those of the plugin hosts) at real-time priority.

- If there is an rtprio limit, Brack uses `SCHED_FIFO`. Otherwise it asks RealtimeKit for `SCHED_RR`.
- RealtimeKit allows Brack started from the desktop. It refuses Brack started over SSH or run as a service. Over SSH, starting Brack with `systemd-run --user` gets permission (give `systemd-run` the executable's absolute path).
- By default, RealtimeKit makes at most 15 processes per user real-time. The desktop uses this quota too, so on GNOME in Ubuntu 24.04, about 9 plugin hosts got real-time priority. With many plugins, or when RealtimeKit is unavailable, set an rtprio limit.

  ```bash
  echo '@audio - rtprio 95' | sudo tee /etc/security/limits.d/audio.conf
  sudo usermod -aG audio $USER
  ```

  It takes effect after you log in again.
- If Brack cannot get real-time priority, it runs at normal priority and says so in the Log (for example `plugin hosts' audio threads: normal priority (...)`).

## When MIDI is not passed through unchanged

Brack normally passes the MIDI it receives to plugins as bytes (`CLAP_EVENT_MIDI` and `CLAP_EVENT_MIDI_SYSEX` in CLAP, MIDI and SysEx events in VST2). The exceptions are these.

- CLAP: A port that supports the MIDI dialect receives every message as it is. Only when a note port accepts nothing but the CLAP note dialect does Brack replace note on and off with `CLAP_EVENT_NOTE_ON/OFF`. Messages with no matching event in that dialect do not arrive.
- VST2: Every message passes as it is.
- VST3: The format requires these conversions.
  - Note on and off, polyphonic key pressure: VST3 note events
  - SysEx: a data event holding the bytes as they are
  - Control change, pitch bend, channel pressure, program change: changes to the parameters the plugin assigned through `IMidiMapping`. Messages with no assignment do not arrive.
  - System common and real-time messages: do not arrive.

## MIDI ports

### Windows

The MIDI input list shows WinMM's input ports. When an input is disconnected, Brack reconnects once it is back in the list.

Brack creates virtual ports as virtual devices of Windows MIDI Services on Windows 11. Other applications see them as WinMM and WinRT MIDI output ports with the given name. No SDK runtime or driver needs to be installed. Brack turns the messages it receives back into MIDI 1.0 bytes without loss (SysEx is joined into F0..F7, and a note on with velocity 0 is not rewritten either).

- The 32-bit build cannot use virtual ports, because the Windows MIDI Services client is 64-bit only (`brack-cli virtual-check` shows why). MIDI input ports and input from the API work. To send MIDI from a virtual port to a 32-bit plugin, use the 64-bit Brack (it runs 32-bit plugins too).

> **Known issue:** Windows MIDI Services has a bug where deleting a virtual device stops the service (microsoft/MIDI #1047). The fix is expected in a Windows update in late November 2026. Once the service stops, no MIDI port anywhere on the system can be opened or closed until you restart Windows. With two or more virtual ports, deleting the first stops the service, and the remaining ports stay undeleted.
>
> - On an affected Windows, Brack warns in the GUI (where you add virtual ports) and in `brack-cli virtual-check`.
> - When Brack notices the service has stopped, it quits at once without waiting to release the remaining ports. It also fails the creation of new virtual ports at once.

### Linux

Brack uses the ALSA sequencer. Device ports and other programs' ports (virtual keyboards, sequencers and others) appear in the same list. It works as it is on PipeWire.

- The MIDI input list shows the ports that send MIDI, by port name. Only when the name matches a port of another client does Brack add the client's name in parentheses (`Out (VMPK)` and so on). Ports whose client name is also the same (two interfaces of the same model, for example) appear from the second on as "name (2)" and so on. A saved name is found with or without the client's name in parentheses. Brack's own ports do not appear.
- When an input goes away (a device is unplugged, a program ends, or `aconnect -d` disconnects it), Brack reports it. Brack reconnects once it is back in the list.
- A virtual port becomes a port with the given name on a client named "Brack". Other programs see it as a MIDI destination (`aconnect -l`, `aplaymidi -p`, and others). There is no deletion bug as on Windows.
- Without the ALSA library (`libasound.so.2`), the MIDI input list is empty, and neither ports nor virtual ports open ("MIDI ports are not available without libasound.so.2"). MIDI sent from the API works.

### macOS

Brack uses CoreMIDI.

- The MIDI input list shows CoreMIDI sources (devices, the IAC Driver, other programs' virtual sources) by display name. Sources with the same name (two keyboards of the same model, for example) appear from the second on as "name (2)" and so on.
- When an input goes away, Brack reports it. Brack reconnects once it is back in the list.
- A virtual port becomes a CoreMIDI destination with the given name. Other programs see it as a MIDI output.
