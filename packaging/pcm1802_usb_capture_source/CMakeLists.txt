cmake_minimum_required(VERSION 3.13)

if(NOT PICO_SDK_PATH)
    set(PICO_SDK_PATH $ENV{PICO_SDK_PATH})
endif()

if(NOT PICO_SDK_PATH)
    message(FATAL_ERROR "Set PICO_SDK_PATH to a Raspberry Pi Pico SDK checkout")
endif()

if(NOT DEFINED PICO_BOARD)
    set(PICO_BOARD weact_studio_rp2350b_core)
endif()

if(PICO_BOARD MATCHES "rp2350")
    set(PICO_PLATFORM rp2350 CACHE STRING "Pico platform" FORCE)
endif()

include(${PICO_SDK_PATH}/external/pico_sdk_import.cmake)

project(neopico_pcm1802_usb_capture VERSION 1.0.0 LANGUAGES C CXX ASM)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

pico_sdk_init()

if(NOT PICO_PLATFORM MATCHES "^rp2350")
    message(FATAL_ERROR "This diagnostic requires an RP2350 board with PIO2")
endif()

set(NEOPICO_PCM1802_USB_BITS_PER_SAMPLE "24" CACHE STRING
    "USB payload width: 24 (full samples) or 16 (reduced-bandwidth diagnostic)")
set_property(CACHE NEOPICO_PCM1802_USB_BITS_PER_SAMPLE PROPERTY STRINGS 24 16)
if(NOT NEOPICO_PCM1802_USB_BITS_PER_SAMPLE STREQUAL "24" AND
   NOT NEOPICO_PCM1802_USB_BITS_PER_SAMPLE STREQUAL "16")
    message(FATAL_ERROR "NEOPICO_PCM1802_USB_BITS_PER_SAMPLE must be 24 or 16")
endif()

add_executable(neopico_pcm1802_usb_capture
    src/main.c
    src/usb_descriptors.c
)

target_include_directories(neopico_pcm1802_usb_capture PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/src
)

target_compile_options(neopico_pcm1802_usb_capture PRIVATE
    -Wall
    -Wextra
    -Werror
)

target_compile_definitions(neopico_pcm1802_usb_capture PRIVATE
    NEOPICO_PCM1802_USB_BITS_PER_SAMPLE=${NEOPICO_PCM1802_USB_BITS_PER_SAMPLE}
)

pico_generate_pio_header(neopico_pcm1802_usb_capture
    ${CMAKE_CURRENT_LIST_DIR}/src/i2s_capture.pio
)

target_link_libraries(neopico_pcm1802_usb_capture
    pico_stdlib
    pico_unique_id
    tinyusb_device
    tinyusb_board
    hardware_pio
    hardware_dma
    hardware_irq
    hardware_gpio
)

pico_set_program_name(neopico_pcm1802_usb_capture "neopico_pcm1802_usb_capture")
pico_set_program_version(neopico_pcm1802_usb_capture "1.0.0")
pico_enable_stdio_usb(neopico_pcm1802_usb_capture 0)
pico_enable_stdio_uart(neopico_pcm1802_usb_capture 0)
pico_add_extra_outputs(neopico_pcm1802_usb_capture)
