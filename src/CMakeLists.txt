# =============================================================================
# NeoPico-HD Main Firmware (MVS capture + HSTX output)
# =============================================================================
add_executable(neopico_hd
    main.c
    video/video_capture.c
    osd/osd.c
    audio/i2s_capture.c
    audio/audio_pipeline.c
    audio/audio_subsystem.c
    audio/audio_buffer.c
    audio/dc_filter.c
    audio/lowpass.c
    audio/src.c
)

# Add pico_div2 library
add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/../lib/pico_div2 ${CMAKE_CURRENT_BINARY_DIR}/pico_div2)

pico_generate_pio_header(neopico_hd ${CMAKE_CURRENT_LIST_DIR}/video/video_capture.pio)
pico_generate_pio_header(neopico_hd ${CMAKE_CURRENT_LIST_DIR}/audio/i2s_capture.pio)

target_include_directories(neopico_hd PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}
    ${CMAKE_CURRENT_LIST_DIR}/video
    ${CMAKE_CURRENT_LIST_DIR}/audio
    ${CMAKE_CURRENT_LIST_DIR}/osd
)

target_link_libraries(neopico_hd
    pico_div2
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

