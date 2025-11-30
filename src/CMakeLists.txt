# =============================================================================
# Main capture application
# =============================================================================
add_executable(neopico_hd
    main.c
)

# Generate PIO headers
pico_generate_pio_header(neopico_hd ${CMAKE_CURRENT_LIST_DIR}/mvs_sync.pio)

# Link libraries
target_link_libraries(neopico_hd
    pico_stdlib
    hardware_pio
    hardware_clocks
    hardware_dma
    hardware_irq
)

# Enable SDK stdio USB
pico_enable_stdio_usb(neopico_hd 1)
pico_enable_stdio_uart(neopico_hd 0)

# Create map/bin/hex/uf2 files
pico_add_extra_outputs(neopico_hd)

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

# =============================================================================
# Main DVI application (MVS capture + DVI output)
# =============================================================================
add_executable(main_dvi
    main_dvi.c
)

# Generate PIO headers
pico_generate_pio_header(main_dvi ${CMAKE_CURRENT_LIST_DIR}/mvs_sync.pio)

# Include PicoDVI headers
target_include_directories(main_dvi PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/../lib/PicoDVI/software/include
    ${CMAKE_CURRENT_LIST_DIR}/../lib/PicoDVI/software/libdvi
)

# Link libraries
target_link_libraries(main_dvi
    pico_stdlib
    pico_multicore
    hardware_pio
    hardware_clocks
    hardware_dma
    hardware_irq
    hardware_vreg
    libdvi
)

# Enable USB for debug output
pico_enable_stdio_usb(main_dvi 1)
pico_enable_stdio_uart(main_dvi 0)

# Create UF2
pico_add_extra_outputs(main_dvi)