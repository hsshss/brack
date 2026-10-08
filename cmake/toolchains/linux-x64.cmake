# Linux x64 from an ARM64 machine with Ubuntu's cross compiler (g++-x86-64-linux-gnu):
#   cmake -S . -B build-x64 -G Ninja --toolchain cmake/toolchains/linux-x64.cmake
# The tests run where the kernel hands x64 programs to an emulator (FEX-Emu's binfmt_misc entry).
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR x86_64)
set(CMAKE_C_COMPILER x86_64-linux-gnu-gcc)
set(CMAKE_CXX_COMPILER x86_64-linux-gnu-g++)
set(CMAKE_FIND_ROOT_PATH /usr/x86_64-linux-gnu)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
