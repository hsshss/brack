cmake_minimum_required(VERSION 3.25)
# macOS 14.4: plugin hosts wait on shared memory with os_sync_wait_on_address (remote/futex_posix.h).
set(CMAKE_OSX_DEPLOYMENT_TARGET 14.4 CACHE STRING "The oldest macOS Brack runs on")
# The version brack_version_string() and the CLAP host report: a release passes its tag without
# the "v" (.github/workflows/release.yml). Not cached unless given, so a build folder follows a
# change of the default here.
if(NOT DEFINED BRACK_VERSION)
  set(BRACK_VERSION 1.0.0)
endif()
if(NOT BRACK_VERSION MATCHES "^([0-9]+\\.[0-9]+\\.[0-9]+)(-[0-9A-Za-z.-]+)?$")
  message(FATAL_ERROR "BRACK_VERSION '${BRACK_VERSION}' is not a version (1.2.0, or 1.2.0-beta.1)")
endif()
project(brack VERSION ${CMAKE_MATCH_1} LANGUAGES C CXX)
if(APPLE)
  enable_language(OBJC OBJCXX)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

# On Linux the GUI is built where GLFW's X11 backend has its development files (Ubuntu:
# libx11-dev libxrandr-dev libxinerama-dev libxcursor-dev libxi-dev libgl-dev); under Wayland it
# runs in Xwayland, as plugin editors do (src/core/plugin/host_window_x11.cpp).
if(WIN32)
  set(_gui_default ON)
elseif(APPLE)
  set(_gui_default ON)
else()
  # libGL.so.1, which every Linux desktop has, rather than GLVND's libOpenGL.so.0 and libGLX.so.0,
  # which systems without GLVND lack (also for dependencies.cmake's find_package).
  set(OpenGL_GL_PREFERENCE LEGACY)
  find_package(X11)
  find_package(OpenGL)
  if(X11_FOUND AND X11_Xrandr_FOUND AND X11_Xinerama_FOUND AND X11_Xcursor_FOUND AND X11_Xi_FOUND
     AND X11_Xkb_FOUND AND X11_Xshape_FOUND AND OPENGL_FOUND)
    set(_gui_default ON)
  else()
    set(_gui_default OFF)
    message(STATUS "Brack: no GUI (the X11 and OpenGL development files GLFW needs are missing)")
  endif()
endif()
option(BRACK_BUILD_GUI   "Build the standalone GUI (Dear ImGui + GLFW)" ${_gui_default})
option(BRACK_BUILD_CLI   "Build the command line tool" ON)
option(BRACK_BUILD_TESTS "Build tests and the test CLAP plugin" ON)

if(MSVC)
  add_compile_options(/utf-8 /W4 /permissive- /Zc:__cplusplus /wd4100 /wd4505)
  add_compile_definitions(UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)
  # Static CRT keeps brack.dll self-contained for host applications.
  set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
else()
  add_compile_options(-Wall -Wextra -Wno-unused-parameter)
  if(NOT APPLE)
    # Each binary carries its own C++ runtime, as the Windows ones do (the static CRT): a plugin
    # host of another architecture then needs only that architecture's C library.
    add_link_options(-static-libstdc++ -static-libgcc)
    # ... without exporting it from libbrack.so or a plugin, where it would interpose on the
    # application's own libstdc++.
    add_link_options(-Wl,--exclude-libs,ALL)
    # Each binary needs only the libraries it uses. Ubuntu's g++ does so by default, RHEL's does
    # not.
    add_link_options(-Wl,--as-needed)
    # x86: binutils 2.46 marks what calls ___tls_get_addr as needing glibc 2.43's TLS fix
    # (GLIBC_ABI_GNU_TLS), which no older C library loads (Ubuntu 24.04's 2.39). The fix keeps SSE
    # registers across the call; Brack's x86 code uses none, and programs make no such call.
    include(CheckLinkerFlag)
    check_linker_flag(CXX -Wl,--no-gnu-tls-tag BRACK_LINKER_NO_GNU_TLS_TAG)
    if(BRACK_LINKER_NO_GNU_TLS_TAG)
      add_link_options(-Wl,--no-gnu-tls-tag)
    endif()
  endif()
  # Nor the template code the compiler makes inline in Brack's own files.
  set(CMAKE_VISIBILITY_INLINES_HIDDEN ON)
