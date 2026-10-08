# Linux x86 (32-bit) on x64 Linux with the system's own compiler (-m32) and its 32-bit libraries,
# installed beside the 64-bit ones as RHEL-like systems do (AlmaLinux 8: glibc-devel.i686,
# libX11-devel.i686, and gcc-toolset-14-libstdc++-devel.i686 for gcc-toolset-14):
#   cmake -S . -B build-x86 -G Ninja --toolchain cmake/toolchains/linux-x86-m32.cmake
# Not a cross build: CMake sees an x86_64 system with 4-byte pointers, which CMakeLists.txt counts as x86.
set(CMAKE_C_FLAGS_INIT -m32)
set(CMAKE_CXX_FLAGS_INIT -m32)
