cmake_minimum_required(VERSION 3.13)

# Set PICO_SDK_PATH if not set in environment
if (NOT PICO_SDK_PATH)
    set(PICO_SDK_PATH $ENV{PICO_SDK_PATH})
endif()

# Set board type before SDK import (override with -DPICO_BOARD=pico for Pico 1)
if(NOT DEFINED PICO_BOARD)
    set(PICO_BOARD weact_studio_rp2350b_core)
endif()

# For SDK 2.x, ensure the platform is set before project() to help toolchain selection
if(PICO_BOARD MATCHES "rp2350")
    set(PICO_PLATFORM rp2350-arm-s CACHE STRING "Pico platform")
endif()

# Initialize the SDK
include(${PICO_SDK_PATH}/external/pico_sdk_import.cmake)

project(neopico_hd C CXX ASM)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

# Initialize the Raspberry Pi Pico SDK
pico_sdk_init()

# Add PicoDVI library
add_subdirectory(lib/PicoDVI/software/libdvi)

# Add the src directory
add_subdirectory(src)
