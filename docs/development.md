# Brack development notes

English | [日本語](development.ja.md)

This document is for those who work on Brack. It covers the source layout, building, tests and tools, releases, how to write docs and comments, and the status of each platform. How to use Brack is in the [README](../README.md), the technical details for users are in [details.md](details.md), and the design decisions with their reasons and the investigation results are in [design-notes.md](design-notes.md) (in Japanese).

## Layout

```
include/brack/brack.h       public C API
src/core/                   the engine (a static library)
  engine.*                  host thread, routing graph, rendering
  plugin/                   plugin interface shared by the formats, scanning, editor windows,
                            each OS's rules for plugin files (plugin_files.*)
  clap/  vst3/  vst2/       module loading, instances and editors of each format
  audio/                    output (miniaudio), SRC (r8brain)
  midi/                     MIDI input (WinMM), virtual ports (MidiSrv), the ALSA sequencer on Linux (alsa/),
                            CoreMIDI on macOS (coremidi/)
  remote/                   communication with plugin host processes (both sides)
  session.cpp               session JSON
src/host/                   plugin host process (brack-host-<architecture>)
src/dll/  src/cli/  src/gui/
src/gui/icon/               the application icon (SVG, and the files for each OS exported from it)
bindings/dotnet/            .NET binding (Brack) and its tests
tests/                      test synths (CLAP / VST3 / VST2) and tests
tools/vmidi_probe/          virtual port diagnostic tool
tools/midi-timing/          MIDI timing measurement (design notes, chapter 15)
tools/app-icon/             exports the files for each OS from the icon SVG
tools/linux_syntax_check.sh syntax-checks the shared code with Linux g++ for x64, x86 and ARM64 (run in WSL or similar)
tools/arch_matrix.sh        checks the combinations of CPU architectures, per build and per way of running (macOS, Linux, Windows)
tools/arm64ec_synth.ps1     builds the ARM64EC and ARM64X test synths (Windows)
tools/check_release_files.sh checks the files of the release archives and the NuGet package against a list (release)
cmake/toolchains/           toolchains that build Linux x86 and ARM64 from x64 Linux, and x64 from ARM64 Linux
tools/linux_release_build.sh builds the Linux release in a manylinux_2_28 container (release)
tools/linux_smoke_test.sh   starts the Linux release in containers of other distributions (release)
docs/details.md             supplement to the README (technical details for users)
docs/design-notes.md        implementation decisions and investigation results
```

