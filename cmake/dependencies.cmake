include(FetchContent)

# All dependencies are fetched as plain sources; SOURCE_SUBDIR points to a
# non-existent directory so their own CMake projects are not added.
# To build offline, point FETCHCONTENT_SOURCE_DIR_<NAME> at local checkouts.
macro(brack_fetch name repo tag)
  FetchContent_Declare(${name} GIT_REPOSITORY https://github.com/${repo}.git GIT_TAG ${tag} GIT_SHALLOW TRUE
                       GIT_PROGRESS FALSE SOURCE_SUBDIR __brack_no_cmake__)
  FetchContent_MakeAvailable(${name})
endmacro()

brack_fetch(clap      free-audio/clap            1.2.10)
brack_fetch(r8brain   avaneev/r8brain-free-src   version-6.5)
brack_fetch(miniaudio mackron/miniaudio          0.11.25)
brack_fetch(json      nlohmann/json              v3.12.0)
brack_fetch(vst2sdk   Xaymar/vst2sdk             v0.4.0)

# pluginterfaces' headers include each other as "pluginterfaces/...", so the checkout goes
# into a folder of that name and its parent is the include directory. (An offline
# FETCHCONTENT_SOURCE_DIR_VST3_PLUGINTERFACES must also be a folder named pluginterfaces.)
FetchContent_Declare(vst3_pluginterfaces
  GIT_REPOSITORY https://github.com/steinbergmedia/vst3_pluginterfaces.git GIT_TAG v3.8.1_build_84
  GIT_SHALLOW TRUE GIT_PROGRESS FALSE
  SOURCE_DIR ${FETCHCONTENT_BASE_DIR}/vst3sdk/pluginterfaces SOURCE_SUBDIR __brack_no_cmake__)
FetchContent_MakeAvailable(vst3_pluginterfaces)

add_library(brack_dep_clap INTERFACE)
target_include_directories(brack_dep_clap SYSTEM INTERFACE ${clap_SOURCE_DIR}/include)

add_library(brack_dep_json INTERFACE)
target_include_directories(brack_dep_json SYSTEM INTERFACE ${json_SOURCE_DIR}/include)

add_library(brack_dep_vst2 INTERFACE)
target_include_directories(brack_dep_vst2 SYSTEM INTERFACE ${vst2sdk_SOURCE_DIR}/include)
# vst.h spells its calling convention __cdecl everywhere; only Windows compilers know it, and
# elsewhere the default convention is the one meant.
if(NOT WIN32)
  target_compile_definitions(brack_dep_vst2 INTERFACE __cdecl=)
endif()

# The FUnknown support code plus brack's interface-id definitions (also linked into the
# test VST3 plugin, which needs the same ids).
get_filename_component(BRACK_VST3_SDK_DIR ${vst3_pluginterfaces_SOURCE_DIR} DIRECTORY)
add_library(brack_dep_vst3 STATIC
  ${vst3_pluginterfaces_SOURCE_DIR}/base/funknown.cpp
  ${PROJECT_SOURCE_DIR}/src/core/vst3/vst3_iids.cpp)
target_include_directories(brack_dep_vst3 SYSTEM PUBLIC ${BRACK_VST3_SDK_DIR})
if(MSVC)
  target_compile_options(brack_dep_vst3 PRIVATE /W0)
endif()
if(APPLE)
  target_link_libraries(brack_dep_vst3 PUBLIC "-framework CoreFoundation")  # FUID::generate
endif()

add_library(brack_dep_miniaudio INTERFACE)
target_include_directories(brack_dep_miniaudio SYSTEM INTERFACE ${miniaudio_SOURCE_DIR})

add_library(brack_dep_r8brain STATIC ${r8brain_SOURCE_DIR}/r8bbase.cpp)
target_include_directories(brack_dep_r8brain SYSTEM PUBLIC ${r8brain_SOURCE_DIR})
if(MSVC)
  target_compile_options(brack_dep_r8brain PRIVATE /W0)
endif()

if(BRACK_BUILD_GUI)
  brack_fetch(imgui ocornut/imgui v1.92.9)
  FetchContent_Declare(glfw GIT_REPOSITORY https://github.com/glfw/glfw.git GIT_TAG 3.4 GIT_SHALLOW TRUE)
  set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
  set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
  set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
  set(GLFW_INSTALL OFF CACHE BOOL "" FORCE)
  set(GLFW_BUILD_WAYLAND OFF CACHE BOOL "" FORCE)  # X11 only: Xwayland under Wayland, as the editors
  FetchContent_MakeAvailable(glfw)
  if(NOT MSVC)
    target_compile_options(glfw PRIVATE -w)  # its own code: not held to Brack's warnings
  endif()

  find_package(OpenGL REQUIRED)
  add_library(brack_dep_imgui STATIC
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp
    ${imgui_SOURCE_DIR}/misc/cpp/imgui_stdlib.cpp)
  target_include_directories(brack_dep_imgui SYSTEM PUBLIC
    ${imgui_SOURCE_DIR} ${imgui_SOURCE_DIR}/backends ${imgui_SOURCE_DIR}/misc/cpp)
  target_link_libraries(brack_dep_imgui PUBLIC glfw OpenGL::GL)
  if(MSVC)
    target_compile_options(brack_dep_imgui PRIVATE /W0)
  endif()
endif()
