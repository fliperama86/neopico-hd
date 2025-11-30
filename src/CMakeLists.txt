# =============================================================================
# Main DVI application (MVS capture + HDMI output)
# =============================================================================
add_executable(neopico_hd
    main.c
)

# Generate PIO headers
pico_generate_pio_header(neopico_hd ${CMAKE_CURRENT_LIST_DIR}/mvs_capture.pio)

# Include PicoDVI headers
target_include_directories(neopico_hd PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/../lib/PicoDVI/software/include
    ${CMAKE_CURRENT_LIST_DIR}/../lib/PicoDVI/software/libdvi
)

# Link libraries
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

# Disable line doubling for 240p output
target_compile_definitions(neopico_hd PRIVATE
    DVI_VERTICAL_REPEAT=1
)

# Enable USB for debug output
pico_enable_stdio_usb(neopico_hd 1)
pico_enable_stdio_uart(neopico_hd 0)

# Create map/bin/hex/uf2 files
pico_add_extra_outputs(neopico_hd)

# =============================================================================
# USB streaming application (legacy - streams to PC viewer)
# =============================================================================
add_executable(neopico_usb
    main_usb.c
)

# Generate PIO headers
pico_generate_pio_header(neopico_usb ${CMAKE_CURRENT_LIST_DIR}/mvs_capture.pio)

# Link libraries
target_link_libraries(neopico_usb
    pico_stdlib
    hardware_pio
    hardware_clocks
    hardware_dma
    hardware_irq
)

# Enable SDK stdio USB
pico_enable_stdio_usb(neopico_usb 1)
pico_enable_stdio_uart(neopico_usb 0)

# Create map/bin/hex/uf2 files
pico_add_extra_outputs(neopico_usb)

# =============================================================================
# GPIO Test (for debugging wiring)
# =============================================================================
add_executable(gpio_test
    gpio_test.c
)

target_link_libraries(gpio_test
    pico_stdlib
)

pico_enable_stdio_usb(gpio_test 1)
pico_enable_stdio_uart(gpio_test 0)
pico_add_extra_outputs(gpio_test)

# =============================================================================
# DVI Test application (color bars)
# =============================================================================
add_executable(dvi_test
    dvi_test.c
)

# Include PicoDVI headers
target_include_directories(dvi_test PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/../lib/PicoDVI/software/include
    ${CMAKE_CURRENT_LIST_DIR}/../lib/PicoDVI/software/libdvi
)

# Link libraries
target_link_libraries(dvi_test
    pico_stdlib
    pico_multicore
    hardware_vreg
    libdvi
)

# Enable USB for debug output
pico_enable_stdio_usb(dvi_test 1)
pico_enable_stdio_uart(dvi_test 0)

# Create UF2
pico_add_extra_outputs(dvi_test)