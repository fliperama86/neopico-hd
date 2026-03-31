# =============================================================================
# NeoPico-HD Main Firmware (MVS capture + HSTX output)
# =============================================================================
add_executable(neopico_hd
    main.c
    experiments/menu_diag_experiment.c
    video/video_capture.c
    video/video_pipeline.c
    osd/fast_osd.c
    osd/selftest_layout.c
    audio/i2s_capture.c
    audio/audio_pipeline.c
    audio/audio_subsystem.c
    audio/audio_buffer.c
    audio/dc_filter.c
    audio/lowpass.c
    audio/src.c
)

# =============================================================================
# NeoPico-HD Test Firmware (Telemetry OSD HUD)
# =============================================================================
option(NEOPICO_BUILD_TESTFW "Build standalone telemetry test firmware target" OFF)

if(NEOPICO_BUILD_TESTFW)
add_executable(neopico_hd_testfw
    testfw_main.c
    experiments/lock_telemetry_experiment.c
    testfw/sync_probe.c
    osd/fast_osd.c
)
endif()

# Compile-time feature flags (cache params for fast A/B testing)
option(NEOPICO_VIDEO_240P "Start HDMI output in 240p mode via runtime mode API" OFF)
option(NEOPICO_ENABLE_OSD "Enable OSD overlay and button handling" OFF)
option(NEOPICO_ENABLE_SELFTEST "Enable self-test diagnostic layout (requires OSD)" OFF)
option(NEOPICO_ENABLE_DARK_SHADOW "Enable DARK/SHADOW LUT path in capture" OFF)
option(NEOPICO_EXP_GENLOCK_STATIC "Enable static boot-time genlock sysclk target" OFF)
set(NEOPICO_GENLOCK_TARGET_FPS_X100 "5920" CACHE STRING "Static genlock target FPS x100 (e.g. 5920 = 59.20)")
option(NEOPICO_EXP_VTOTAL_MATCH "Enable fixed-pixel-clock output with custom V total for near-source frame rate" OFF)
set(NEOPICO_EXP_VTOTAL_LINES "532" CACHE STRING "Custom V total lines when NEOPICO_EXP_VTOTAL_MATCH is ON")
option(NEOPICO_EXP_GENLOCK_DYNAMIC "Enable dynamic VTOTAL genlock (runtime phase tracking)" OFF)
option(NEOPICO_AUDIO_PCM1802 "Use PCM1802 ADC for analog audio capture (AES/non-MV1C boards)" OFF)

if(NEOPICO_ENABLE_OSD)
    set(ENABLE_OSD_VALUE 1)
else()
    set(ENABLE_OSD_VALUE 0)
endif()

if(NEOPICO_ENABLE_SELFTEST)
    set(ENABLE_SELFTEST_VALUE 1)
else()
    set(ENABLE_SELFTEST_VALUE 0)
endif()

if(NEOPICO_ENABLE_DARK_SHADOW)
    set(ENABLE_DARK_SHADOW_VALUE 1)
else()
    set(ENABLE_DARK_SHADOW_VALUE 0)
endif()

if(NEOPICO_EXP_GENLOCK_STATIC)
    set(GENLOCK_STATIC_VALUE 1)
else()
    set(GENLOCK_STATIC_VALUE 0)
endif()

if(NEOPICO_EXP_VTOTAL_MATCH)
    set(VTOTAL_MATCH_VALUE 1)
else()
    set(VTOTAL_MATCH_VALUE 0)
endif()

if(NEOPICO_EXP_GENLOCK_DYNAMIC)
    set(GENLOCK_DYNAMIC_VALUE 1)
else()
    set(GENLOCK_DYNAMIC_VALUE 0)
endif()

if(NEOPICO_AUDIO_PCM1802)
    set(AUDIO_PCM1802_VALUE 1)
else()
    set(AUDIO_PCM1802_VALUE 0)
endif()

