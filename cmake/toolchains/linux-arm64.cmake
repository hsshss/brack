# Linux ARM64 from an x64 machine with Ubuntu's cross compiler (g++-aarch64-linux-gnu):
#   cmake -S . -B build-arm64 -G Ninja --toolchain cmake/toolchains/linux-arm64.cmake
# ctest runs the tests through qemu-user; set QEMU_LD_PREFIX=/usr/aarch64-linux-gnu so that the
# programs they start (the plugin host) find the ARM64 C library too.
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)
set(CMAKE_C_COMPILER aarch64-linux-gnu-gcc)
set(CMAKE_CXX_COMPILER aarch64-linux-gnu-g++)
set(CMAKE_FIND_ROOT_PATH /usr/aarch64-linux-gnu)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_CROSSCOMPILING_EMULATOR qemu-aarch64)
