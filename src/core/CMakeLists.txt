add_library(brack_core STATIC
  util/common.cpp
  audio/resampler.cpp
  audio/audio_output.cpp
  plugin/plugin.cpp
  plugin/plugin_files.cpp
  plugin/plugin_scan.cpp
  plugin/scan_cache.cpp
  plugin/library.cpp
  plugin/binary_info.cpp
  clap/clap_module.cpp
  clap/clap_instance.cpp
  clap/clap_editor.cpp
  vst2/vst2_plugin.cpp
  vst3/vst3_plugin.cpp
  engine.cpp
  session.cpp
  report.cpp
  remote/host_protocol.cpp
  remote/remote_plugin.cpp
  remote/plugin_host.cpp
)
target_include_directories(brack_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(brack_core PUBLIC BRACK_VERSION="${BRACK_VERSION}")
target_link_libraries(brack_core
  PUBLIC brack_dep_clap
  PRIVATE brack_dep_json brack_dep_miniaudio brack_dep_r8brain brack_dep_vst2 brack_dep_vst3)

if(NOT APPLE)
  target_sources(brack_core PRIVATE util/main_thread.cpp)
endif()
if(WIN32)
  target_sources(brack_core PRIVATE
    host_thread_win32.cpp
    plugin/host_window_win32.cpp
    plugin/library_win32.cpp
    midi/win/winmm_input.cpp
    midi/win/virtual_input.cpp
    midi/win/midisrv_virtual_port.cpp
    remote/brack_link_win32.cpp
    remote/host_process_win32.cpp
    util/realtime_win32.cpp)
  target_link_libraries(brack_core PRIVATE winmm ole32 oleaut32 rpcrt4 shell32 user32 advapi32 cfgmgr32 avrt)
elseif(APPLE)
  target_sources(brack_core PRIVATE
    host_thread_mac.cpp
    util/main_thread_mac.mm
    plugin/host_window_mac.mm
    plugin/library_posix.cpp
    midi/coremidi/coremidi_input.cpp
    remote/brack_link_posix.cpp
    remote/host_process_posix.cpp
    util/realtime_mac.cpp)
  set_source_files_properties(plugin/host_window_mac.mm PROPERTIES COMPILE_OPTIONS -fobjc-arc)
  target_link_libraries(brack_core PUBLIC "-framework CoreFoundation" "-framework Foundation" "-framework AppKit")
  target_link_libraries(brack_core PRIVATE "-framework CoreMIDI" "-framework AudioToolbox")
else()
  # Linux (docs/design-notes.md, chapter 13):
  #   host_thread_posix.cpp          a poll() loop, with the descriptors of X11 connections and plugins
  #   plugin/host_window_x11.cpp     editors, where the X11 development files are; else
  #   plugin/host_window_posix.cpp   no editors
  #   midi/alsa/                     MIDI ports on the ALSA sequencer, where its development files are; else
  #   midi/posix/                    no ports
  #   plugin/library_posix.cpp       calls into plugins guarded with signals
  #   util/realtime_linux.cpp        real-time audio threads
  #   util/dbus_linux.cpp            libdbus, loaded when needed (RealtimeKit, the GUI's file dialogs)
  # libX11 and libasound are loaded when needed too, not linked: only their headers are used here.
  find_package(X11)
  if(X11_FOUND)
    target_sources(brack_core PRIVATE plugin/host_window_x11.cpp)
    target_include_directories(brack_core PRIVATE ${X11_INCLUDE_DIR})
  else()
    target_sources(brack_core PRIVATE plugin/host_window_posix.cpp)
  endif()
  find_package(ALSA)
  if(ALSA_FOUND)
    target_sources(brack_core PRIVATE midi/alsa/alsa_seq_input.cpp)
    target_include_directories(brack_core PRIVATE ${ALSA_INCLUDE_DIRS})
  else()
    target_sources(brack_core PRIVATE midi/posix/midi_unavailable.cpp)
  endif()
  target_sources(brack_core PRIVATE
    host_thread_posix.cpp
    plugin/library_posix.cpp
    remote/brack_link_posix.cpp
    remote/host_process_posix.cpp
    util/dbus_linux.cpp
    util/realtime_linux.cpp)
endif()
if(NOT WIN32)
  find_package(Threads REQUIRED)
  target_link_libraries(brack_core PUBLIC Threads::Threads ${CMAKE_DL_LIBS})
  find_library(BRACK_LIBRT rt)
  if(BRACK_LIBRT)
    target_link_libraries(brack_core PUBLIC ${BRACK_LIBRT})  # shm_open, before glibc 2.34
  endif()
endif()
