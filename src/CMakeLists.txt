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
    misc/test_patterns.c
    misc/debug_render.c
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

# =============================================================================
# GPIO Frequency Analyzer (debug tool)
# =============================================================================
add_executable(gpio_freq_analyzer
    misc/gpio_freq_analyzer.c
)

target_link_libraries(gpio_freq_analyzer
    pico_stdlib
    hardware_pio
    hardware_gpio
    hardware_clocks
)

pico_enable_stdio_usb(gpio_freq_analyzer 1)
pico_enable_stdio_uart(gpio_freq_analyzer 0)
pico_add_extra_outputs(gpio_freq_analyzer)

# =============================================================================
# Simple GPIO Test (single pin, RP2350B Bank 1)
# =============================================================================
add_executable(gpio_simple_test
    misc/gpio_simple_test.c
)

target_link_libraries(gpio_simple_test
    pico_stdlib
    hardware_pio
    hardware_gpio
    hardware_clocks
)

pico_enable_stdio_usb(gpio_simple_test 1)
pico_enable_stdio_uart(gpio_simple_test 0)
pico_add_extra_outputs(gpio_simple_test)

# =============================================================================
# Bank 1 GPIO Test (GP42 with gpio_base)
# =============================================================================
add_executable(bank1_test
    misc/bank1_test.c
)

target_link_libraries(bank1_test
    pico_stdlib
    hardware_pio
    hardware_gpio
    hardware_clocks
)

pico_enable_stdio_usb(bank1_test 1)
pico_enable_stdio_uart(bank1_test 0)
pico_add_extra_outputs(bank1_test)

# =============================================================================
# GPIO Activity Map (hardware-in-loop electrical verification)
# =============================================================================
add_executable(gpio_activity_map
    misc/gpio_activity_map.c
)

target_link_libraries(gpio_activity_map
    pico_stdlib
    hardware_gpio
    hardware_clocks
)

pico_enable_stdio_usb(gpio_activity_map 1)
pico_enable_stdio_uart(gpio_activity_map 0)
pico_add_extra_outputs(gpio_activity_map)

# =============================================================================
# PIO PC Tracker (PIO state machine execution monitor)
# =============================================================================
add_executable(pio_pc_tracker
    misc/pio_pc_tracker.c
)

target_link_libraries(pio_pc_tracker
    pico_stdlib
    hardware_pio
    hardware_gpio
    hardware_clocks
)

pico_enable_stdio_usb(pio_pc_tracker 1)
pico_enable_stdio_uart(pio_pc_tracker 0)
pico_add_extra_outputs(pio_pc_tracker)

# =============================================================================
# Bank 1 GPIO Speed Test (gpio_get() performance comparison)
# =============================================================================
add_executable(bank1_gpio_speed_test
    misc/bank1_gpio_speed_test.c
)

target_link_libraries(bank1_gpio_speed_test
    pico_stdlib
    hardware_gpio
    hardware_clocks
)

pico_enable_stdio_usb(bank1_gpio_speed_test 1)
pico_enable_stdio_uart(bank1_gpio_speed_test 0)
pico_add_extra_outputs(bank1_gpio_speed_test)

# =============================================================================
# DVI Audio Test (HDMI audio output with test tone)
# =============================================================================
add_executable(dvi_audio_test
    misc/dvi_audio_test.c
)

target_include_directories(dvi_audio_test PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}
    ${CMAKE_CURRENT_LIST_DIR}/../lib/PicoDVI/software/include
    ${CMAKE_CURRENT_LIST_DIR}/../lib/PicoDVI/software/libdvi
)

target_link_libraries(dvi_audio_test
    pico_stdlib
    pico_multicore
    hardware_pio
    hardware_gpio
    hardware_clocks
    hardware_dma
    hardware_irq
    hardware_vreg
    libdvi
)

target_compile_definitions(dvi_audio_test PRIVATE
    DVI_VERTICAL_REPEAT=1
    DVI_USE_PIO_CLOCK=1
)

pico_enable_stdio_usb(dvi_audio_test 1)
pico_enable_stdio_uart(dvi_audio_test 0)
pico_add_extra_outputs(dvi_audio_test)

# =============================================================================
# HSTX Lab - HSTX-based DVI with HDMI audio
# =============================================================================
add_executable(hstx_lab
    misc/hstx_lab/src/main.c
    misc/hstx_lab/src/data_packet.c
)

target_include_directories(hstx_lab PRIVATE
    misc/hstx_lab/include
)

target_link_libraries(hstx_lab
    pico_stdlib
    hardware_dma
    hardware_gpio
    hardware_irq
    hardware_clocks
)

pico_enable_stdio_usb(hstx_lab 1)
pico_enable_stdio_uart(hstx_lab 0)
pico_add_extra_outputs(hstx_lab)
