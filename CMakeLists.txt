cmake_minimum_required(VERSION 3.13)

# Set board type before SDK import (override with -DPICO_BOARD=pico for Pico 1)
if(NOT DEFINED PICO_BOARD)
    set(PICO_BOARD pico2)
endif()

# Initialize the SDK
include($ENV{PICO_SDK_PATH}/external/pico_sdk_import.cmake)

project(neopico_hd C CXX ASM)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

# Initialize the Raspberry Pi Pico SDK
pico_sdk_init()

# Add PicoDVI library
add_subdirectory(lib/PicoDVI/software/libdvi)

# Add the src directory
add_subdirectory(src)
