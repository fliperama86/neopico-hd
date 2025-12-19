# =============================================================================
# NeoPico-HD Main Firmware (MVS capture + HDMI output)
# =============================================================================
add_executable(neopico_hd
    main.c
    audio/audio_buffer.c
    audio/i2s_capture.c
    audio/src.c
    audio/dc_filter.c
    audio/lowpass.c
)

pico_generate_pio_header(neopico_hd ${CMAKE_CURRENT_LIST_DIR}/mvs_capture.pio)
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

# =============================================================================
# Audio Pipeline Test (audio capture + color bars display)
# =============================================================================
add_executable(audio_pipeline_test
    audio_pipeline_test.c
    audio/audio_buffer.c
    audio/dc_filter.c
    audio/lowpass.c
    audio/src.c
    audio/i2s_capture.c
    audio/audio_pipeline.c
)

pico_generate_pio_header(audio_pipeline_test ${CMAKE_CURRENT_LIST_DIR}/audio/i2s_capture.pio)

target_include_directories(audio_pipeline_test PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}
    ${CMAKE_CURRENT_LIST_DIR}/../lib/PicoDVI/software/include
    ${CMAKE_CURRENT_LIST_DIR}/../lib/PicoDVI/software/libdvi
)

target_link_libraries(audio_pipeline_test
    pico_stdlib
    pico_multicore
    hardware_vreg
    hardware_pio
    hardware_dma
    hardware_irq
    libdvi
)

target_compile_definitions(audio_pipeline_test PRIVATE DVI_USE_PIO_CLOCK=1)

pico_enable_stdio_usb(audio_pipeline_test 1)
pico_enable_stdio_uart(audio_pipeline_test 0)
pico_add_extra_outputs(audio_pipeline_test)

# =============================================================================
# DVI Test (color bars - no capture, HDMI output only)
# =============================================================================
add_executable(dvi_test
    dvi_test.c
)

target_include_directories(dvi_test PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/../lib/PicoDVI/software/include
    ${CMAKE_CURRENT_LIST_DIR}/../lib/PicoDVI/software/libdvi
)

target_link_libraries(dvi_test
    pico_stdlib
    pico_multicore
    hardware_vreg
    libdvi
)

target_compile_definitions(dvi_test PRIVATE DVI_USE_PIO_CLOCK=1)

pico_enable_stdio_usb(dvi_test 1)
pico_enable_stdio_uart(dvi_test 0)
pico_add_extra_outputs(dvi_test)

# =============================================================================
# DVI Audio Test (240p with HDMI audio - test tone)
# =============================================================================
add_executable(dvi_audio_test
    dvi_audio_test.c
)

target_include_directories(dvi_audio_test PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/../lib/PicoDVI/software/include
    ${CMAKE_CURRENT_LIST_DIR}/../lib/PicoDVI/software/libdvi
)

target_link_libraries(dvi_audio_test
    pico_stdlib
    pico_multicore
    hardware_vreg
    libdvi
)

target_compile_definitions(dvi_audio_test PRIVATE DVI_USE_PIO_CLOCK=1)

pico_enable_stdio_usb(dvi_audio_test 1)
pico_enable_stdio_uart(dvi_audio_test 0)
pico_add_extra_outputs(dvi_audio_test)

# =============================================================================
# GPIO Frequency Analyzer (debug wiring and clock signals)
# =============================================================================
add_executable(gpio_freq_analyzer
    gpio_freq_analyzer.c
)

target_link_libraries(gpio_freq_analyzer
    pico_stdlib
    hardware_timer
    hardware_pio
    hardware_clocks
)

pico_enable_stdio_usb(gpio_freq_analyzer 1)
pico_enable_stdio_uart(gpio_freq_analyzer 0)
pico_add_extra_outputs(gpio_freq_analyzer)