The dependencies are fetched with git at configure time: [CLAP](https://github.com/free-audio/clap), [VST3 pluginterfaces](https://github.com/steinbergmedia/vst3_pluginterfaces), [vst2sdk](https://github.com/Xaymar/vst2sdk), [r8brain-free-src](https://github.com/avaneev/r8brain-free-src), [miniaudio](https://github.com/mackron/miniaudio), [nlohmann/json](https://github.com/nlohmann/json), [Dear ImGui](https://github.com/ocornut/imgui), [GLFW](https://github.com/glfw/glfw). `src/core/vst2/vst2_abi.h` replaces the vst2sdk definitions that disagree with real plugins ([design notes](design-notes.md), chapter 6, in Japanese).

`THIRD_PARTY_NOTICES.md` lists, one by one, the libraries in the release and the text copied from other projects into the source, with their licenses. `cmake --install` and the NuGet package include it. When you add a dependency or upgrade one, update this file to match that library's current license text.

Names that users see are written "Brack". This covers window titles, dialogs, log and error messages, CLI headings, the host name given to plugins (CLAP, VST3, VST2), the descriptions of the Windows MIDI Services session and virtual ports, the default name of virtual ports, `brack_version_string()`, and the running text of the docs. Identifiers stay lowercase (file and folder names, the settings folder, the format IDs `brack-session` and `brack-settings`, the C API, CMake targets and options, and others). The CLI's usage text uses the executable's name (`brack-cli`).

## Build

On every OS, the build and the tests take this form. The outputs land in `build/bin/`.

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build
ctest --test-dir build --output-on-failure
```

To build offline, point to local sources with `-DFETCHCONTENT_SOURCE_DIR_CLAP=...` and the like.

| Option | Default | What it does |
|---|---|---|
| `BRACK_BUILD_GUI` | ON (on Linux, when the X11 and OpenGL development files are present) | The GUI (Dear ImGui + GLFW). Brack.app on macOS |
| `BRACK_BUILD_CLI` | ON | The CLI |
| `BRACK_BUILD_TESTS` | ON | Tests, test plugins (CLAP / VST3 / VST2), diagnostic tools |
| `BRACK_VERSION` | `1.0.0` | The version (`1.2.0`, or `1.2.0-beta.1` for a prerelease). `brack_version_string()` and the CLAP host report it. Brack.app's `Info.plist` gets only the numeric part. A release takes it from the tag ("Releases" below) |
| `BRACK_OTHER_ARCH_BINS` | Those of `build/bin`, `build-x86/bin` and `build-arm64/bin` other than this build's (on macOS `build-x64` instead of `build-x86`. On Linux also `build-x64/bin`, for the x64 build placed next to `build` on an ARM64 machine) | The `bin` folders of the other architectures' builds (separated by `;`). The bundled plugin hosts are copied from there into this build's `bin` |

### Bundled plugin hosts

The Brack of each architecture bundles the plugin hosts (`brack-host-<architecture>`, with `.exe` on Windows) of the architectures it runs. Which architectures those are is in [the table in the details](details.md#plugins-of-other-architectures).

- Each build copies the other architectures' plugin hosts from the `bin` of those architectures' builds, every time it builds. It removes the ones it does not bundle from `bin`. So build x86 before x64 and ARM64, and build x64 too before ARM64. The build warns when one is missing.
- After a rebuild, build the one you built first again (it compiles nothing and only copies). A plugin host from a different build refuses to start and reports it ("the plugin host is from another build of Brack").
- `tools/arch_matrix.sh` checks at once what runs in each combination (macOS, ARM64 and x64 Linux, ARM64 and x64 Windows. [Design notes](design-notes.md), chapter 15, in Japanese).
- The Apple silicon build uses the x64 plugin host when Rosetta 2 is installed. Without it, x64 plugins do not load ("this computer does not run x64 programs"). Even when the universal build starts under Rosetta 2 (as x64), the ARM64 plugin host still runs ARM64 plugins.

### Windows

You need Visual Studio 2022 or later (Desktop development with C++, including C++20, CMake and Ninja).

The x64 and x86 builds bundle each other's plugin hosts, and the ARM64 build bundles the x64 and x86 ones. So build in this order: x86, x64, x86 again (copy only), ARM64 (see also "On Windows" below).

1. In the "x86 Native Tools Command Prompt" (or a command prompt where `vcvars32.bat` has run), build x86 into `build-x86`.

   ```bat
   cmake -S . -B build-x86 -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
   cmake --build build-x86
   ```

2. In the "x64 Native Tools Command Prompt", build x64 into `build` (change `build-x86` above to `build`).
3. In the x86 environment, run `cmake --build build-x86` again (it only copies the x64 plugin host again).
4. Build ARM64 into `build-arm64`. Use the "ARM64 Native Tools Command Prompt", or on an x64 PC a command prompt where `vcvarsamd64_arm64.bat` has run (this needs the ARM64 compiler that runs on x64, Visual Studio's `VC.Tools.ARM64`). Run the tests on an ARM64 PC.

- `build\bin` gets `brack.exe` (the GUI), `brack-cli.exe`, `brack.dll` and the bundled plugin hosts. `cmake --install` puts these in `bin`, `brack.h` in `include` and `brack.lib` in `lib`.
- On an ARM64 PC, you can also build x64 and x86 into `build` and `build-x86` with `vcvarsarm64_amd64.bat` and `vcvarsarm64_x86.bat`. The tests run under Windows' emulation.
- With MSVC, targets of the same name (`brack.exe` and `brack.dll`, which land in the same folder, and `brack-test-synth`, the CLAP and VST3 test synths) try to write the same `.pdb` and `.ilk`. So the linker files are kept apart (the GUI's PDB is `brack-gui.pdb`, and `/INCREMENTAL:NO`), and `add_dependencies` makes them link one after another. The reason is that the linker deletes an old `.ilk` of the same name, so linking them at the same time deletes the other's `.ilk`.

### Linux

You need g++ 13 or later, CMake 3.25 or later, Ninja and git. On Ubuntu 24.04, install these packages.

```bash
sudo apt install build-essential cmake ninja-build git pkg-config libasound2-dev libx11-dev libxrandr-dev libxinerama-dev libxcursor-dev libxi-dev libgl-dev
```

- Without the ALSA development files (`libasound2-dev`), Brack builds without MIDI ports ("MIDI ports are not available on this system yet").
- Without the X11 and OpenGL development files (`libx11-dev` through `libgl-dev`), Brack builds without the GUI. Without the X11 development files (`libx11-dev`), it also builds without plugin editors, and opening an editor gives "plugin editors are not available on this system yet".
- Every program links the C++ runtime library statically (what running needs is in ["Release contents" in the details](details.md#release-contents)).

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build
cmake --install build --prefix ~/.local
```

- `cmake --install` puts `brack` (the GUI) and `brack-cli` in `bin`, `libbrack.so` in `lib`, the plugin hosts in `libexec/brack`, and `brack.h` in `include` (where the plugin hosts are looked for is in ["Release contents" in the details](details.md#release-contents)).
- You can build x86 and ARM64 from x64 Linux with cross compilers (Ubuntu's `g++-i686-linux-gnu` and `g++-aarch64-linux-gnu`). Build x86 first, so that the x64 build bundles the x86 plugin host. To run the x86 build and the x86 plugin host on x64 Linux, you need the 32-bit C library (Ubuntu's `libc6-i386`).
- Cross builds (x86 and ARM64 from x64 Linux, x64 and x86 from ARM64 Linux) have no MIDI ports, plugin editors or GUI, because the ALSA, X11 and OpenGL development files for that architecture are missing. The reason is that the toolchains look for libraries and headers only under `/usr/aarch64-linux-gnu` and the like.
  - When a cross-built plugin host is bundled, the editors of that architecture's plugins do not open either ("plugin editors are not available on this system yet"). MIDI and the GUI belong to Brack itself, so they do not depend on the plugin host.
  - Putting that architecture's development files where the toolchain looks should add them (not tried).
- On ARM64 Linux, `build` gets the ARM64 build. If you first cross-build x64 into `build-x64` and x86 into `build-x86` with `cmake/toolchains/linux-x64.cmake` and `linux-x86.cmake`, the ARM64 build bundles their plugin hosts.
- On x64 Linux, the ARM64 tests run under qemu-user (`QEMU_LD_PREFIX=/usr/aarch64-linux-gnu ctest --test-dir build-arm64`).
- On x64 RHEL-family systems (AlmaLinux and others), you can build x86 with the 32-bit libraries (`glibc-devel.i686` and others) and `cmake/toolchains/linux-x86-m32.cmake`. This is not a cross build, so with the 32-bit X11 and ALSA development files (`libX11-devel.i686`, `alsa-lib-devel.i686`) it gets editors and MIDI ports too.

```bash
cmake -S . -B build-x86 -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo --toolchain cmake/toolchains/linux-x86.cmake && cmake --build build-x86
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo && cmake --build build
cmake -S . -B build-arm64 -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo --toolchain cmake/toolchains/linux-arm64.cmake && cmake --build build-arm64
```

### macOS

You need Xcode (or its command line tools), and `cmake` (3.25 or later) and `ninja` from Homebrew. `build/bin/` gets Brack.app, brack-cli, libbrack.dylib and the plugin hosts.

- `cmake --install` uses the same layout as on Linux (with `libbrack.dylib` in `lib`), and adds `Brack.app` at the top.
- On an Apple silicon Mac, you can cross-build for Intel (x64) into `build-x64`. If you build it first, the Apple silicon build bundles its x64 plugin host.
- You can also make a universal build that combines both. The programs and libbrack.dylib contain both architectures, and there is one plugin host per architecture.

```bash
cmake -S . -B build-x64 -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_OSX_ARCHITECTURES=x86_64 && cmake --build build-x64
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo && cmake --build build && ctest --test-dir build
cmake -S . -B build-universal -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo "-DCMAKE_OSX_ARCHITECTURES=arm64;x86_64" && cmake --build build-universal
```

To sign with the Hardened Runtime for distribution, add `-DBRACK_CODESIGN_IDENTITY="Developer ID Application: ..."` (`-` just to try it). This signs the programs, the plugin hosts, libbrack.dylib and Brack.app with the entitlements plugins need (`cmake/brack.entitlements`: loading plugins from other developers, and JIT). Notarization is a separate step (it needs an Apple developer account, so it has not been tried).

## Tests and tools

- ctest includes tests of the engine, the test synth of each format, crash containment, the plugin host, the DLL, the .NET binding and more ([design notes](design-notes.md), chapter 8, in Japanese).
  - The .NET tests (`bindings/dotnet/Brack.Tests`) are part of ctest in the 64-bit builds when the .NET SDK is present (Windows, Linux, macOS).
  - `test_api_parity` checks that the engine's public methods and the C API match, and that every C API function is in the .NET binding.
  - `test_isolation` also shows the time per block ([design notes](design-notes.md), chapter 15, in Japanese). `test_isolation --block-cost <brack-test-synth.clap> [<frames>]` measures only the block cost. It runs 1, 2, 4 and 8 plugins, and measures both with blocks back to back and with each block processed at its own time as a real device does (paced). Without `<frames>`, it measures both 64 and 256 frames. On Linux and macOS it also shows how many times per block the thread slept (and, on Linux only, how many times it moved to another CPU).
- `test_virtual_midi` and `vmidi_probe` are manual tests that create real virtual ports. They are not in ctest because of the Windows MIDI Services bug (microsoft/MIDI#1047: deleting a virtual port stops the service). `vmidi_probe --check` creates no virtual port, so you can run it at any time.
- `tools/linux_syntax_check.sh` checks that the shared code compiles without warnings. The cross compilers are Ubuntu's `g++-i686-linux-gnu` and `g++-aarch64-linux-gnu`.
  - In WSL, reading files on the Windows side (`/mnt/c`) is slow, so the script copies the source and the dependencies to the Linux side (`~/.cache/brack-syntax-check`) before it compiles. The three architectures take about 10 seconds (on a 24-core PC).
- `tools/arch_matrix.sh` runs the table of combinations in chapter 13 of the [design notes](design-notes.md) (in Japanese), with the expected outcome of each. It also lists the combinations it cannot run, with the reason, and `--all` fails on those too. It takes about 15 seconds on a Mac and about 7 seconds in a VM.
- `.sh` files keep LF line endings even when checked out on Windows (`.gitattributes`), so that bash in WSL and similar can run them from a Windows working folder.
- `tools/midi-timing` measures where in the period MIDI lands, on a real device ("Measuring MIDI timing" below, and [design notes](design-notes.md), chapter 15, in Japanese). It also shows how many of the messages sent reached the plugin, and how many arrived before ones sent earlier, and by how much. The purpose is to check that MIDI is neither dropped nor reordered, even when the audio drops out.
- `tools/app-icon` exports, from the SVG in `src/gui/icon`, Windows' `.ico`, macOS's `.icns`, the Linux PNGs, and the pixels of the Linux window icon (`window_icon.inc`). The exported files are in the repository too, so that the build needs no tool to render SVG.
  - After you change the SVG, run `dotnet run --project tools/app-icon` at the top of the repository. Icons of 32px and smaller are drawn from `brack-small.svg`, which leaves out details, so change both together.

### On Windows

- A build configured from scratch prints `cl : Command line warning D9025 : overriding '/W4' with '/W0'` for each file of the dependencies (the VST3 SDK and others). The cause is that `/W0` is added for the dependencies only, on top of the global `/W4`. It is not a warning about the source.
- If you rebuild only x64 without rebuilding x86, the x64 plugin host bundled with the x86 build stays old. If the plugin host protocol version (`kProtocolVersion`) has gone up, the x86 tests (`isolation`, `dll`) fail with "the plugin host is from another build of Brack (build both architectures again)".
- Run the tests from that architecture's environment (`vcvars32.bat` for x86, and on an ARM64 PC `vcvarsarm64_amd64.bat` and `vcvarsarm64_x86.bat` for x64 and x86).
- Run `tools/arch_matrix.sh` in Git Bash (`"C:\Program Files\Git\bin\bash.exe" -lc "cd <repository> && tools/arch_matrix.sh --all"`).
  - First build the ARM64EC and ARM64X test synths (`build-arm64ec`) with `powershell -ExecutionPolicy Bypass -File tools\arm64ec_synth.ps1` (about 1 minute). This needs Visual Studio's `VC.Tools.ARM64EC`.
    - `dumpbin /headers` shows what was built. The ARM64EC one shows "8664 machine (x64) (ARM64X)", and the ARM64X one "AA64 machine (ARM64) (ARM64X)".
  - The script looks for a Visual Studio that has the ARM64EC component (because `vswhere -latest` may pick one without it), and on an x64 PC it builds in the x64-to-ARM64 cross environment (`amd64_arm64`). Without these two bundles, the table marks their rows "not run", and `--all` fails.
- The dependencies that CMake fetches have deep folder trees, so allow long paths (`LongPathsEnabled` in the registry, and git's `core.longpaths`).
- When you set up the Visual Studio environment (`vcvarsall.bat`) from SSH or a PowerShell script and build, set both the console's input and output code pages to UTF-8 before you configure. If you change only the output code page, Ninja stops picking up header dependencies.
  - The reason is that cl.exe writes the `/showIncludes` prefix (in a Japanese Visual Studio, "メモ: インクルード ファイル:") in the output code page, and CMake reads it in the input code page. When the dependencies are not picked up, `ninja -C build -t deps <obj>` shows `#deps 0`, and changing a header rebuilds nothing.
  - The prefix is baked into the build folder's configuration, so after the fix, delete the build folder and configure again.
- A log written with PowerShell's `*>` is UTF-16. Written with `cmd /c "... > log 2>&1"`, it keeps the original encoding.
- An array passed to PowerShell's `-File` becomes one string (`-Modes now,ahead` becomes `"now,ahead"`). The script splits it with `-split ","`.
- A GUI started over SSH runs in a session with no screen, so it is not visible. To show it on the desktop, start it in the logged-on session with the Task Scheduler (`schtasks /create /tn brackgui /tr <path to brack.exe> /sc once /st 23:59 /it /f`, `schtasks /run /tn brackgui`, and when done, `schtasks /delete /tn brackgui /f`).
- With a display driver that has no OpenGL for ARM64 programs (VMware's SVGA 3D, for example), the ARM64 GUI ends with "cannot open an OpenGL 3 window". It opens if you put the ARM64 `opengl32.dll` of Mesa's llvmpipe (OpenGL rendered in software. `llvmpipe-arm64` from [mmozeiko/build-mesa](https://github.com/mmozeiko/build-mesa)) next to `brack.exe` (a rebuild does not delete it). The x64 and x86 GUIs open as they are. Microsoft's "OpenCL, OpenGL, and Vulkan Compatibility Pack" runs on D3D12, so it does not help with drivers that stop at D3D11 (VMware's SVGA 3D).

### On Linux

- On Ubuntu, the tests and measurements use `alsa-utils` (`aplaymidi`, `aconnect`), `dotnet-sdk-10.0` and `xdotool`. To run the x86 and ARM64 builds on x64 Linux, also install `libc6-i386`, `qemu-user` and `qemu-user-binfmt`. To try editors, you can use the plugins packaged by Ubuntu (`dpf-plugins`, `lsp-plugins`).
- `tools/arch_matrix.sh` switches the qemu-user and FEX-Emu registrations (binfmt_misc) and hides the 32-bit C library (in a mount namespace for that test only) as it runs. So it needs sudo without a password. It puts everything back when it ends.
- `test_isolation --block-cost` exceeds RealtimeKit's limit on requests (25 per 20 seconds per user). It makes 61 requests without `<frames>` and 31 with it. So the later plugin hosts may stay at normal priority (the log shows a warning). The same happens right after a full ctest run, because the earlier tests have used up the requests (`Device or resource busy`).
- Only when `BRACK_TEST_XFT_DPI=1` does `test_isolation` change the screen's `Xft.dpi` for a moment, to check that the editor receives `set_scale`. Every program on the same screen sees the change, so it is not tried by default. The original `RESOURCE_MANAGER` is written back unchanged.
- Programs started over SSH are refused by RealtimeKit and run at normal priority (["Real-time priority (Linux)" in the details](details.md#real-time-priority-linux)). Started with `systemd-run --user`, they get permission from RealtimeKit. The user's systemd has the session's environment (`DISPLAY`, `WAYLAND_DISPLAY`, `XAUTHORITY`), so the GUI also appears on the desktop. Give the executable's absolute path, and environment variables with `-E` (`-E BRACK_AUDIO_BACKEND=alsa` and so on). In an SSH shell, first set `XDG_RUNTIME_DIR=/run/user/$(id -u)`.

  ```bash
  systemd-run --user --unit=brack-gui --collect ~/brack/build/bin/brack
  systemd-run --user --quiet --wait --pipe -p WorkingDirectory=$PWD dotnet tools/midi-timing/bin/Release/net10.0/MidiTiming.dll build/bin/libbrack.so build/bin/brack-test-synth.clap now 480 1500
  ```

- To measure with an output that has a well-behaved clock, or to record the output's sound, add a null sink to PipeWire and make it the default. The recording program also follows the default output (`pw-record` follows it even with `--target` or `node.dont-move`), so stop it from connecting automatically and connect it by hand to the null sink's monitor. Count the silence in the recording by the peak of each 1 ms. When done, restore the default and remove the null sink (`pw-cli destroy brack-test-sink`).

  ```bash
  pw-cli create-node adapter '{ factory.name=support.null-audio-sink node.name=brack-test-sink node.description="Brack test sink" media.class=Audio/Sink object.linger=true audio.position=[FL FR] audio.rate=48000 }'
  wpctl set-default <the number of Brack test sink in wpctl status>
  pw-record -P "{ node.name=rec node.autoconnect=false }" --rate 48000 --channels 2 --format s16 out.wav &
  pw-link brack-test-sink:monitor_FL rec:input_FL
  pw-link brack-test-sink:monitor_FR rec:input_FR
  ```

- To stop the device during a measurement (to see whether Brack decides it has stopped and reopens it), pause the PipeWire of your own session (`kill -STOP`, then `kill -CONT` 1 second later). With PulseAudio, stop PulseAudio.
- When you send the working tree to another machine with tar, give the files the time of unpacking (`tar -xm`) so that Ninja rebuilds. If you send it with git, set the receiving repository to `receive.denyCurrentBranch updateInstead`, and a push updates the working tree too.
- To run x64 and x86 plugins on ARM64 Linux, install FEX-Emu. On Ubuntu, install from `ppa:fex-emu/fex` the package that matches the CPU, such as `fex-emu-armv8.4`, and the registrations `fex-emu-binfmt32` and `fex-emu-binfmt64`.
  - Fetch the root file system with `FEXRootFSFetcher`. Use the Ubuntu 24.04 one (glibc 2.39). Plugin hosts cross-built on Ubuntu 26.04 require glibc 2.38, so they do not start on older ones (Ubuntu 22.04 and others) ([design notes](design-notes.md), chapter 15, "配るプラグインホストの glibc", in Japanese). The plugin hosts in the release run from glibc 2.28.
  - With the Fedora 44 one (glibc 2.43), a FEX-Emu bug means that when x64 and x86 plugins overflow the stack in their own threads, the crash is not reported (they keep running, or become "stopped responding"). So the `crash` and `isolation` tests fail ([design notes](design-notes.md), chapter 15, in Japanese).
  - Without `~/.config/fex-emu/Config.json`, `FEXRootFSFetcher` says it set the default but does not write it, and FEX-Emu cannot find the root file system. Write it first, for example `{"Config":{"RootFS":"Fedora_44"}}` (if the file exists, the tool rewrites it).
  - To choose the root file system for each run, `FEX_ROOTFS` alone is not enough (the one mounted by the running FEXServer is used). Also separate `XDG_DATA_HOME` and `FEX_SERVERSOCKETPATH`, so that a separate FEXServer runs.
  - The qemu-user registrations (`qemu-user-binfmt`) conflict with FEX-Emu's, and installing one removes the other. Also, qemu-user gets its loader and libraries through a single `QEMU_LD_PREFIX` (`/usr/x86_64-linux-gnu` or `/usr/i686-linux-gnu`), so only one of x64 and x86 runs at a time.
  - To test the code path that finds FEX-Emu on the PATH, turn the registrations off for a while (`echo 0 | sudo tee /proc/sys/fs/binfmt_misc/FEX-x86_64 /proc/sys/fs/binfmt_misc/FEX-x86`, and `1` to restore them). `tools/arch_matrix.sh --all` switches them itself as it runs.

### Measuring MIDI timing (`tools/midi-timing`)

```bat
dotnet build tools\midi-timing -c Release
tools\midi-timing\bin\Release\net10.0\MidiTiming.exe build\bin\brack.dll build\bin\brack-test-synth.clap now 480 1500
```

- The tool sends a note every 7.3 ms (an interval offset from the period). It compares the frame where each note arrived, which the test synth records through `BRACK_TESTSYNTH_FRAMES`, with the time it was sent.
- `now` and `ahead` send through Brack's API, so they need nothing else. `--exclusive` uses WASAPI exclusive mode. The volume is 0, so there is no sound.
- `midi`, `midi-ahead` and `midi-zero` send through a MIDI port.
  - On Windows, the tool sends from the Windows MIDI Services endpoint named by `--port=` (a loopback or similar). Without one, it sends from the WinMM output of the same name. Without either, it has Brack create a virtual port.
  - On Linux and macOS, the tool creates the ALSA sequencer or CoreMIDI port named by `--port=` itself, and Brack opens it as an input.
- If Windows MIDI Services is a version that stops when a virtual port is deleted (microsoft/MIDI#1047), pass the name of a loopMIDI or similar port, so that the tool does not have a virtual port created. WinMM cannot attach timestamps, so `midi-ahead` cannot run through that path.
- On Windows, if you mistype the name, neither path is found, and Brack creates a virtual port. Check the name in the list of WinMM outputs first.
- The jitter width is the width of the deviations of the arrival frames from a line fitted to the send times. It does not tell uniform jitter from a single step. One step makes the width about the height of the step, and also tilts the line.
  - When the width is large, lay out the deviations over time, and tell whether there is a step, uniform jitter, or a steady slope (a device clock drifting). Laying out the render position minus the advance of steady_clock (the reading of `ArrivalClock`) shows when the device lost time. The tool does neither, so add them yourself to look.

## Releases

When you create a release on GitHub, `.github/workflows/release.yml` builds and tests Brack, attaches the release archives and the NuGet package to the release, and publishes the .NET binding to nuget.org. Tags take the form `v1.2.0`, and a prerelease can be written like `v1.2.0-beta.1`. The tag without the `v` becomes `BRACK_VERSION` and the NuGet package version.

- The release archives are zips for Windows (x64, x86, ARM64) and macOS (universal), and tar.gz files for Linux (x64, ARM64). Their contents are as in ["Release contents" in the details](details.md#release-contents).
- The three Windows builds are built on an x64 machine, in the order of "Windows" under "Build". The ARM64 build is a cross build, so CI does not test it.
- macOS is built on an Apple silicon machine as a universal build with ad hoc signing (`-`). There is no Developer ID signing or notarization.
- Linux is built by `tools/linux_release_build.sh` inside a manylinux_2_28 container (AlmaLinux 8, GCC 14), so that it runs on glibc 2.28 or later. The tests run outside the container, on Ubuntu.
  - An x64 machine builds x86 (`-m32`) and x64, and an ARM64 machine builds the ARM64 build, which bundles the x64 and x86 plugin hosts of the x64 build.
  - To build by hand, mount the source folder into the container at the same path and run the script, as its header shows. For ARM64, copy `brack-host-x64` and `brack-host-x86` from the `build/bin` built on the x64 machine into one folder, and pass it with `-DBRACK_OTHER_ARCH_BINS`. With Docker, the folders end up owned by root, so take them back afterwards with `sudo chown -R` (rootless podman does not need this).
  - Before publishing, `linux-smoke` unpacks the release in containers of Ubuntu, Debian, AlmaLinux, Rocky Linux, Fedora, openSUSE and Arch, installs only the libraries needed with `tools/linux_smoke_test.sh`, and checks that Brack starts and scans. This and the build environment are the basis for "glibc 2.28 or later" in the README's requirements.
- The files of the release archives and the NuGet package are checked against the list in `tools/check_release_files.sh`. When you change which files go in, update this list too. The Linux binaries are also checked for requiring no glibc newer than 2.28 (`cmake/check_glibc_needs.cmake`).
- When you run the workflow by hand from the Actions page and pass a tag, it rebuilds that release's archives and replaces them. If nuget.org already has the same version, it does not upload the package. Without a tag, the run is a trial. The version becomes `0.0.0-dev.<run number>`, and the archives and the package are only kept as the run's artifacts.

## Writing docs and comments

Each document has its own role.

| Document | What it covers |
|---|---|
| [README](../README.md) | For users: what Brack does, requirements, installation, basic usage |
| [details.md](details.md) | Details for users (formats, search paths, crash handling, MIDI, audio, and more) |
| development.md (this document) | For developers: source layout, building, tests and tools, releases, notes for the work |
| [design-notes.md](design-notes.md) | Decisions and their reasons, investigation and measurement results, remaining work (TODO) |
| Comments in `include/brack/brack.h` and the .NET classes | How to use the library, and its contracts |

README, details.md and development.md each have an English version (the default) and a Japanese version (`.ja.md`). When you change one, change the other to match. design-notes.md is in Japanese only.

Docs and comments both contain:

- The reason for the current design, and the alternatives not taken (with why).
- Constraints that still apply, and their workarounds (quirks of an OS or of real plugins, and others).
- Measurements that support a conclusion. Give the numbers and the main conditions (environment, period, rate, number of plugins). Give a representative value, not the value of each run.
- Steps to reproduce or check something, when they are written nowhere else.
- The current contracts and behavior.
- A fact that something was checked, only when it is the sole basis for a claim that is not obvious, and briefly (for example, that a row of `tools/arch_matrix.sh` turns FAIL when the mechanism it checks breaks).

Docs and comments do not contain:

- The history of a fixed bug ("it used to..."). Write it only as the reason for the current design, when it is one. When you fix something, rewrite the description of the current design instead of adding the history.
- Dated records of checks ("N tests passed on ...", "checked on ..."). Where and what was checked goes only in the table "アーキテクチャごとの状態" (status per architecture) in chapter 13 of the [design notes](design-notes.md).
- Fine details of how something was measured (the number of runs, alternating measurements, how the median was taken, which processes were stopped). Write them only when they affect how far the result can be trusted. The measuring methods themselves go in "Tests and tools" in this document.
- VM configurations that do not bear on the conclusion.

Do not rename headings and bold labels that other files or code comments refer to (the chapter numbers of the design notes, chapter 15's "Windows（ARM64）", and others). In comments, do not write what the code already shows.

## Platform status

The target architectures are as in ["Requirements" in the details](details.md#requirements). Plugin executables in PE, ELF and Mach-O (including universal) can all have their architecture and exports read from their headers on every OS.

How far each OS and architecture has been checked (the number of tests, the environments) is in the table "アーキテクチャごとの状態" in [chapter 13 of the design notes](design-notes.md#13-他プラットフォームへの対応) (in Japanese). What remains (TODO), and why, is in the same chapter. The implementation decisions and checks for each OS are in the per-OS sections of chapter 15. Linux and macOS share Brack's POSIX parts (communication with the plugin host and others).
