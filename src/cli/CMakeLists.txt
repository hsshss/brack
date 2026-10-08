add_executable(brack_cli main.cpp)
set_target_properties(brack_cli PROPERTIES OUTPUT_NAME brack-cli)
target_link_libraries(brack_cli PRIVATE brack_core)
add_dependencies(brack_cli brack_host)  # runs the plugins
brack_codesign(brack_cli)
