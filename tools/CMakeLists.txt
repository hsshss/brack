# Low-level probe for the Windows virtual MIDI port backend (manual use).
add_executable(vmidi_probe vmidi_probe/main.cpp)
target_link_libraries(vmidi_probe PRIVATE brack_core winmm)
