#!/bin/bash
# tools/linux_release_build.sh <x64|arm64> <version> [<cmake option>...]
# Builds the Linux release of that architecture in its manylinux_2_28 container (AlmaLinux 8,
# glibc 2.28), so that it runs on any Linux with glibc 2.28 or newer, and installs it stripped
# into stage/brack-<version>-linux-<arch>. The options go to the configure of build.
#   x64:   build-x86 (-m32) for the x86 plugin host, then build, whose bin then holds the x64 and
#          x86 plugin hosts the ARM64 release ships too.
#   arm64: build; pass it a folder with those with -DBRACK_OTHER_ARCH_BINS=<the folder>.
# Mount the source at the same path, so that ctest can run outside the container afterwards:
#   docker run --rm -v "$PWD:$PWD" -w "$PWD" quay.io/pypa/manylinux_2_28_x86_64 tools/linux_release_build.sh x64 1.2.0
set -euo pipefail
arch=$1 version=$2
shift 2

dnf install -y alsa-lib-devel libX11-devel libXrandr-devel libXinerama-devel libXcursor-devel libXi-devel \
  mesa-libGL-devel
if [[ $arch == x64 ]]; then
  dnf install -y glibc-devel.i686 gcc-toolset-14-libstdc++-devel.i686 libX11-devel.i686 alsa-lib-devel.i686
fi
export PATH=$PATH:$HOME/.local/bin
command -v ninja > /dev/null || pipx install ninja

configure() {
  cmake -S . -B "$1" -G Ninja -DCMAKE_BUILD_TYPE=Release "-DBRACK_VERSION=$version" "${@:2}"
}
if [[ $arch == x64 ]]; then
  configure build-x86 --toolchain cmake/toolchains/linux-x86-m32.cmake
  cmake --build build-x86
fi
configure build "$@"
cmake --build build
cmake --install build --prefix "stage/brack-$version-linux-$arch" --strip
