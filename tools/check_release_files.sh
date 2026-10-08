#!/bin/bash
# tools/check_release_files.sh dist <folder> <rid>   a distribution (cmake --install) of that rid
# tools/check_release_files.sh nupkg <file.nupkg>    the NuGet package's native files
# Fails unless the files are exactly those listed here. A plugin host from another architecture's
# build is installed OPTIONAL, and packed only where it exists, so neither step misses one.
# Fails too when a Linux binary needs a newer glibc than 2.28, the oldest the release supports
# (it is built in manylinux_2_28 containers: tools/linux_release_build.sh); needs readelf.
set -euo pipefail

win_dist() {
  printf '%s\n' LICENSE THIRD_PARTY_NOTICES.md include/brack/brack.h lib/brack.lib \
    bin/brack.exe bin/brack-cli.exe bin/brack.dll bin/brack-host-x86.exe bin/brack-host-x64.exe
}
linux_dist() {
  printf '%s\n' share/doc/brack/LICENSE share/doc/brack/THIRD_PARTY_NOTICES.md include/brack/brack.h \
    bin/brack bin/brack-cli lib/libbrack.so libexec/brack/brack-host-x86 libexec/brack/brack-host-x64 \
    share/applications/brack.desktop share/icons/hicolor/scalable/apps/brack.svg
  for px in 16 24 32 48 64 128 256 512; do
    echo "share/icons/hicolor/${px}x${px}/apps/brack.png"
  done
}
dist_files() {
  case $1 in
    win-x64 | win-x86) win_dist ;;
    win-arm64) win_dist; echo bin/brack-host-arm64.exe ;;
    linux-x64) linux_dist ;;
    linux-arm64) linux_dist; echo libexec/brack/brack-host-arm64 ;;
    osx-universal)
      printf '%s\n' LICENSE THIRD_PARTY_NOTICES.md include/brack/brack.h bin/brack-cli lib/libbrack.dylib \
        libexec/brack/brack-host-arm64 libexec/brack/brack-host-x64 \
        Brack.app/Contents/Info.plist Brack.app/Contents/_CodeSignature/CodeResources \
        Brack.app/Contents/MacOS/Brack Brack.app/Contents/MacOS/brack-host-arm64 \
        Brack.app/Contents/MacOS/brack-host-x64 Brack.app/Contents/Resources/brack.icns ;;
    *) echo "no file list for '$1'" >&2; exit 2 ;;
  esac
}

nupkg_files() {
  local rid
  for rid in win-x64 win-x86; do
    printf "runtimes/$rid/native/%s\n" brack.dll brack-host-x86.exe brack-host-x64.exe
  done
  printf 'runtimes/win-arm64/native/%s\n' brack.dll brack-host-x86.exe brack-host-x64.exe brack-host-arm64.exe
  printf 'runtimes/linux-x64/native/%s\n' libbrack.so brack-host-x86 brack-host-x64
  printf 'runtimes/linux-arm64/native/%s\n' libbrack.so brack-host-x86 brack-host-x64 brack-host-arm64
  printf 'runtimes/osx-arm64/native/%s\n' libbrack.dylib brack-host-arm64 brack-host-x64
  printf 'runtimes/osx-x64/native/%s\n' libbrack.dylib brack-host-x64
}

# compare <what> <expected> <actual>: the lists, one path a line, in any order.
compare() {
  local diff
  if diff=$(diff <(LC_ALL=C sort <<<"$2") <(LC_ALL=C sort <<<"$3")); then
    echo "$1: as expected ($(wc -l <<<"$2" | tr -d ' ') files)"
  else
    echo "$1 differs from the list in $0 (< missing, > not listed):" >&2
    grep '^[<>]' <<<"$diff" >&2
    exit 1
  fi
}

# check_glibc <file or folder>
check_glibc() {
  cmake "-DREADELF=${READELF:-readelf}" -DNEWEST=28 "-DFILE=$1" -P "$(dirname "$0")/../cmake/check_glibc_needs.cmake"
}

case ${1:-} in
  dist)
    expected=$(dist_files "$3")
    actual=$(cd "$2" && find . -type f -o -type l | sed 's|^\./||')
    compare "$2" "$expected" "$actual"
    if [[ $3 == linux-* ]]; then
      check_glibc "$2"
    fi ;;
  nupkg)
    expected=$(nupkg_files)
    actual=$(unzip -Z1 "$2" | { grep '^runtimes/' || true; })
    compare "$2" "$expected" "$actual"
    linux=$(mktemp -d)
    trap 'rm -rf "$linux"' EXIT
    unzip -q "$2" 'runtimes/linux-*' -d "$linux"
    check_glibc "$linux" ;;
  *)
    echo "usage: $0 dist <folder> <rid> | nupkg <file.nupkg>" >&2; exit 2 ;;
esac
