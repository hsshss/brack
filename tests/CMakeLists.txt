# Test CLAP plugin (a .clap is a plain shared library with a different extension).
add_library(brack_test_synth MODULE test_synth/test_synth.cpp)
target_link_libraries(brack_test_synth PRIVATE brack_dep_clap)
set_target_properties(brack_test_synth PROPERTIES PREFIX "" SUFFIX ".clap" OUTPUT_NAME brack-test-synth)
# An X11 editor, as Brack has one (src/core/CMakeLists.txt), with libX11 loaded at run time too.
if(NOT WIN32 AND NOT APPLE)
  find_package(X11)
endif()
if(X11_FOUND AND NOT WIN32 AND NOT APPLE)
  target_compile_definitions(brack_test_synth PRIVATE BRACK_TEST_SYNTH_X11)
  target_include_directories(brack_test_synth PRIVATE ${X11_INCLUDE_DIR})
  target_link_libraries(brack_test_synth PRIVATE ${CMAKE_DL_LIBS})
endif()


# The same synth as a VST2 DLL and as a VST3 (a plain .vst3 file; the test builds a bundle).
add_library(brack_test_synth_vst2 MODULE test_vst2/test_vst2_synth.cpp)
target_link_libraries(brack_test_synth_vst2 PRIVATE brack_dep_vst2)
set_target_properties(brack_test_synth_vst2 PROPERTIES PREFIX "" OUTPUT_NAME brack-test-synth-vst2)

add_library(brack_test_synth_vst3 MODULE test_vst3/test_vst3_synth.cpp)
if(WIN32)
  target_sources(brack_test_synth_vst3 PRIVATE test_vst3/test_vst3_synth.def)
endif()
target_link_libraries(brack_test_synth_vst3 PRIVATE brack_dep_vst3)
set_target_properties(brack_test_synth_vst3 PROPERTIES PREFIX "" SUFFIX ".vst3" OUTPUT_NAME brack-test-synth)
if(APPLE)
  set_source_files_properties(test_synth/test_synth.cpp TARGET_DIRECTORY brack_test_synth PROPERTIES
                              LANGUAGE OBJCXX COMPILE_OPTIONS -fobjc-arc)
  target_link_libraries(brack_test_synth PRIVATE "-framework AppKit")
endif()

# macOS plugins are bundles: Name.clap/Contents/MacOS/Name, with an Info.plist naming the binary.
if(APPLE)
  set_target_properties(brack_test_synth PROPERTIES BUNDLE TRUE BUNDLE_EXTENSION clap SUFFIX "")
  set_target_properties(brack_test_synth_vst2 PROPERTIES BUNDLE TRUE BUNDLE_EXTENSION vst)
  set_target_properties(brack_test_synth_vst3 PROPERTIES BUNDLE TRUE BUNDLE_EXTENSION vst3 SUFFIX "")
  set(BRACK_TEST_SYNTH "$<TARGET_BUNDLE_DIR:brack_test_synth>")
  set(BRACK_TEST_SYNTH_VST2 "$<TARGET_BUNDLE_DIR:brack_test_synth_vst2>")
  set(BRACK_TEST_SYNTH_VST3 "$<TARGET_BUNDLE_DIR:brack_test_synth_vst3>")
else()
  set(BRACK_TEST_SYNTH "$<TARGET_FILE:brack_test_synth>")
  set(BRACK_TEST_SYNTH_VST2 "$<TARGET_FILE:brack_test_synth_vst2>")
  set(BRACK_TEST_SYNTH_VST3 "$<TARGET_FILE:brack_test_synth_vst3>")
endif()

if(MSVC)
  # The CLAP synth is brack-test-synth too: keep the PDBs, .ilk and links apart, as for brack.exe
  # (src/gui/CMakeLists.txt).
  set_target_properties(brack_test_synth_vst3 PROPERTIES PDB_NAME brack-test-synth-vst3)
  target_link_options(brack_test_synth_vst3 PRIVATE /INCREMENTAL:NO)
  add_dependencies(brack_test_synth_vst3 brack_test_synth)
