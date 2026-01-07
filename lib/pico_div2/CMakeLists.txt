add_library(pico_div2
    src/video_output.c
    src/hstx_data_island_queue.c
    src/hstx_packet.c
)

target_include_directories(pico_div2 PUBLIC 
    ${CMAKE_CURRENT_LIST_DIR}/include
)

target_link_libraries(pico_div2
    pico_stdlib
    hardware_dma
    hardware_irq
    hardware_gpio
)
