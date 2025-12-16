cmake_minimum_required(VERSION 3.13)

# Pull in SDK (must be before project)
include(pico_sdk_import.cmake)

project(audio_logic_analyzer C CXX ASM)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

pico_sdk_init()

add_executable(audio_logic_analyzer
    audio_logic_analyzer.c
)

# Use USB for stdio
pico_enable_stdio_usb(audio_logic_analyzer 1)
pico_enable_stdio_uart(audio_logic_analyzer 0)

target_link_libraries(audio_logic_analyzer
    pico_stdlib
    hardware_pio
    hardware_dma
    hardware_clocks
)

pico_add_extra_outputs(audio_logic_analyzer)

# Generate PIO header
pico_generate_pio_header(audio_logic_analyzer ${CMAKE_CURRENT_LIST_DIR}/capture.pio)