endif()

add_executable(test_resampler test_resampler.cpp)
target_link_libraries(test_resampler PRIVATE brack_core)
add_test(NAME resampler COMMAND test_resampler)

add_executable(test_frame_map test_frame_map.cpp)
target_link_libraries(test_frame_map PRIVATE brack_core)
add_test(NAME frame_map COMMAND test_frame_map)

add_executable(test_arrival_clock test_arrival_clock.cpp)
target_link_libraries(test_arrival_clock PRIVATE brack_core)
add_test(NAME arrival_clock COMMAND test_arrival_clock)

add_executable(test_callback_cadence test_callback_cadence.cpp)
target_link_libraries(test_callback_cadence PRIVATE brack_core)
add_test(NAME callback_cadence COMMAND test_callback_cadence)

add_executable(test_host_thread test_host_thread.cpp)
target_link_libraries(test_host_thread PRIVATE brack_core)
add_test(NAME host_thread COMMAND test_host_thread)

# Linux: MIDI ports on the ALSA sequencer, where Brack has them (src/core/CMakeLists.txt).
if(NOT WIN32 AND NOT APPLE)
  find_package(ALSA)
  if(ALSA_FOUND)
    add_executable(test_alsa_midi test_alsa_midi.cpp)
    target_link_libraries(test_alsa_midi PRIVATE brack_core ALSA::ALSA)
    add_test(NAME alsa_midi COMMAND test_alsa_midi)
  endif()
endif()

if(APPLE)
  add_executable(test_coremidi test_coremidi.cpp)
  target_link_libraries(test_coremidi PRIVATE brack_core "-framework CoreMIDI")
  add_test(NAME coremidi COMMAND test_coremidi)
endif()

add_executable(test_engine test_engine.cpp)
target_link_libraries(test_engine PRIVATE brack_core)
add_dependencies(test_engine brack_host brack_test_synth)
add_test(NAME engine COMMAND test_engine ${BRACK_TEST_SYNTH})

# What the guard around calls into plugins catches: SEH on Windows, signals elsewhere.
add_executable(test_crash test_crash.cpp)
target_link_libraries(test_crash PRIVATE brack_core)
add_dependencies(test_crash brack_host brack_test_synth brack_test_synth_vst3)
add_test(NAME crash COMMAND test_crash ${BRACK_TEST_SYNTH} ${BRACK_TEST_SYNTH_VST3})

# VST2 and VST3 end to end; makes a VST3 bundle and fakes other architectures' plugins.
add_executable(test_formats test_formats.cpp)
target_link_libraries(test_formats PRIVATE brack_core)
add_dependencies(test_formats brack_host brack_test_synth_vst2 brack_test_synth_vst3 brack)
add_test(NAME formats COMMAND test_formats ${BRACK_TEST_SYNTH_VST2} ${BRACK_TEST_SYNTH_VST3})

# Plugin host processes: what only they contain, the same sound, ending with Brack.
add_executable(test_isolation test_isolation.cpp)
target_link_libraries(test_isolation PRIVATE brack_core)
if(APPLE)
  set_source_files_properties(test_isolation.cpp TARGET_DIRECTORY test_isolation PROPERTIES
                              LANGUAGE OBJCXX COMPILE_OPTIONS -fobjc-arc)
  target_link_libraries(test_isolation PRIVATE "-framework AppKit" "-framework CoreGraphics")
endif()
if(X11_FOUND AND NOT WIN32 AND NOT APPLE)
  target_compile_definitions(test_isolation PRIVATE BRACK_TEST_X11)
  target_link_libraries(test_isolation PRIVATE X11::X11)
endif()
add_dependencies(test_isolation brack_host brack_test_synth brack_test_synth_vst2 brack_test_synth_vst3)
set(_other_synths)
foreach(_bin IN LISTS BRACK_OTHER_ARCH_BINS)
  list(APPEND _other_synths "${_bin}/brack-test-synth.clap")
