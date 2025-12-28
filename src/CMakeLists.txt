# =============================================================================
# NeoPico-HD Main Firmware (MVS capture + HDMI output)
# =============================================================================
add_executable(neopico_hd
    main.c
    video/video_capture.c
    video/video_output.c
    audio/audio_pipeline.c
    audio/audio_output.c
    audio/audio_buffer.c
    audio/i2s_capture.c
    audio/src.c
    audio/dc_filter.c
    audio/lowpass.c
)

pico_generate_pio_header(neopico_hd ${CMAKE_CURRENT_LIST_DIR}/video/video_capture.pio)
pico_generate_pio_header(neopico_hd ${CMAKE_CURRENT_LIST_DIR}/audio/i2s_capture.pio)

target_include_directories(neopico_hd PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}
    ${CMAKE_CURRENT_LIST_DIR}/../lib/PicoDVI/software/include
    ${CMAKE_CURRENT_LIST_DIR}/../lib/PicoDVI/software/libdvi
)

target_link_libraries(neopico_hd
    pico_stdlib
    pico_multicore
    hardware_pio
    hardware_clocks
    hardware_dma
    hardware_irq
    hardware_vreg
    libdvi
)

target_compile_definitions(neopico_hd PRIVATE
    DVI_VERTICAL_REPEAT=1
    DVI_USE_PIO_CLOCK=1
)

pico_enable_stdio_usb(neopico_hd 1)
pico_enable_stdio_uart(neopico_hd 0)
pico_add_extra_outputs(neopico_hd)