if(NEOPICO_VIDEO_240P)
    set(VIDEO_240P_VALUE 1)
else()
    set(VIDEO_240P_VALUE 0)
endif()

# Enable runtime video mode switching in pico_hdmi
set(PICO_HDMI_RUNTIME_MODES ON CACHE BOOL "" FORCE)

# Add pico_hdmi library (MUST be after add_compile_definitions and option overrides)
add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/../lib/pico_hdmi ${CMAKE_CURRENT_BINARY_DIR}/pico_hdmi)

pico_generate_pio_header(neopico_hd ${CMAKE_CURRENT_LIST_DIR}/video/video_capture.pio)
pico_generate_pio_header(neopico_hd ${CMAKE_CURRENT_LIST_DIR}/audio/i2s_capture.pio)
if(NEOPICO_BUILD_TESTFW)
pico_generate_pio_header(neopico_hd_testfw ${CMAKE_CURRENT_LIST_DIR}/testfw/sync_probe.pio)
endif()

target_include_directories(neopico_hd PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}
    ${CMAKE_CURRENT_LIST_DIR}/video
    ${CMAKE_CURRENT_LIST_DIR}/audio
    ${CMAKE_CURRENT_LIST_DIR}/osd
)
if(NEOPICO_BUILD_TESTFW)
target_include_directories(neopico_hd_testfw PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}
    ${CMAKE_CURRENT_LIST_DIR}/testfw
    ${CMAKE_CURRENT_LIST_DIR}/osd
)
endif()

target_link_libraries(neopico_hd
    pico_hdmi
    pico_stdlib
    pico_multicore
    hardware_pio
    hardware_clocks
    hardware_dma
    hardware_irq
    hardware_gpio
)
if(NEOPICO_BUILD_TESTFW)
target_link_libraries(neopico_hd_testfw
    pico_hdmi
    pico_stdlib
    pico_multicore
    hardware_pio
    hardware_clocks
    hardware_dma
    hardware_irq
    hardware_gpio
)
endif()

target_compile_definitions(neopico_hd PRIVATE
    NEOPICO_VIDEO_240P=${VIDEO_240P_VALUE}
    NEOPICO_ENABLE_OSD=${ENABLE_OSD_VALUE}
    NEOPICO_ENABLE_SELFTEST=${ENABLE_SELFTEST_VALUE}
    ENABLE_DARK_SHADOW=${ENABLE_DARK_SHADOW_VALUE}
    NEOPICO_EXP_GENLOCK_STATIC=${GENLOCK_STATIC_VALUE}
    NEOPICO_GENLOCK_TARGET_FPS_X100=${NEOPICO_GENLOCK_TARGET_FPS_X100}
    NEOPICO_EXP_VTOTAL_MATCH=${VTOTAL_MATCH_VALUE}
    NEOPICO_EXP_VTOTAL_LINES=${NEOPICO_EXP_VTOTAL_LINES}
    NEOPICO_EXP_GENLOCK_DYNAMIC=${GENLOCK_DYNAMIC_VALUE}
    NEOPICO_AUDIO_PCM1802=${AUDIO_PCM1802_VALUE}
)
if(NEOPICO_BUILD_TESTFW)
target_compile_definitions(neopico_hd_testfw PRIVATE
    NEOPICO_VIDEO_240P=${VIDEO_240P_VALUE}
    NEOPICO_ENABLE_OSD=1
    NEOPICO_ENABLE_SELFTEST=0
    PICO_CORE1_STACK_SIZE=1536
)
endif()

pico_enable_stdio_usb(neopico_hd 1)
pico_enable_stdio_uart(neopico_hd 0)
pico_add_extra_outputs(neopico_hd)
if(NEOPICO_BUILD_TESTFW)
pico_enable_stdio_usb(neopico_hd_testfw 1)
pico_enable_stdio_uart(neopico_hd_testfw 0)
pico_add_extra_outputs(neopico_hd_testfw)
endif()
