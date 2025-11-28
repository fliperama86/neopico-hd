# Add executable
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

# Enable USB output, disable UART output
pico_enable_stdio_usb(neopico_hd 1)
pico_enable_stdio_uart(neopico_hd 0)

# Create map/bin/hex/uf2 files
pico_add_extra_outputs(neopico_hd)
