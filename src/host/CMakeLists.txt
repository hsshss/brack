# The plugin host process, named for its architecture (brack-host-x64.exe, brack-host-x86.exe):
# Brack starts the one matching a plugin's binary.
add_executable(brack_host main.cpp)
set_target_properties(brack_host PROPERTIES OUTPUT_NAME brack-host-${BRACK_ARCH})
target_link_libraries(brack_host PRIVATE brack_core)
brack_codesign(brack_host)
if(BRACK_UNIVERSAL)
  # One thin plugin host per architecture, as Brack starts them by name.
  set_target_properties(brack_host PROPERTIES OSX_ARCHITECTURES arm64)
  add_executable(brack_host_x64 main.cpp)
  set_target_properties(brack_host_x64 PROPERTIES OUTPUT_NAME brack-host-x64 OSX_ARCHITECTURES x86_64)
  target_link_libraries(brack_host_x64 PRIVATE brack_core)
  brack_codesign(brack_host_x64)
  add_dependencies(brack_host brack_host_x64)
endif()
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  # Stands in for the plugins' pthread_create: exported, so that their calls come here.
  target_sources(brack_host PRIVATE plugin_threads_linux.cpp)
  target_link_libraries(brack_host PRIVATE brack_dep_json)  # remote/brack_link.h
  target_link_options(brack_host PRIVATE -Wl,--export-dynamic-symbol=pthread_create)
  if(CMAKE_CROSSCOMPILING AND BRACK_ARCH MATCHES "^x(86|64)$")
    # Cross-built for an ARM64 Brack, which runs them on FEX-Emu with the user's rootfs, whose
    # glibc may be older than the cross compiler's: Ubuntu 24.04's (2.39) at least.
    add_custom_command(TARGET brack_host POST_BUILD
      COMMAND ${CMAKE_COMMAND} -DREADELF=${CMAKE_READELF} -DNEWEST=39 -DFILE=$<TARGET_FILE:brack_host>
              -P ${PROJECT_SOURCE_DIR}/cmake/check_glibc_needs.cmake
      VERBATIM)
  endif()
endif()

# The other plugin hosts this build ships (BRACK_BUNDLED_HOSTS) come from the builds for those
# architectures (BRACK_OTHER_ARCH_BINS), beside this build's binaries, so that this Brack also
# runs those architectures' plugins. Hosts it does not ship are removed from there.
set(_others ${BRACK_BUNDLED_HOSTS})
list(REMOVE_ITEM _others ${BRACK_ARCH})
if(BRACK_OTHER_ARCH_BINS)
  set(_names)
  foreach(_arch IN LISTS _others)
    list(APPEND _names brack-host-${_arch}${CMAKE_EXECUTABLE_SUFFIX})
  endforeach()
  string(REPLACE ";" "|" _dirs "${BRACK_OTHER_ARCH_BINS}")
  string(REPLACE ";" "|" _names "${_names}")
  add_custom_target(brack_host_other ALL
    COMMAND ${CMAKE_COMMAND} "-DDIRS=${_dirs}" "-DHOSTS=${_names}" -DDST=${CMAKE_RUNTIME_OUTPUT_DIRECTORY}
            -DOWN=brack-host-${BRACK_ARCH}${CMAKE_EXECUTABLE_SUFFIX} -DSUFFIX=${CMAKE_EXECUTABLE_SUFFIX}
            -P ${PROJECT_SOURCE_DIR}/cmake/copy_plugin_hosts.cmake
    VERBATIM)
  add_dependencies(brack_host brack_host_other)
endif()