endif()

include(cmake/dependencies.cmake)

# macOS: an identity ("Developer ID Application: ...", or "-" for an ad-hoc signature) signs the
# programs, plugin hosts and libbrack with the Hardened Runtime, with the entitlements plugins need
# there (cmake/brack.entitlements): others' code to load, and code made at run time. Unset, the
# linker's ad-hoc signature stays, without the Hardened Runtime.
if(APPLE)
  set(BRACK_CODESIGN_IDENTITY "" CACHE STRING "macOS code signing identity (Hardened Runtime); empty: none")
endif()
function(brack_codesign target)
  if(APPLE AND BRACK_CODESIGN_IDENTITY)
    add_custom_command(TARGET ${target} POST_BUILD
      COMMAND codesign --force --sign "${BRACK_CODESIGN_IDENTITY}" --options runtime
              --entitlements ${PROJECT_SOURCE_DIR}/cmake/brack.entitlements $<TARGET_FILE:${target}>
      VERBATIM)
  endif()
endfunction()

# The architecture of this build, named as buildArchitecture() names it: x64, x86 or arm64.
# macOS builds universal programs too (CMAKE_OSX_ARCHITECTURES "arm64;x86_64"): each half is its
# architecture's build, and the plugin hosts are made thin, one per architecture (src/host).
# BRACK_ARCH then names the arm64 half.
set(BRACK_UNIVERSAL OFF)
if(MSVC)
  set(_arch "${CMAKE_CXX_COMPILER_ARCHITECTURE_ID}")
elseif(APPLE AND CMAKE_OSX_ARCHITECTURES)
  set(_arch "${CMAKE_OSX_ARCHITECTURES}")
  list(SORT _arch)
  if(_arch STREQUAL "arm64;x86_64")
    set(BRACK_UNIVERSAL ON)
    set(_arch arm64)
  endif()
else()
  set(_arch "${CMAKE_SYSTEM_PROCESSOR}")
endif()
string(TOLOWER "${_arch}" _arch)
# ARM64EC code is x64 code to everything around it (an x64 process on an ARM64 PC loads it, and
# it loads x64 libraries), so an ARM64EC build counts as x64 (tools/arm64ec_synth.ps1).
if(_arch MATCHES "^(x64|amd64|x86_64|arm64ec)$")
  set(BRACK_ARCH x64)
elseif(_arch MATCHES "^(x86|i[3-6]86)$")
  set(BRACK_ARCH x86)
elseif(_arch MATCHES "^(arm64|aarch64)$")
  set(BRACK_ARCH arm64)
else()
  message(FATAL_ERROR "Brack builds for x64, x86 or arm64 (or, on macOS, arm64 and x86_64 together), not for '${_arch}'")
endif()
if(BRACK_ARCH STREQUAL "x64" AND CMAKE_SIZEOF_VOID_P EQUAL 4)
  set(BRACK_ARCH x86)  # 32-bit code from a 64-bit system's compiler (-m32)
endif()

# Builds for the other architectures, in folders beside this one, lend this build their plugin
# hosts (src/host): it then runs plugins of their architectures too.
if(APPLE)
  set(_dirs build build-x64 build-arm64)
elseif(WIN32)
  set(_dirs build build-x86 build-arm64)
else()
  set(_dirs build build-x64 build-x86 build-arm64)  # build-x64 beside an ARM64 machine's build
endif()
# test_dll mixes in the test synths of those builds, and on macOS of a universal one.
set(_test_dirs ${_dirs})
if(APPLE)
  list(APPEND _test_dirs build-universal)
endif()
set(BRACK_TEST_OTHER_BINS)
foreach(_dir IN LISTS _test_dirs)
  if(NOT "${PROJECT_SOURCE_DIR}/${_dir}" STREQUAL "${CMAKE_BINARY_DIR}")
    list(APPEND BRACK_TEST_OTHER_BINS "${PROJECT_SOURCE_DIR}/${_dir}/bin")
  endif()
endforeach()
if(BRACK_UNIVERSAL)
  set(BRACK_OTHER_ARCH_BINS "")  # it makes every plugin host it ships
