idf_component_register(SRCS "main.cpp" "audio.cpp" "voice.cpp" "camera.cpp" "proximity.cpp" "walk_screen.cpp" "walk_upload.cpp" INCLUDE_DIRS "."
    REQUIRES arduino M5Unified TinyGPSPlus live_esp32 espressif__esp32-camera)
foreach(op Init Use_Port Deinit Probe Read Write)
    target_link_options(${COMPONENT_LIB} INTERFACE "LINKER:--wrap=SCCB_${op}")
endforeach()
