add_library(brack SHARED brack_api.cpp)
target_include_directories(brack PUBLIC ${PROJECT_SOURCE_DIR}/include)
target_compile_definitions(brack PRIVATE BRACK_BUILDING_DLL)
target_link_libraries(brack PRIVATE brack_core)
add_dependencies(brack brack_host)  # runs the plugins
brack_codesign(brack)
