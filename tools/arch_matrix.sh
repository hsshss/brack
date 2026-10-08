#!/bin/bash
# Every combination of CPU architectures this machine can put together, run through the DLL
# (test_dll's mixed rack): each build of Brack, each way it can run (natively, Rosetta 2, FEX-Emu,
# qemu-user, Windows' emulation, none), with the test synths (CLAP, VST2, VST3) of every build and
# the ARM64EC/ARM64X bundles of tools/arm64ec_synth.ps1. Each folder has the outcome it must have:
# run as an architecture, or be refused (and why, where known: "no-host" for a plugin host the
# build does not ship, "not-run" for an architecture the computer does not run, "no-binary" for a
# VST3 bundle with no binary for it). The combinations per OS: design notes, ch. 13.
#
#   tools/arch_matrix.sh [--all]
#
# The builds sit beside each other in the repository: build (this machine's), build-x64 and
# build-universal (macOS), build-x64 and build-x86 (ARM64 Linux), build-x86 and build-arm64 (x64
# Linux); on Windows build-arm64, build (x64) and build-x86 (docs/development.md: on an x64 PC the arm64
# build is cross-built, and only its synths are used).
# On Linux the ways to run other architectures' programs are switched in binfmt_misc (sudo without
# a password), and put back on exit; the 32-bit C library is hidden in a mount namespace of the
# test's own. A combination that cannot run here is listed with why; --all fails then too.
set -u
repo="$(cd "$(dirname "$0")/.." && pwd)"
case "$(uname -s)" in MINGW* | MSYS*) repo="$(cygpath -m "$repo")" ;; esac  # C:/... for test_dll.exe
all=0
[ "${1:-}" = "--all" ] && all=1
logs="$(mktemp -d "${TMPDIR:-/tmp}/brack-arch-matrix.XXXXXX")"
rows=()
failed=0
skipped=0

synths() {  # the test synths of build $1, as test_dll takes them
  local bin="$repo/$1/bin" vst2=brack-test-synth-vst2.so
  [ "$(uname -s)" = Darwin ] && vst2=brack-test-synth-vst2.vst
  [ -n "$windows" ] && vst2=brack-test-synth-vst2.dll
  echo "$bin/brack-test-synth.clap" "$bin/$vst2" "$bin/brack-test-synth.vst3"
}

# run <what> <expectations> <command...>: one combination; the command ends in its folders.
run() {
  local what="$1" expect="$2" log="$logs/$(( ${#rows[@]} + 1 )).log"
  shift 2
  if "$@" > "$log" 2>&1; then
    rows+=("PASS  $what: $expect")
  else
    rows+=("FAIL  $what: $expect (log: $log)")
    failed=$(( failed + 1 ))
  fi
}

skip() {
  rows+=("----  $1: not run, $2")
  skipped=$(( skipped + 1 ))
}

need() {  # need <build> <architecture as `file`/lipo name it>: the build is there, for that
  local f="$repo/$1/bin/test_dll$windows"
  [ -x "$f" ] || return 1
  if [ "$(uname -s)" = Darwin ]; then
    [ "$(lipo -archs "$f" 2>/dev/null)" = "$2" ]
  else
    file -L "$f" | grep -qE "$2"
  fi
}