elseif(WIN32 OR APPLE OR CMAKE_SYSTEM_NAME STREQUAL "Linux")
  set(_bins)
  foreach(_dir IN LISTS _dirs)
    if(NOT "${PROJECT_SOURCE_DIR}/${_dir}" STREQUAL "${CMAKE_BINARY_DIR}")
      list(APPEND _bins "${PROJECT_SOURCE_DIR}/${_dir}/bin")
    endif()
  endforeach()
  set(BRACK_OTHER_ARCH_BINS "${_bins}" CACHE STRING
      "bin folders of Brack builds for other architectures, whose plugin hosts this build uses")
endif()

# The plugin hosts a build ships beside its programs and brack.dll (libbrack.so): for its own
# architecture and those its OS runs too.
#   Windows x64 (and x86): x86, x64        Windows ARM64: x86, x64 (Windows' emulation), arm64
#   Linux x64: x86, x64                    Linux ARM64: x86, x64 (FEX-Emu), arm64
#   macOS Intel: x64                       macOS Apple silicon: x64 (Rosetta 2), arm64
if(APPLE AND BRACK_ARCH STREQUAL "arm64")
  set(BRACK_BUNDLED_HOSTS x64 arm64)
elseif((WIN32 OR CMAKE_SYSTEM_NAME STREQUAL "Linux") AND BRACK_ARCH STREQUAL "arm64")
  set(BRACK_BUNDLED_HOSTS x86 x64 arm64)
elseif(WIN32 OR (CMAKE_SYSTEM_NAME STREQUAL "Linux" AND BRACK_ARCH STREQUAL "x64"))
  set(BRACK_BUNDLED_HOSTS x86 x64)
else()
  set(BRACK_BUNDLED_HOSTS ${BRACK_ARCH})
endif()

add_subdirectory(src/core)
add_subdirectory(src/host)
add_subdirectory(src/dll)
if(BRACK_BUILD_CLI)
  add_subdirectory(src/cli)
endif()
if(BRACK_BUILD_GUI)
  add_subdirectory(src/gui)
endif()
if(BRACK_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
if(BRACK_BUILD_TESTS AND WIN32)
  add_subdirectory(tools)
endif()

# A distribution (cmake --install): the programs, brack.dll with its header and import library,
# and every plugin host it ships (BRACK_BUNDLED_HOSTS), which other architectures' builds lend.
# Windows keeps the plugin hosts in bin, beside brack.dll; Linux in libexec/brack, where the
# programs and libbrack.so look for them (pluginHostExecutable()). The license and the notices of
# the libraries compiled in go at the top of the archive, or in Linux's documentation folder.
if(WIN32)
  set(_hosts_dir bin)
else()
  set(_hosts_dir libexec/brack)
endif()
if(WIN32 OR APPLE)
  set(_docs_dir .)
else()
  set(_docs_dir share/doc/brack)
endif()
install(FILES LICENSE THIRD_PARTY_NOTICES.md DESTINATION ${_docs_dir})
install(TARGETS brack RUNTIME DESTINATION bin LIBRARY DESTINATION lib ARCHIVE DESTINATION lib)
install(TARGETS brack_host RUNTIME DESTINATION ${_hosts_dir})
install(FILES include/brack/brack.h DESTINATION include/brack)
if(BRACK_BUILD_CLI)
  install(TARGETS brack_cli RUNTIME DESTINATION bin)
endif()
if(BRACK_BUILD_GUI)
  install(TARGETS brack_gui RUNTIME DESTINATION bin BUNDLE DESTINATION .)
  if(NOT WIN32 AND NOT APPLE)
    foreach(_px 16 24 32 48 64 128 256 512)
      install(FILES src/gui/icon/png/brack-${_px}.png DESTINATION share/icons/hicolor/${_px}x${_px}/apps
              RENAME brack.png)
    endforeach()
    install(FILES src/gui/icon/brack.svg DESTINATION share/icons/hicolor/scalable/apps)
    install(FILES src/gui/brack.desktop DESTINATION share/applications)
  endif()
endif()
foreach(_arch IN LISTS BRACK_BUNDLED_HOSTS)
  if(NOT _arch STREQUAL BRACK_ARCH)
    # OPTIONAL: a build without the other architectures' builds still installs (and has warned).
    install(PROGRAMS "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/brack-host-${_arch}${CMAKE_EXECUTABLE_SUFFIX}"
            DESTINATION ${_hosts_dir} OPTIONAL)
  endif()
endforeach()
