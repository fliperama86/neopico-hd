# =============================================================================
# Main DVI application (MVS capture + HDMI output)
# =============================================================================
add_executable(neopico_hd
    main.c
)

# Generate PIO headers
pico_generate_pio_header(neopico_hd ${CMAKE_CURRENT_LIST_DIR}/mvs_capture.pio)

# Include PicoDVI headers
target_include_directories(neopico_hd PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/../lib/PicoDVI/software/include
    ${CMAKE_CURRENT_LIST_DIR}/../lib/PicoDVI/software/libdvi
)

# Link libraries
target_link_libraries(neopico_hd
    pico_stdlib
    pico_multicore
    hardware_pio
    hardware_clocks
    hardware_dma
    hardware_irq
    hardware_vreg
    libdvi
)

# DVI config: 240p output, PIO clock for odd-numbered clock pin
target_compile_definitions(neopico_hd PRIVATE
    DVI_VERTICAL_REPEAT=1
    DVI_USE_PIO_CLOCK=1
)

# Enable USB for debug output
pico_enable_stdio_usb(neopico_hd 1)
pico_enable_stdio_uart(neopico_hd 0)

# Create map/bin/hex/uf2 files
pico_add_extra_outputs(neopico_hd)

# =============================================================================
# USB streaming application (legacy - streams to PC viewer)
# =============================================================================
add_executable(neopico_usb
    main_usb.c
)

# Generate PIO headers
pico_generate_pio_header(neopico_usb ${CMAKE_CURRENT_LIST_DIR}/mvs_capture.pio)

# Link libraries
target_link_libraries(neopico_usb
    pico_stdlib
    hardware_pio
    hardware_clocks
    hardware_dma
    hardware_irq
)

# Enable SDK stdio USB
pico_enable_stdio_usb(neopico_usb 1)
pico_enable_stdio_uart(neopico_usb 0)

# Create map/bin/hex/uf2 files
pico_add_extra_outputs(neopico_usb)

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
# Audio Test (ADC capture + USB streaming)
# =============================================================================
add_executable(audio_test
    audio_test.c
)

target_link_libraries(audio_test
    pico_stdlib
    hardware_adc
    hardware_dma
    hardware_irq
)

pico_enable_stdio_usb(audio_test 1)
pico_enable_stdio_uart(audio_test 0)
pico_add_extra_outputs(audio_test)

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

# PIO clock for odd-numbered clock pin
target_compile_definitions(dvi_test PRIVATE DVI_USE_PIO_CLOCK=1)

# Enable USB for debug output
pico_enable_stdio_usb(dvi_test 1)
pico_enable_stdio_uart(dvi_test 0)

# Create UF2
pico_add_extra_outputs(dvi_test)

# =============================================================================
# HDMI Audio Test (color bars + audio tone via HDMI)
# =============================================================================
add_executable(hdmi_audio_test
    hdmi_audio_test.c
)

target_include_directories(hdmi_audio_test PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/../lib/PicoDVI/software/include
    ${CMAKE_CURRENT_LIST_DIR}/../lib/PicoDVI/software/libdvi
)

target_link_libraries(hdmi_audio_test
    pico_stdlib
    pico_multicore
    hardware_vreg
    libdvi
)

target_compile_definitions(hdmi_audio_test PRIVATE DVI_USE_PIO_CLOCK=1)

pico_enable_stdio_usb(hdmi_audio_test 1)
pico_enable_stdio_uart(hdmi_audio_test 0)
pico_add_extra_outputs(hdmi_audio_test)

# =============================================================================
# HDMI ADC Audio Test (MVS audio capture via ADC + HDMI output)
# =============================================================================
add_executable(hdmi_adc_audio_test
    hdmi_adc_audio_test.c
)

target_include_directories(hdmi_adc_audio_test PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/../lib/PicoDVI/software/include
    ${CMAKE_CURRENT_LIST_DIR}/../lib/PicoDVI/software/libdvi
)

target_link_libraries(hdmi_adc_audio_test
    pico_stdlib
    pico_multicore
    hardware_vreg
    hardware_adc
    libdvi
)

target_compile_definitions(hdmi_adc_audio_test PRIVATE DVI_USE_PIO_CLOCK=1)

pico_enable_stdio_usb(hdmi_adc_audio_test 1)
pico_enable_stdio_uart(hdmi_adc_audio_test 0)
pico_add_extra_outputs(hdmi_adc_audio_test)

# =============================================================================
# HDMI I2S Audio Test (MVS digital audio + HDMI output)
# =============================================================================
add_executable(hdmi_i2s_audio_test
    hdmi_i2s_audio_test.c
)

pico_generate_pio_header(hdmi_i2s_audio_test ${CMAKE_CURRENT_LIST_DIR}/i2s_audio.pio)

target_include_directories(hdmi_i2s_audio_test PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/../lib/PicoDVI/software/include
    ${CMAKE_CURRENT_LIST_DIR}/../lib/PicoDVI/software/libdvi
)

target_link_libraries(hdmi_i2s_audio_test
    pico_stdlib
    pico_multicore
    hardware_vreg
    hardware_pio
    hardware_dma
    libdvi
)

target_compile_definitions(hdmi_i2s_audio_test PRIVATE DVI_USE_PIO_CLOCK=1)

pico_enable_stdio_usb(hdmi_i2s_audio_test 1)
pico_enable_stdio_uart(hdmi_i2s_audio_test 0)
pico_add_extra_outputs(hdmi_i2s_audio_test)

