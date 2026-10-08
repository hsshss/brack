#!/bin/bash
# tools/linux_smoke_test.sh <unpacked release> <synths>
# As root in a fresh container of a Linux distribution: installs only what the release needs
# (docs/details.md, Release contents), with the 32-bit C library for the x86 plugin host on x64, then
# fails unless every binary of this machine's architectures finds its libraries, brack-cli starts,
# and it lists the test synths in <synths> (<synths>/<arch>/brack-test-synth.clap) with their
# architectures: x64 and x86 on x64, arm64 on ARM64. Brack loads libasound and libX11 only when it
# needs them, so where the image lacks them, all but the GUI is tried before they are installed too.
set -euo pipefail
release=$1 synths=$2

case $(uname -m) in
  x86_64) archs="x64 x86" machines="3e00 0300" ;;
  aarch64) archs="arm64" machines="b700" ;;
  *) echo "no smoke test for $(uname -m)" >&2; exit 2 ;;
esac
x64=$([[ $archs == x64* ]] && echo yes || true)

if command -v apt-get > /dev/null; then
  export DEBIAN_FRONTEND=noninteractive
  [[ $x64 ]] && dpkg --add-architecture i386
  apt-get update -q
  alsa=libasound2
  apt-cache show libasound2t64 > /dev/null 2>&1 && alsa=libasound2t64
  pkg_install() { apt-get install -y -q --no-install-recommends "$@"; }
  base=${x64:+libc6:i386} libs="$alsa libx11-6 libgl1"
elif command -v dnf > /dev/null; then
  pkg_install() { dnf install -y -q "$@"; }
  base=${x64:+glibc.i686} libs="alsa-lib libX11 libglvnd-glx"
elif command -v zypper > /dev/null; then
  pkg_install() { zypper -n -q install --no-recommends "$@"; }
  base=${x64:+glibc-32bit} libs="libasound2 libX11-6 libglvnd"
elif command -v pacman > /dev/null; then
  [[ $x64 ]] && printf '[multilib]\nInclude = /etc/pacman.d/mirrorlist\n' >> /etc/pacman.conf
  pacman -Syu --noconfirm -q
  pkg_install() { pacman -S --noconfirm -q --needed "$@"; }
  base=${x64:+lib32-glibc} libs="alsa-lib libx11 libglvnd"
else
  echo "no package manager this script knows" >&2; exit 2
fi

# Fails unless every binary of this machine's architectures, but those named, finds its libraries.
libraries_found() {
  local file libs
  shopt -s globstar
  for file in "$release"/**; do
    [[ -f $file && $(od -An -tx1 -N4 "$file" | tr -d ' \n') == 7f454c46 ]] || continue
    [[ " $machines " == *" $(od -An -tx1 -j18 -N2 "$file" | tr -d ' \n') "* ]] || continue
    [[ " $* " == *" ${file#"$release"/} "* ]] && continue
    libs=$(ldd "$file")
    if grep 'not found' <<< "$libs"; then
      echo "$file: libraries not found" >&2
      exit 1
    fi
    echo "libraries found: $file"
  done
}

starts_and_scans() {
  "$release/bin/brack-cli" devices
  "$release/bin/brack-cli" midi-inputs
  local listed expected
  listed=$("$release/bin/brack-cli" scan --dir "$synths" --json)
  found=$(grep -o '"architecture": "[a-z0-9]*"' <<< "$listed" | cut -d'"' -f4 | sort -u | tr '\n' ' ')
  expected=$(tr ' ' '\n' <<< "$archs" | sort | tr '\n' ' ')
  if [[ $found != "$expected" ]]; then
    echo "$listed" >&2
    echo "the scan found test synths for '$found', not '$expected'" >&2
    exit 1
  fi
}

[[ $base ]] && pkg_install $base
cache=$(ldconfig -p)
if grep -q -e 'libasound\.so\.2 ' -e 'libX11\.so\.6 ' <<< "$cache"; then
  without="no (the image has libasound or libX11)"
else
  libraries_found bin/brack
  starts_and_scans
  without=yes
fi

pkg_install $libs
libraries_found
starts_and_scans
echo "smoke test passed: $(. /etc/os-release && echo "$PRETTY_NAME"), test synths for $found," \
  "tried without libasound and libX11: $without"
