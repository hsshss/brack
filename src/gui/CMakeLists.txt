add_executable(brack_gui WIN32 main.cpp settings.cpp theme.cpp widgets.cpp)
set_target_properties(brack_gui PROPERTIES OUTPUT_NAME brack)
if(MSVC)
  # brack.exe sits next to brack.dll, so keep the linker's per-target files apart:
  # its own PDB name, and no incremental linking (the .ilk would be brack.ilk too). Even so the
  # linker clears a stale brack.ilk, so the two must not link at the same time.
  set_target_properties(brack_gui PROPERTIES PDB_NAME brack-gui)
  target_link_options(brack_gui PRIVATE /INCREMENTAL:NO)
  add_dependencies(brack_gui brack)
endif()
target_link_libraries(brack_gui PRIVATE brack_core brack_dep_imgui brack_dep_json)
if(WIN32)
  target_sources(brack_gui PRIVATE file_dialog_win32.cpp brack.rc)
  target_link_libraries(brack_gui PRIVATE comdlg32 shell32 ole32)
elseif(APPLE)
  target_sources(brack_gui PRIVATE file_dialog_mac.mm)
  set_source_files_properties(file_dialog_mac.mm PROPERTIES COMPILE_OPTIONS -fobjc-arc)
  target_link_libraries(brack_gui PRIVATE "-framework AppKit" "-framework UniformTypeIdentifiers")
  target_compile_definitions(brack_gui PRIVATE GL_SILENCE_DEPRECATION)
else()
  target_sources(brack_gui PRIVATE file_dialog_posix.cpp)
endif()
add_dependencies(brack_gui brack_host)  # runs the plugins
if(APPLE)
  # CMake writes the bundle's Info.plist only when it configures, so a bundle moved away (dragged
  # to /Applications) would come back from the next build without one: the build copies it in too.
  set(MACOSX_BUNDLE_EXECUTABLE_NAME Brack)
  set(MACOSX_BUNDLE_BUNDLE_NAME Brack)
  set(MACOSX_BUNDLE_GUI_IDENTIFIER io.github.hsshss.brack)
  set(MACOSX_BUNDLE_SHORT_VERSION_STRING ${PROJECT_VERSION})
  set(MACOSX_BUNDLE_BUNDLE_VERSION ${PROJECT_VERSION})
  set(MACOSX_BUNDLE_ICON_FILE brack.icns)
  configure_file(${PROJECT_SOURCE_DIR}/cmake/Info.plist.in ${CMAKE_CURRENT_BINARY_DIR}/Info.plist)
  set_target_properties(brack_gui PROPERTIES
    OUTPUT_NAME Brack
    MACOSX_BUNDLE TRUE
    MACOSX_BUNDLE_INFO_PLIST ${CMAKE_CURRENT_BINARY_DIR}/Info.plist)
  target_sources(brack_gui PRIVATE icon/brack.icns)
  set_source_files_properties(icon/brack.icns PROPERTIES MACOSX_PACKAGE_LOCATION Resources)
  set(_names)
  foreach(_arch IN LISTS BRACK_BUNDLED_HOSTS)
    list(APPEND _names brack-host-${_arch})
  endforeach()
  string(REPLACE ";" "|" _names "${_names}")
  add_custom_target(brack_gui_hosts ALL
    COMMAND ${CMAKE_COMMAND} -DDIRS=${CMAKE_RUNTIME_OUTPUT_DIRECTORY} "-DHOSTS=${_names}"
            "-DDST=$<TARGET_BUNDLE_CONTENT_DIR:brack_gui>/MacOS" -DOWN= -DSUFFIX=
            -P ${PROJECT_SOURCE_DIR}/cmake/copy_plugin_hosts.cmake
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_CURRENT_BINARY_DIR}/Info.plist
            $<TARGET_BUNDLE_CONTENT_DIR:brack_gui>/Info.plist
    VERBATIM)
  if(BRACK_CODESIGN_IDENTITY)  # the bundle, its plugin hosts in it, last
    add_custom_command(TARGET brack_gui_hosts POST_BUILD
      COMMAND ${CMAKE_COMMAND} "-DDIR=$<TARGET_BUNDLE_CONTENT_DIR:brack_gui>/MacOS" "-DIDENTITY=${BRACK_CODESIGN_IDENTITY}"
              -DENTITLEMENTS=${PROJECT_SOURCE_DIR}/cmake/brack.entitlements -P ${PROJECT_SOURCE_DIR}/cmake/codesign_hosts.cmake
      COMMAND codesign --force --sign "${BRACK_CODESIGN_IDENTITY}" --options runtime
              --entitlements ${PROJECT_SOURCE_DIR}/cmake/brack.entitlements $<TARGET_BUNDLE_DIR:brack_gui>
      VERBATIM)
  endif()
  add_dependencies(brack_gui_hosts brack_gui brack_host)
  if(TARGET brack_host_other)
    add_dependencies(brack_gui_hosts brack_host_other)
  endif()
endif()
