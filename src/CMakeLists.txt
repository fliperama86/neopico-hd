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

# 240p direct mode: uncomment to enable (requires pico_hdmi 240p support)
# add_compile_definitions(VIDEO_MODE_320x240=1)

# Compile-time feature flags (cache params for fast A/B testing)
option(NEOPICO_EXP_MENU_DIAG "Enable menu diagnostics experiment module" ON)
option(NEOPICO_ENABLE_DARK_SHADOW "Enable DARK/SHADOW LUT path in capture" ON)
option(NEOPICO_EXP_BASELINE_TELEMETRY "Enable baseline input/output frame telemetry (no timing changes)" OFF)
option(NEOPICO_EXP_GENLOCK_STATIC "Enable static boot-time genlock sysclk target" OFF)
set(NEOPICO_GENLOCK_TARGET_FPS_X100 "5920" CACHE STRING "Static genlock target FPS x100 (e.g. 5920 = 59.20)")
option(NEOPICO_EXP_VTOTAL_MATCH "Enable fixed-pixel-clock output with custom V total for near-source frame rate" OFF)
set(NEOPICO_EXP_VTOTAL_LINES "532" CACHE STRING "Custom V total lines when NEOPICO_EXP_VTOTAL_MATCH is ON")

if(NEOPICO_EXP_MENU_DIAG)
    set(EXP_MENU_DIAG_VALUE 1)
else()
    set(EXP_MENU_DIAG_VALUE 0)
endif()

if(NEOPICO_ENABLE_DARK_SHADOW)
    set(ENABLE_DARK_SHADOW_VALUE 1)
else()
    set(ENABLE_DARK_SHADOW_VALUE 0)
endif()

if(NEOPICO_EXP_BASELINE_TELEMETRY)
    set(BASELINE_TELEMETRY_VALUE 1)
else()
    set(BASELINE_TELEMETRY_VALUE 0)
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

# Enable runtime video mode switching in pico_hdmi
set(PICO_HDMI_RUNTIME_MODES ON CACHE BOOL "" FORCE)

# Add pico_hdmi library (MUST be after add_compile_definitions and option overrides)
add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/../lib/pico_hdmi ${CMAKE_CURRENT_BINARY_DIR}/pico_hdmi)

pico_generate_pio_header(neopico_hd ${CMAKE_CURRENT_LIST_DIR}/video/video_capture.pio)
pico_generate_pio_header(neopico_hd ${CMAKE_CURRENT_LIST_DIR}/audio/i2s_capture.pio)

target_include_directories(neopico_hd PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}
    ${CMAKE_CURRENT_LIST_DIR}/video
    ${CMAKE_CURRENT_LIST_DIR}/audio
    ${CMAKE_CURRENT_LIST_DIR}/osd
)

target_link_libraries(neopico_hd
    pico_hdmi
    pico_stdlib
    pico_multicore
    hardware_pio
    hardware_clocks
    hardware_dma
    hardware_irq
    hardware_gpio
    hardware_interp
)

target_compile_definitions(neopico_hd PRIVATE
    HSTX_LAB_BUILD=1
    EXP_MENU_DIAG=${EXP_MENU_DIAG_VALUE}
    ENABLE_DARK_SHADOW=${ENABLE_DARK_SHADOW_VALUE}
    NEOPICO_EXP_BASELINE_TELEMETRY=${BASELINE_TELEMETRY_VALUE}
    NEOPICO_EXP_GENLOCK_STATIC=${GENLOCK_STATIC_VALUE}
    NEOPICO_GENLOCK_TARGET_FPS_X100=${NEOPICO_GENLOCK_TARGET_FPS_X100}
    NEOPICO_EXP_VTOTAL_MATCH=${VTOTAL_MATCH_VALUE}
    NEOPICO_EXP_VTOTAL_LINES=${NEOPICO_EXP_VTOTAL_LINES}
)

pico_enable_stdio_usb(neopico_hd 1)
pico_enable_stdio_uart(neopico_hd 0)
pico_add_extra_outputs(neopico_hd)
