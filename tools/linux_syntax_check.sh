#!/bin/bash
# Compiles Brack's shared sources (everything but the Windows-only files) with Linux's g++ for
# x64, x86 and arm64, syntax only, to keep them building for Linux and macOS. Needs the
# dependency sources a Windows build fetched, and the cross compilers (Ubuntu:
# g++-i686-linux-gnu g++-aarch64-linux-gnu); for example, from WSL:
#   wsl bash tools/linux_syntax_check.sh /mnt/c/path/to/brack/build [x64|x86|arm64 ...]
# Nothing is linked and no Linux libraries are needed: OpenGL is replaced by a stand-in header.
#
# WSL reads files on the Windows side (/mnt/c) several times slower than its own, and every
# file includes large headers, so what the compiler reads is mirrored first (rsync, into
# BRACK_SYNTAX_CACHE, by default ~/.cache/brack-syntax-check) and compiled there, one job per CPU.
set -u
repo="$(cd "$(dirname "$0")/.." && pwd)"
deps="${1:-$repo/build}/_deps"
shift $(( $# > 0 ? 1 : 0 ))
archs=("$@")
[ ${#archs[@]} -eq 0 ] && archs=(x64 x86 arm64)
if [ ! -d "$deps/clap-src" ]; then
  echo "no fetched dependencies in $deps (configure a build first)" >&2
  exit 2
fi

cache="${BRACK_SYNTAX_CACHE:-$HOME/.cache/brack-syntax-check}"
mkdir -p "$cache/repo" "$cache/deps" "$cache/stub/GL"
rsync -a --delete --exclude .git "$repo/src" "$repo/include" "$repo/tests" "$cache/repo/"
rsync -a --delete --exclude .git "$deps/clap-src" "$deps/json-src" "$deps/vst2sdk-src" "$deps/vst3sdk" \
      "$deps/miniaudio-src" "$deps/r8brain-src" "$deps/imgui-src" "$deps/glfw-src" "$cache/deps/"
cat > "$cache/stub/GL/gl.h" <<'EOF'
#pragma once
typedef unsigned int GLbitfield;
typedef int GLint;
typedef int GLsizei;
typedef float GLfloat;
#define GL_COLOR_BUFFER_BIT 0x4000
void glViewport(GLint, GLint, GLsizei, GLsizei);
void glClearColor(GLfloat, GLfloat, GLfloat, GLfloat);
void glClear(GLbitfield);
EOF

src="$cache/repo" d="$cache/deps"
flags=(-std=c++20 -fsyntax-only -Wall -Wextra -Wno-unused-parameter -D__cdecl= -DBRACK_BUILDING_DLL '-DBRACK_VERSION="0"'
       -I "$src/src/core" -I "$src/src" -I "$src/include" -isystem "$cache/stub"
       -isystem "$d/clap-src/include" -isystem "$d/json-src/include" -isystem "$d/vst2sdk-src/include"
       -isystem "$d/vst3sdk" -isystem "$d/miniaudio-src" -isystem "$d/r8brain-src"
       -isystem "$d/imgui-src" -isystem "$d/imgui-src/backends" -isystem "$d/imgui-src/misc/cpp"
       -isystem "$d/glfw-src/include")

files=$(cd "$repo" && git ls-files --cached --others --exclude-standard 'src/*.cpp' 'tests/test_portable.cpp' \
          'tests/test_api_parity.cpp' | grep -v -e '_win32\.cpp$' -e '/win/' -e '_mac\.cpp$' -e '/coremidi/')
results="$(mktemp -d)"
trap 'rm -rf "$results"' EXIT

failed=0
declare -A compiler
for arch in "${archs[@]}"; do
  case "$arch" in
    x64) cxx=x86_64-linux-gnu-g++ ;;
    x86) cxx=i686-linux-gnu-g++ ;;
    arm64) cxx=aarch64-linux-gnu-g++ ;;
    *) echo "unknown architecture: $arch (x64, x86 or arm64)" >&2; exit 2 ;;
  esac
  if ! command -v "$cxx" > /dev/null; then
    echo "$arch: $cxx is not installed" >&2
    failed=1
    continue
  fi
  compiler[$arch]=$cxx
done

# Every architecture's files at once; a file that fails leaves its messages in $results.
running=0
for arch in "${!compiler[@]}"; do
  for f in $files; do
    (
      out=$(cd "$src" && "${compiler[$arch]}" "${flags[@]}" "$f" 2>&1)
      if [ $? -ne 0 ] || grep -q "warning:" <<< "$out"; then
        { echo "FAIL  $arch  $f"; grep -E "error|warning" <<< "$out" | head -8; } > "$results/$arch.${f//\//_}.fail"
      fi
    ) &
    running=$((running + 1))
    if [ $running -ge "$(nproc)" ]; then
      wait -n
      running=$((running - 1))
    fi
  done
done
wait

total=$(wc -w <<< "$files")
for arch in "${archs[@]}"; do
  [ -n "${compiler[$arch]:-}" ] || continue
  fails=$(find "$results" -name "$arch.*.fail" | wc -l)
  find "$results" -name "$arch.*.fail" -exec cat {} +
  echo "$arch: $((total - fails)) of $total files compile cleanly (${compiler[$arch]})"
  [ "$fails" -eq 0 ] || failed=1
done
exit $failed
