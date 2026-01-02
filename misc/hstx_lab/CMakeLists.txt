cmake_minimum_required(VERSION 3.13)

# Initialize the Pico SDK
set(PICO_PLATFORM rp2350)
set(PICO_BOARD pico2)

# Pull in SDK (must be before project)
include(pico_sdk_import.cmake)

project(hstx_lab C CXX ASM)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

# Initialize the SDK
pico_sdk_init()

# Add executable
add_executable(${PROJECT_NAME}
    src/main.c
    src/data_packet.c
)

# Include directories
target_include_directories(${PROJECT_NAME} PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/include
)

# Link libraries
target_link_libraries(${PROJECT_NAME}
    pico_stdlib
    hardware_dma
    hardware_gpio
    hardware_irq
    hardware_clocks
)

# Enable USB stdio for debugging and picotool reboot
pico_enable_stdio_usb(${PROJECT_NAME} 1)
pico_enable_stdio_uart(${PROJECT_NAME} 0)

# Create map/bin/hex/uf2 files
pico_add_extra_outputs(${PROJECT_NAME})

# ============================================
# Debug Firmware for Logic Analyzer capture
# Runs at HALF SPEED (126Mbps TMDS) with trigger on GP20
# ============================================
add_executable(hstx_debug
    src/main_debug.c
    src/data_packet.c
)

target_include_directories(hstx_debug PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/include
)

target_link_libraries(hstx_debug
    pico_stdlib
    hardware_dma
    hardware_gpio
    hardware_irq
    hardware_clocks
)

pico_enable_stdio_usb(hstx_debug 1)
pico_enable_stdio_uart(hstx_debug 1)
pico_add_extra_outputs(hstx_debug)

# ============================================
# Analyzer Firmware (400MHz overclocked blast capture)
# ============================================
add_executable(analyzer
    src/analyzer.c
)

pico_generate_pio_header(analyzer ${CMAKE_CURRENT_LIST_DIR}/pio/blast_capture.pio)

target_include_directories(analyzer PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/include
)

target_link_libraries(analyzer
    pico_stdlib
    hardware_pio
    hardware_dma
    hardware_clocks
    hardware_vreg
)

pico_enable_stdio_usb(analyzer 1)
pico_enable_stdio_uart(analyzer 0)

pico_add_extra_outputs(analyzer)

# ============================================
# PicoDVI Audio Test (uses PIO instead of HSTX)
# ============================================
add_subdirectory(lib/PicoDVI/software/libdvi)

add_executable(picodvi_audio_test
    src/picodvi_audio_test.c
)

target_include_directories(picodvi_audio_test PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/lib/PicoDVI/software/include
    ${CMAKE_CURRENT_LIST_DIR}/lib/PicoDVI/software/libdvi
)

target_link_libraries(picodvi_audio_test
    pico_stdlib
    pico_multicore
    hardware_vreg
    libdvi
)

target_compile_definitions(picodvi_audio_test PRIVATE DVI_USE_PIO_CLOCK=1)

pico_enable_stdio_usb(picodvi_audio_test 1)
pico_enable_stdio_uart(picodvi_audio_test 0)
pico_add_extra_outputs(picodvi_audio_test)

# ============================================
# LA Test - Simple GPIO patterns for verification
# ============================================
add_executable(la_test
    src/la_test.c
)

target_link_libraries(la_test
    pico_stdlib
    hardware_gpio
)

pico_enable_stdio_usb(la_test 1)
pico_enable_stdio_uart(la_test 0)
pico_add_extra_outputs(la_test)
