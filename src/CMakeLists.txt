# =============================================================================
# NeoPico-HD Main Firmware (MVS capture + HSTX output)
# =============================================================================
add_executable(neopico_hd
    main.c
    video/video_capture.c
    video/video_pipeline.c
    osd/fast_osd.c
    osd/selftest_layout.c
    audio/i2s_capture.c
    audio/audio_pipeline.c
    audio/audio_subsystem.c
    audio/audio_buffer.c
    audio/dc_filter.c
    audio/lowpass.c
    audio/src.c
)

# 240p direct mode: uncomment to enable (requires pico_hdmi 240p support)
# add_compile_definitions(VIDEO_MODE_320x240=1)

# Enable runtime video mode switching in pico_hdmi
set(PICO_HDMI_RUNTIME_MODES ON CACHE BOOL "" FORCE)

# Add pico_hdmi library (MUST be after add_compile_definitions and option overrides)
add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/../lib/pico_hdmi ${CMAKE_CURRENT_BINARY_DIR}/pico_hdmi)

pico_generate_pio_header(neopico_hd ${CMAKE_CURRENT_LIST_DIR}/video/video_capture.pio)
pico_generate_pio_header(neopico_hd ${CMAKE_CURRENT_LIST_DIR}/audio/i2s_capture.pio)

target_include_directories(neopico_hd PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}
    ${CMAKE_CURRENT_LIST_DIR}/video
    ${CMAKE_CURRENT_LIST_DIR}/audio
    ${CMAKE_CURRENT_LIST_DIR}/osd
)

target_link_libraries(neopico_hd
    pico_hdmi
    pico_stdlib
    pico_multicore
    hardware_pio
    hardware_clocks
    hardware_dma
    hardware_irq
    hardware_gpio
    hardware_interp
)

target_compile_definitions(neopico_hd PRIVATE
    HSTX_LAB_BUILD=1
)

pico_enable_stdio_usb(neopico_hd 1)
pico_enable_stdio_uart(neopico_hd 0)
pico_add_extra_outputs(neopico_hd)