endforeach()
add_test(NAME isolation COMMAND test_isolation ${BRACK_TEST_SYNTH} ${BRACK_TEST_SYNTH_VST2}
                                ${BRACK_TEST_SYNTH_VST3} ${_other_synths})

# Exercises brack.dll (libbrack.so) through its public C header only.
add_executable(test_dll test_dll.c)
target_link_libraries(test_dll PRIVATE brack)
if(APPLE)
  target_link_libraries(test_dll PRIVATE "-framework CoreFoundation")
endif()
add_dependencies(test_dll brack_host brack_test_synth brack_test_synth_vst2 brack_test_synth_vst3)
add_test(NAME dll COMMAND test_dll ${BRACK_TEST_SYNTH} ${BRACK_TEST_SYNTH_VST2} ${BRACK_TEST_SYNTH_VST3}
                          ${BRACK_TEST_OTHER_BINS})
# A universal build's x64 half too, through Rosetta 2.
if(BRACK_UNIVERSAL AND EXISTS "/Library/Apple/usr/libexec/oah/libRosettaRuntime")
  add_test(NAME dll_x64 COMMAND arch -x86_64 $<TARGET_FILE:test_dll> ${BRACK_TEST_SYNTH} ${BRACK_TEST_SYNTH_VST2}
                                ${BRACK_TEST_SYNTH_VST3} ${BRACK_TEST_OTHER_BINS})
endif()

# The per-OS plugin file rules and UTF-16 text, for every OS.
add_executable(test_portable test_portable.cpp)
target_link_libraries(test_portable PRIVATE brack_core)
add_test(NAME portable COMMAND test_portable)

# Every public engine method is in the C API, or listed as deliberately left out.
add_executable(test_api_parity test_api_parity.cpp)
add_test(NAME api_parity COMMAND test_api_parity ${CMAKE_SOURCE_DIR}/src/core/engine.h
                                 ${CMAKE_SOURCE_DIR}/include/brack/brack.h
                                 ${CMAKE_SOURCE_DIR}/bindings/dotnet/Brack/Native/NativeMethods.cs)

# The .NET binding (bindings/dotnet), through brack.dll (libbrack.so, libbrack.dylib), when a .NET SDK
# of this build's architecture is installed: its dotnet loads no other (a 32-bit build's, an x64
# build's on Apple silicon).
find_program(BRACK_DOTNET dotnet)
if(BRACK_DOTNET)
  execute_process(COMMAND ${BRACK_DOTNET} --info OUTPUT_VARIABLE _info ERROR_QUIET)
  # The architecture follows the RID's last hyphen: linux-arm64, or a distribution's own .NET's
  # ubuntu.26.04-arm64.
  string(REGEX MATCH "RID:[ \t]+[^ \t\r\n]*-([a-z0-9]+)" _rid "${_info}")
  set(BRACK_DOTNET_ARCH "${CMAKE_MATCH_1}")
endif()
if(BRACK_DOTNET AND (BRACK_DOTNET_ARCH STREQUAL BRACK_ARCH OR BRACK_UNIVERSAL))
  add_test(NAME dotnet
           COMMAND ${BRACK_DOTNET} run --project ${CMAKE_SOURCE_DIR}/bindings/dotnet/Brack.Tests -c Release
                   -- $<TARGET_FILE:brack> ${BRACK_TEST_SYNTH})
  set_tests_properties(dotnet PROPERTIES TIMEOUT 600)
endif()

# Publishes a virtual port and sends to it through WinMM like any other app would.
# Manual only (not registered with ctest): on MidiSrv builds without the late-2026
# servicing fix, removing a virtual device can hang the MIDI service until reboot.
if(WIN32)
  add_executable(test_virtual_midi test_virtual_midi.cpp)
  target_link_libraries(test_virtual_midi PRIVATE brack_core winmm)
  add_dependencies(test_virtual_midi brack_test_synth)
endif()
