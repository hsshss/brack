# Linux x86 (32-bit) from an x64 or ARM64 machine with Ubuntu's cross compiler (g++-i686-linux-gnu):
#   cmake -S . -B build-x86 -G Ninja --toolchain cmake/toolchains/linux-x86.cmake
# The results run on x64 Linux with the 32-bit C library (Ubuntu: libc6-i386).
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR i686)
set(CMAKE_C_COMPILER i686-linux-gnu-gcc)
set(CMAKE_CXX_COMPILER i686-linux-gnu-g++)
set(CMAKE_FIND_ROOT_PATH /usr/i686-linux-gnu)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