macos() {
  local rosetta=0 a=build x=build-x64 u=build-universal
  [ -e /Library/Apple/usr/libexec/oah/libRosettaRuntime ] && rosetta=1
  local x64=refused
  [ $rosetta = 1 ] && x64=x64
  local missing=""
  need $a arm64 || missing="$missing $a (arm64)"
  need $x x86_64 || missing="$missing $x (CMAKE_OSX_ARCHITECTURES=x86_64)"
  need $u "x86_64 arm64" || missing="$missing $u (CMAKE_OSX_ARCHITECTURES=\"arm64;x86_64\")"
  if [ -n "$missing" ]; then
    skip "every combination" "builds missing:$missing"
    return
  fi
  local A="$repo/$a/bin" X="$repo/$x/bin" U="$repo/$u/bin"
  run "arm64 build" "x64 build=$x64, universal=arm64" \
    arch -arm64 "$A/test_dll" $(synths $a) "$X=$x64" "$U=arm64"
  run "universal build as arm64" "arm64 build=arm64, x64 build=$x64" \
    arch -arm64 "$U/test_dll" $(synths $u) "$A=arm64" "$X=$x64"
  if [ $rosetta = 1 ]; then
    run "x64 build (Rosetta 2)" "arm64 build=refused (no arm64 host in it), universal=x64" \
      arch -x86_64 "$X/test_dll" $(synths $x) "$A=refused" "$U=x64"
    run "universal build as x64 (Rosetta 2)" "arm64 build=arm64, x64 build=x64" \
      arch -x86_64 "$U/test_dll" $(synths $u) "$A=arm64" "$X=x64"
  else
    skip "x64 and universal builds as x64" "Rosetta 2 is not installed"
  fi
}

# ---- ARM64 Linux: the ways to run x64 and x86 programs, switched in binfmt_misc ----
binfmt=/proc/sys/fs/binfmt_misc
restore=()  # commands that put binfmt_misc back, run on exit

entry_state() { [ -e "$binfmt/$1" ] && head -n1 "$binfmt/$1" || echo absent; }

set_entry() {  # set_entry <name> 0|1
  [ -e "$binfmt/$1" ] || return 0
  echo "$2" | sudo -n tee "$binfmt/$1" > /dev/null
}

# qemu-user's entries, as qemu-binfmt-conf.sh writes them (no F flag: the interpreter is found
# when a program starts).
qemu_line() {
  local elf='\x7fELF' mask='\xff\xff\xff\xff\xff\xfe\xfe\x00\xff\xff\xff\xff\xff\xff\xff\xff\xfe\xff\xff\xff'
  case "$1" in
    qemu-x86_64) echo ":qemu-x86_64:M::$elf\x02\x01\x01\x00\x00\x00\x00\x00\x00\x00\x00\x00\x02\x00\x3e\x00:$mask:$(command -v qemu-x86_64):" ;;
    qemu-i386) echo ":qemu-i386:M::$elf\x01\x01\x01\x00\x00\x00\x00\x00\x00\x00\x00\x00\x02\x00\x03\x00:$mask:$(command -v qemu-i386):" ;;
    qemu-aarch64) echo ":qemu-aarch64:M::$elf\x02\x01\x01\x00\x00\x00\x00\x00\x00\x00\x00\x00\x02\x00\xb7\x00:$mask:$(command -v qemu-aarch64):" ;;
  esac
}

