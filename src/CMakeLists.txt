# Add executable
add_executable(freq_counter
    main_dma.c
)

# Generate PIO headers
pico_generate_pio_header(freq_counter ${CMAKE_CURRENT_LIST_DIR}/freq_counter.pio)
pico_generate_pio_header(freq_counter ${CMAKE_CURRENT_LIST_DIR}/mvs_sync.pio)

# Link libraries
target_link_libraries(freq_counter
    pico_stdlib
    hardware_pio
    hardware_clocks
    hardware_dma
    hardware_irq
)

# Enable USB output, disable UART output
pico_enable_stdio_usb(freq_counter 1)
pico_enable_stdio_uart(freq_counter 0)

# Create map/bin/hex/uf2 files
pico_add_extra_outputs(freq_counter)
