cmake_minimum_required(VERSION 3.13)

# Initialize the SDK
include($ENV{PICO_SDK_PATH}/external/pico_sdk_import.cmake)

project(freq_counter C CXX ASM)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

# Initialize the Raspberry Pi Pico SDK
pico_sdk_init()

# Add the src directory
add_subdirectory(src)