put_back() {
  local i
  for (( i = ${#restore[@]} - 1; i >= 0; i-- )); do eval "${restore[$i]}"; done
  restore=()
}
trap put_back EXIT

# mode fex|path|none|qemu: FEX-Emu's entries on or off, qemu-user's registered or not.
mode() {
  local fex=1 qemu=0 name
  case "$1" in
    path|none) fex=0 ;;
    qemu) fex=0; qemu=1 ;;
  esac
  for name in FEX-x86_64 FEX-x86; do
    local was; was="$(entry_state $name)"
    [ "$was" = absent ] && continue
    local want=disabled; [ $fex = 1 ] && want=enabled
    if [ "$was" != "$want" ]; then
      set_entry $name $fex || return 1
      restore+=("set_entry $name $([ "$was" = enabled ] && echo 1 || echo 0)")
    fi
  done
  for name in qemu-x86_64 qemu-i386; do
    local was; was="$(entry_state $name)"
    if [ $qemu = 1 ] && [ "$was" = absent ]; then
      qemu_line $name | sudo -n tee "$binfmt/register" > /dev/null || return 1
      restore+=("echo -1 | sudo -n tee $binfmt/$name > /dev/null")
    elif [ "$was" != absent ] && [ "$was" != "$([ $qemu = 1 ] && echo enabled || echo disabled)" ]; then
      set_entry $name $qemu || return 1
      restore+=("set_entry $name $([ "$was" = enabled ] && echo 1 || echo 0)")
    fi
  done
}

# What qemu-user needs to start a program: its loader, in / or under QEMU_LD_PREFIX.
qemu_runs() {  # qemu_runs <loader> <prefix>
  [ -e "$1" ] || { [ -n "$2" ] && [ -e "$2$1" ]; }
}

linux_arm64() {
  local a=build x=build-x64 y=build-x86 missing=""
  need $a aarch64 || missing="$missing $a (ARM64)"
  need $x x86-64 || missing="$missing $x (cmake/toolchains/linux-x64.cmake)"
  need $y "Intel (i386|80386)" || missing="$missing $y (cmake/toolchains/linux-x86.cmake)"
  if [ -n "$missing" ]; then
    skip "every combination" "builds missing:$missing"
    return
  fi
  local A="$repo/$a/bin" X="$repo/$x/bin" Y="$repo/$y/bin"
  local fex_registered=0
  [ "$(entry_state FEX-x86_64)" != absent ] && [ "$(entry_state FEX-x86)" != absent ] && fex_registered=1

  if [ $fex_registered = 1 ] && mode fex; then
    run "arm64 build, FEX-Emu registered" "x64 build=x64, x86 build=x86" \
      "$A/test_dll" $(synths $a) "$X=x64" "$Y=x86"
    run "x64 build (FEX-Emu)" "arm64 build=refused (no arm64 host in it), x86 build=x86" \
      "$X/test_dll" $(synths $x) "$A=refused" "$Y=x86"
    # Under FEX-Emu the kernel looks like x64 to the x86 build, as on an x64 PC: the x64 synth is
    # refused for the x64 host the x86 build does not ship, not as a program the computer cannot run.
    run "x86 build (FEX-Emu)" "arm64 build=refused, x64 build=refused (no x64 host in it)" \
      "$Y/test_dll" $(synths $y) "$A=refused" "$X=no-host"
  else
    skip "FEX-Emu registered; the x64 and x86 builds" "FEX-Emu's binfmt_misc entries are missing (fex-emu-binfmt32/64) or could not be switched on"
  fi
  put_back

  if ! sudo -n true 2> /dev/null; then
    skip "FEX-Emu from PATH, qemu-user, no emulator" "switching binfmt_misc needs sudo without a password"
    return
  fi
  if command -v FEX > /dev/null && mode path; then
    run "arm64 build, FEX-Emu from PATH" "x64 build=x64, x86 build=x86" \
      "$A/test_dll" $(synths $a) "$X=x64" "$Y=x86"
  else
    skip "FEX-Emu from PATH" "no FEX in PATH"
  fi
  put_back

  if mode none; then
    run "arm64 build, no emulator" "x64 build=refused, x86 build=refused (this computer does not run them)" \
      env PATH=/nonexistent "$A/test_dll" $(synths $a) "$X=not-run" "$Y=not-run"
  else
    skip "no emulator" "binfmt_misc could not be switched"
  fi
  put_back

  if command -v qemu-x86_64 > /dev/null && command -v qemu-i386 > /dev/null && mode qemu; then
    local prefix x64 x86
    for prefix in "" /usr/x86_64-linux-gnu /usr/i686-linux-gnu; do
      x64=not-run; qemu_runs /lib64/ld-linux-x86-64.so.2 "$prefix" && x64=x64
      x86=not-run; qemu_runs /lib/ld-linux.so.2 "$prefix" && x86=x86
      run "arm64 build, qemu-user, QEMU_LD_PREFIX=${prefix:-(none)}" "x64 build=$x64, x86 build=$x86" \
        env PATH=/nonexistent QEMU_LD_PREFIX="$prefix" "$A/test_dll" $(synths $a) "$X=$x64" "$Y=$x86"
    done
  else
    skip "qemu-user" "qemu-x86_64 and qemu-i386 are not installed (qemu-user)"
  fi
  put_back
}

# ---- x64 Linux: x86 programs through the 32-bit C library, arm64 ones through qemu-user ----
arm_mode() {  # arm_mode 0|1: qemu-user's entry for arm64 programs off, or registered and on
  local was; was="$(entry_state qemu-aarch64)"
  if [ "$1" = 1 ] && [ "$was" = absent ]; then
    command -v qemu-aarch64 > /dev/null || return 1
    qemu_line qemu-aarch64 | sudo -n tee "$binfmt/register" > /dev/null || return 1
    restore+=("echo -1 | sudo -n tee $binfmt/qemu-aarch64 > /dev/null")
  elif [ "$was" != absent ] && [ "$was" != "$([ "$1" = 1 ] && echo enabled || echo disabled)" ]; then
    set_entry qemu-aarch64 "$1" || return 1
    restore+=("set_entry qemu-aarch64 $([ "$was" = enabled ] && echo 1 || echo 0)")
  fi
}

# without_lib32 <command...>: as this user, with the 32-bit C library's folder (where its loader
# is) hidden under an empty one, in a mount namespace of the command's own.
without_lib32() {
  local empty="$logs/empty" dir
  dir="$(dirname "$(readlink -f /lib/ld-linux.so.2)")"
  mkdir -p "$empty"
  sudo -n unshare --mount sh -c 'mount --bind "$1" "$2" || exit 1; u=$3 g=$4 h=$5 p=$6; shift 6
    exec setpriv --reuid="$u" --regid="$g" --init-groups env HOME="$h" PATH="$p" "$@"' \
    sh "$empty" "$dir" "$(id -u)" "$(id -g)" "$HOME" "$PATH" "$@"
}

# The x64 and x86 builds ship no arm64 plugin host: arm64 plugins are refused for that when
# qemu-user would run it, and as programs this computer does not run otherwise. Under qemu-user,
# the arm64 build sees no way to run x64 and x86 programs (it takes the computer for an ARM64 one).
linux_x64() {
  local x=build y=build-x86 a=build-arm64 missing=""
  need $x x86-64 || missing="$missing $x (x64)"
  need $y "Intel (i386|80386)" || missing="$missing $y (cmake/toolchains/linux-x86.cmake)"
  need $a aarch64 || missing="$missing $a (cmake/toolchains/linux-arm64.cmake)"
  if [ -n "$missing" ]; then
    skip "every combination" "builds missing:$missing"
    return
  fi
  local X="$repo/$x/bin" Y="$repo/$y/bin" A="$repo/$a/bin" loaders=/usr/aarch64-linux-gnu
  local lib32=0 x86=not-run
  if [ -e /lib/ld-linux.so.2 ]; then
    lib32=1 x86=x86
  else
    skip "the x86 build" "the 32-bit C library is not installed (libc6-i386)"
  fi

  if arm_mode 1; then
    local prefix arm64
    for prefix in "" "$loaders"; do
      arm64=not-run; qemu_runs /lib/ld-linux-aarch64.so.1 "$prefix" && arm64=no-host
      run "x64 build, qemu-user, QEMU_LD_PREFIX=${prefix:-(none)}" "x86 build=$x86, arm64 build=refused ($arm64)" \
        env QEMU_LD_PREFIX="$prefix" "$X/test_dll" $(synths $x) "$Y=$x86" "$A=$arm64"
      [ $lib32 = 1 ] && run "x86 build, qemu-user, QEMU_LD_PREFIX=${prefix:-(none)}" "x64 build=refused (no-host), arm64 build=refused ($arm64)" \
        env QEMU_LD_PREFIX="$prefix" "$Y/test_dll" $(synths $y) "$X=no-host" "$A=$arm64"
    done
    if qemu_runs /lib/ld-linux-aarch64.so.1 "$loaders"; then
      run "arm64 build (qemu-user)" "x64 build=refused (not-run), x86 build=refused (not-run)" \
        env QEMU_LD_PREFIX="$loaders" "$A/test_dll" $(synths $a) "$X=not-run" "$Y=not-run"
    else
      skip "arm64 build (qemu-user)" "no ARM64 C library in $loaders (libc6-arm64-cross)"
    fi
  else
    skip "qemu-user" "qemu-aarch64 is not installed (qemu-user), or its entry could not be registered"
  fi
  put_back

  if ! sudo -n true 2> /dev/null; then
    skip "no qemu-user, no 32-bit C library" "switching needs sudo without a password"
    return
  fi
  # With the loaders there, so that the arm64 synths are refused as not run only for the entry off.
  if arm_mode 0; then
    run "x64 build, no qemu-user" "x86 build=$x86, arm64 build=refused (not-run)" \
      env QEMU_LD_PREFIX="$loaders" "$X/test_dll" $(synths $x) "$Y=$x86" "$A=not-run"
    [ $lib32 = 1 ] && run "x86 build, no qemu-user" "x64 build=refused (no-host), arm64 build=refused (not-run)" \
      env QEMU_LD_PREFIX="$loaders" "$Y/test_dll" $(synths $y) "$X=no-host" "$A=not-run"
    [ $lib32 = 1 ] && run "x64 build, without the 32-bit C library" "x86 build=refused (not-run), arm64 build=refused (not-run)" \
      without_lib32 env QEMU_LD_PREFIX="$loaders" "$X/test_dll" $(synths $x) "$Y=not-run" "$A=not-run"
  else
    skip "no qemu-user" "binfmt_misc could not be switched"
  fi
  put_back
}

windows_arm64() {
  local a=build-arm64 x=build y=build-x86 missing=""
  need $a "Aarch64|ARM64" || missing="$missing $a (ARM64)"
  need $x x86-64 || missing="$missing $x (x64)"
  need $y "Intel (i386|80386)" || missing="$missing $y (x86)"
  if [ -n "$missing" ]; then
    skip "every combination" "builds missing:$missing"
    return
  fi
  local A="$repo/$a/bin" X="$repo/$x/bin" Y="$repo/$y/bin"
  # The ARM64EC and ARM64X VST3 bundles of tools/arm64ec_synth.ps1. An x64 process on an ARM64
  # PC loads them and runs them natively, so every build takes them through the x64 plugin host
  # ("x64"), except the arm64 build with the ARM64X one, which holds arm64 code too.
  local E="$repo/build-arm64ec/arm64ec-win" H="$repo/build-arm64ec/arm64x-win"
  local -a ec=() hybrid64=() hybridA=()
  if [ -d "$E" ]; then ec=("$E=x64"); else skip "ARM64EC bundle" "not built (tools/arm64ec_synth.ps1)"; fi
  if [ -d "$H" ]; then hybrid64=("$H=x64") hybridA=("$H=arm64"); else skip "ARM64X bundle" "not built (tools/arm64ec_synth.ps1)"; fi
  run "arm64 build" "x64 build=x64, x86 build=x86 (Windows' emulation), ARM64EC=x64, ARM64X=arm64" \
    "$A/test_dll.exe" $(synths $a) "$X=x64" "$Y=x86" "${ec[@]}" "${hybridA[@]}"
  run "x64 build (Windows' emulation)" "arm64 build=refused (no arm64 host in it), x86 build=x86, ARM64EC=x64, ARM64X=x64" \
    "$X/test_dll.exe" $(synths $x) "$A=refused" "$Y=x86" "${ec[@]}" "${hybrid64[@]}"
  run "x86 build (Windows' emulation)" "arm64 build=refused (no arm64 host in it), x64 build=x64, ARM64EC=x64, ARM64X=x64" \
    "$Y/test_dll.exe" $(synths $y) "$A=refused" "$X=x64" "${ec[@]}" "${hybrid64[@]}"
}

# On an x64 PC: the x64 and x86 builds run each other's plugins (an x86 Brack the x64 plugin host);
# arm64 code runs in neither. The ARM64EC bundle holds no binary for them (it is listed as nothing),
# and the ARM64X one is arm64.
windows_x64() {
  local x=build y=build-x86 a=build-arm64 missing=""
  need $x x86-64 || missing="$missing $x (x64)"
  need $y "Intel (i386|80386)" || missing="$missing $y (x86)"
  if [ -n "$missing" ]; then
    skip "every combination" "builds missing:$missing"
    return
  fi
  local X="$repo/$x/bin" Y="$repo/$y/bin" A="$repo/$a/bin"
  local E="$repo/build-arm64ec/arm64ec-win" H="$repo/build-arm64ec/arm64x-win"
  local -a arm=() ec=() hybrid=()
  if need $a "Aarch64|ARM64"; then arm=("$A=not-run"); else skip "arm64 build's synths" "not built ($a, with the ARM64 build tools)"; fi
  if [ -d "$E" ]; then ec=("$E=no-binary"); else skip "ARM64EC bundle" "not built (tools/arm64ec_synth.ps1)"; fi
  if [ -d "$H" ]; then hybrid=("$H=not-run"); else skip "ARM64X bundle" "not built (tools/arm64ec_synth.ps1)"; fi
  run "x64 build" "x86 build=x86, arm64 build=refused (this computer does not run arm64), ARM64EC=refused (no binary for it), ARM64X=refused (arm64)" \
    "$X/test_dll.exe" $(synths $x) "$Y=x86" "${arm[@]}" "${ec[@]}" "${hybrid[@]}"
  run "x86 build (WOW64)" "x64 build=x64, arm64 build=refused (this computer does not run arm64), ARM64EC=refused (no binary for it), ARM64X=refused (arm64)" \
    "$Y/test_dll.exe" $(synths $y) "$X=x64" "${arm[@]}" "${ec[@]}" "${hybrid[@]}"
}

windows=""
case "$(uname -s)" in MINGW* | MSYS*) windows=.exe ;; esac
# Windows' own name for its CPU, from the system's environment: Git Bash may itself be an x64
# program under emulation, which every program it starts takes for its own.
[ -n "$windows" ] && native="$(MSYS2_ARG_CONV_EXCL='*' reg query \
  'HKLM\SYSTEM\CurrentControlSet\Control\Session Manager\Environment' /v PROCESSOR_ARCHITECTURE |
  awk '/PROCESSOR_ARCHITECTURE/ { print $NF }' | tr -d '\r')"
case "$(uname -s)-$(uname -m)" in
  Darwin-*) macos ;;
  Linux-aarch64) linux_arm64 ;;
  Linux-x86_64) linux_x64 ;;
  MINGW*-* | MSYS*-*)
    case "$native" in
      ARM64) windows_arm64 ;;
      AMD64) windows_x64 ;;
      *)
        echo "tools/arch_matrix.sh covers ARM64 and x64 Windows; this Windows is $native" >&2
        exit 2
        ;;
    esac
    ;;
  *)
    echo "tools/arch_matrix.sh covers macOS, ARM64 and x64 Linux, and ARM64 and x64 Windows; this is $(uname -s) $(uname -m)" >&2
    exit 2
    ;;
esac

printf '%s\n' "${rows[@]}"
echo "$failed failed, $skipped not run"
[ $failed -eq 0 ] && rm -rf "$logs"
[ $failed -eq 0 ] && { [ $all = 0 ] || [ $skipped -eq 0 ]; }