# =============================================================================
# I2S USB Audio Test (digital audio capture + USB streaming)
# =============================================================================
add_executable(i2s_usb_test
    i2s_usb_test.c
)

pico_generate_pio_header(i2s_usb_test ${CMAKE_CURRENT_LIST_DIR}/i2s_audio.pio)

target_link_libraries(i2s_usb_test
    pico_stdlib
    hardware_pio
    hardware_dma
    hardware_irq
)

pico_enable_stdio_usb(i2s_usb_test 1)
pico_enable_stdio_uart(i2s_usb_test 0)
pico_add_extra_outputs(i2s_usb_test)

# =============================================================================
# I2S Debug (raw data capture - serial only, no USB audio)
# =============================================================================
add_executable(i2s_debug
    i2s_debug.c
)

pico_generate_pio_header(i2s_debug ${CMAKE_CURRENT_LIST_DIR}/i2s_audio.pio)

target_link_libraries(i2s_debug
    pico_stdlib
    hardware_pio
)

pico_enable_stdio_usb(i2s_debug 1)
pico_enable_stdio_uart(i2s_debug 0)
pico_add_extra_outputs(i2s_debug)

# =============================================================================
# YM2610 Digital Audio -> HDMI (proper PIO-based capture)
# =============================================================================
add_executable(ym2610_hdmi_audio
    ym2610_hdmi_audio.c
)

pico_generate_pio_header(ym2610_hdmi_audio ${CMAKE_CURRENT_LIST_DIR}/ym2610_audio.pio)

target_include_directories(ym2610_hdmi_audio PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/../lib/PicoDVI/software/include
    ${CMAKE_CURRENT_LIST_DIR}/../lib/PicoDVI/software/libdvi
)

target_link_libraries(ym2610_hdmi_audio
    pico_stdlib
    pico_multicore
    hardware_vreg
    hardware_pio
    libdvi
)

target_compile_definitions(ym2610_hdmi_audio PRIVATE DVI_USE_PIO_CLOCK=1)

pico_enable_stdio_usb(ym2610_hdmi_audio 1)
pico_enable_stdio_uart(ym2610_hdmi_audio 0)
pico_add_extra_outputs(ym2610_hdmi_audio)

# =============================================================================
# Audio Pin Frequency Measurement (PIO-based)
# =============================================================================
add_executable(freq_measure
    freq_measure.c
)

pico_generate_pio_header(freq_measure ${CMAKE_CURRENT_LIST_DIR}/freq_measure.pio)

target_link_libraries(freq_measure
    pico_stdlib
    hardware_pio
    hardware_clocks
)

pico_enable_stdio_usb(freq_measure 1)
pico_enable_stdio_uart(freq_measure 0)
pico_add_extra_outputs(freq_measure)

# =============================================================================
# Audio Capture (raw I2S to USB for WAV recording)
# =============================================================================
add_executable(audio_capture
    audio_capture.c
)

pico_generate_pio_header(audio_capture ${CMAKE_CURRENT_LIST_DIR}/audio_capture.pio)

target_link_libraries(audio_capture
    pico_stdlib
    hardware_pio
    hardware_dma
    hardware_clocks
)

pico_enable_stdio_usb(audio_capture 1)
pico_enable_stdio_uart(audio_capture 0)
pico_add_extra_outputs(audio_capture)

# =============================================================================
# Signal Capture (Logic analyzer for audio pins)
# =============================================================================
add_executable(signal_capture
    signal_capture.c
)

target_link_libraries(signal_capture
    pico_stdlib
    hardware_clocks
)

pico_enable_stdio_usb(signal_capture 1)
pico_enable_stdio_uart(signal_capture 0)
pico_add_extra_outputs(signal_capture)

# =============================================================================
# RGB Conversion Benchmark (GPIO wiring performance test)
# =============================================================================
add_executable(rgb_benchmark
    rgb_benchmark.c
)

target_link_libraries(rgb_benchmark
    pico_stdlib
    hardware_timer
    hardware_clocks
)

pico_enable_stdio_usb(rgb_benchmark 1)
pico_enable_stdio_uart(rgb_benchmark 0)
pico_add_extra_outputs(rgb_benchmark)

# =============================================================================
# ILI9341 Bouncing Box Demo (TENSTAR ROBOT GMT024-08-SPI8P display)
# Uses PIO for high-speed SPI output
# =============================================================================
add_executable(ili9341_bounce
    ili9341_bounce.c
)

pico_generate_pio_header(ili9341_bounce ${CMAKE_CURRENT_LIST_DIR}/lcd_pio.pio)

target_link_libraries(ili9341_bounce
    pico_stdlib
    hardware_pio
    hardware_gpio
    hardware_dma
    hardware_clocks
    hardware_vreg
)

pico_enable_stdio_usb(ili9341_bounce 1)
pico_enable_stdio_uart(ili9341_bounce 0)
pico_add_extra_outputs(ili9341_bounce)

# =============================================================================
# YM2610 Audio USB Capture (raw audio via PIO+DMA to USB)
# Clean capture without DVI - for debugging audio format
# =============================================================================
add_executable(audio_usb_capture
    audio_usb_capture.c
)

pico_generate_pio_header(audio_usb_capture ${CMAKE_CURRENT_LIST_DIR}/ym2610_audio.pio)

target_link_libraries(audio_usb_capture
    pico_stdlib
    hardware_pio
    hardware_dma
    hardware_irq
)

pico_enable_stdio_usb(audio_usb_capture 1)
pico_enable_stdio_uart(audio_usb_capture 0)
pico_add_extra_outputs(audio_usb_capture)