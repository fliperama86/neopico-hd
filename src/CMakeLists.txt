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

# Compile-time feature flags (cache params for fast A/B testing)
option(NEOPICO_VIDEO_240P "Start HDMI output in 240p mode via runtime mode API" OFF)
option(NEOPICO_VIDEO_720P "Build HDMI output in 1280x720 @ 60Hz mode (requires sys_clk 372 MHz / 1.30V)" OFF)
# When ON, link the compile-time pico_hdmi path (video_output.c). Loses the
# rt-only API surface (set_mode, update_acr, dynamic vtotal genlock, etc.) —
# code that uses those is #ifdef'd out. Use as an A/B against the rt path
# when chasing 720p stability.
option(NEOPICO_USE_NONRT_HDMI "Use compile-time pico_hdmi path instead of runtime-mode" OFF)
option(NEOPICO_VIDEO_DVI_ONLY "Disable HDMI Data Islands/audio output for video isolation tests" OFF)
option(NEOPICO_VIDEO_TEST_PATTERN "Render a static Core 1 video test pattern instead of captured video" OFF)
option(NEOPICO_ENABLE_OSD "Enable OSD overlay and button handling" OFF)
option(NEOPICO_ENABLE_SELFTEST "Enable self-test diagnostic layout (requires OSD)" OFF)
option(NEOPICO_ENABLE_DARK_SHADOW "Enable DARK/SHADOW LUT path in capture" OFF)
option(NEOPICO_EXP_GENLOCK_STATIC "Enable static boot-time genlock sysclk target" OFF)
set(NEOPICO_GENLOCK_TARGET_FPS_X100 "5920" CACHE STRING "Static genlock target FPS x100 (e.g. 5920 = 59.20)")
option(NEOPICO_EXP_VTOTAL_MATCH "Enable fixed-pixel-clock output with custom V total for near-source frame rate" OFF)
set(NEOPICO_EXP_VTOTAL_LINES "532" CACHE STRING "Custom V total lines when NEOPICO_EXP_VTOTAL_MATCH is ON")
option(NEOPICO_EXP_GENLOCK_DYNAMIC "Enable dynamic VTOTAL genlock (runtime phase tracking)" OFF)
option(NEOPICO_EXP_REBOOT_MODE_SWITCH "Enable experimental reboot-based 480p/240p HDMI mode switching via OSD Back button" OFF)
option(NEOPICO_EXP_RUNTIME_MODE_SWITCH "Deprecated alias for NEOPICO_EXP_REBOOT_MODE_SWITCH" OFF)
option(NEOPICO_AUDIO_PCM1802 "Use PCM1802 ADC for analog audio capture (AES/non-MV1C boards)" OFF)
option(NEOPICO_CAPTURE_FREEZE_AFTER_FRAME "Capture one MVS frame then stop capture writes for video isolation tests" OFF)
set(NEOPICO_LINE_RING_SIZE "256" CACHE STRING "Number of 320px lines in the capture/display ring")

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

if(NEOPICO_EXP_RUNTIME_MODE_SWITCH)
    message(WARNING "NEOPICO_EXP_RUNTIME_MODE_SWITCH is deprecated; enabling NEOPICO_EXP_REBOOT_MODE_SWITCH instead")
    set(NEOPICO_EXP_REBOOT_MODE_SWITCH ON CACHE BOOL "Enable experimental reboot-based 480p/240p HDMI mode switching via OSD Back button" FORCE)
endif()

if(NEOPICO_EXP_REBOOT_MODE_SWITCH)
    set(REBOOT_MODE_SWITCH_VALUE 1)
else()
    set(REBOOT_MODE_SWITCH_VALUE 0)
endif()

if(NEOPICO_AUDIO_PCM1802)
    set(AUDIO_PCM1802_VALUE 1)
else()
    set(AUDIO_PCM1802_VALUE 0)
endif()

if(NEOPICO_CAPTURE_FREEZE_AFTER_FRAME)
    set(CAPTURE_FREEZE_AFTER_FRAME_VALUE 1)
else()
    set(CAPTURE_FREEZE_AFTER_FRAME_VALUE 0)
endif()

if(NEOPICO_VIDEO_240P)
    set(VIDEO_240P_VALUE 1)
else()
    set(VIDEO_240P_VALUE 0)
endif()

if(NEOPICO_VIDEO_720P)
    set(VIDEO_720P_VALUE 1)
    if(NEOPICO_VIDEO_240P)
        message(FATAL_ERROR "NEOPICO_VIDEO_720P and NEOPICO_VIDEO_240P are mutually exclusive")
    endif()
else()
    set(VIDEO_720P_VALUE 0)
endif()

if(NEOPICO_USE_NONRT_HDMI)
    set(USE_NONRT_HDMI_VALUE 1)
    if(NEOPICO_VIDEO_240P AND NOT NEOPICO_VIDEO_720P)
        # 240p on non-rt would also need VIDEO_MODE_320x240 propagation; keep
        # the surface narrow for now — only 720p (or default 480p) is wired.
        message(WARNING "NEOPICO_USE_NONRT_HDMI=ON with NEOPICO_VIDEO_240P=ON is untested; 240p compile-time mode requires VIDEO_MODE_320x240 also be set on the library and exe.")
    endif()
else()
    set(USE_NONRT_HDMI_VALUE 0)
endif()

if(NEOPICO_EXP_REBOOT_MODE_SWITCH)
    if(NEOPICO_USE_NONRT_HDMI)
        message(FATAL_ERROR "NEOPICO_EXP_REBOOT_MODE_SWITCH requires the runtime pico_hdmi path")
    endif()
    if(NEOPICO_VIDEO_720P)
        message(FATAL_ERROR "NEOPICO_EXP_REBOOT_MODE_SWITCH is currently limited to 480p/240p; do not combine with NEOPICO_VIDEO_720P")
    endif()
    if(NOT NEOPICO_ENABLE_OSD)
        message(FATAL_ERROR "NEOPICO_EXP_REBOOT_MODE_SWITCH currently uses the OSD Back button, so NEOPICO_ENABLE_OSD=ON is required")
    endif()
endif()

if(NEOPICO_VIDEO_DVI_ONLY)
    set(VIDEO_DVI_ONLY_VALUE 1)
else()
    set(VIDEO_DVI_ONLY_VALUE 0)
endif()

if(NEOPICO_VIDEO_TEST_PATTERN)
    set(VIDEO_TEST_PATTERN_VALUE 1)
else()
    set(VIDEO_TEST_PATTERN_VALUE 0)
endif()

# Pick rt vs compile-time pico_hdmi path
if(NEOPICO_USE_NONRT_HDMI)
    set(PICO_HDMI_RUNTIME_MODES OFF CACHE BOOL "" FORCE)
else()
    set(PICO_HDMI_RUNTIME_MODES ON CACHE BOOL "" FORCE)
endif()

# Add pico_hdmi library (MUST be after add_compile_definitions and option overrides)
add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/../lib/pico_hdmi ${CMAKE_CURRENT_BINARY_DIR}/pico_hdmi)

# 720p requires compile-time polarity / DI placement in pico_hdmi (VIDEO_MODE_1280x720)
if(NEOPICO_VIDEO_720P)
    target_compile_definitions(pico_hdmi PRIVATE VIDEO_MODE_1280x720)
    # When using the non-rt path, code calling hstx_encode_data_island directly
    # also resolves DI_HSYNC_ACTIVE at compile time, so the exe target needs
    # the same VIDEO_MODE_* the library was built with — otherwise the embedded
    # HSYNC bit in audio Data Island packets disagrees with the library's sync
    # generation and the display fails to lock.
    if(NEOPICO_USE_NONRT_HDMI)
        target_compile_definitions(neopico_hd PRIVATE VIDEO_MODE_1280x720)
    endif()
endif()

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
)

target_compile_definitions(neopico_hd PRIVATE
    NEOPICO_VIDEO_240P=${VIDEO_240P_VALUE}
    NEOPICO_VIDEO_720P=${VIDEO_720P_VALUE}
    NEOPICO_USE_NONRT_HDMI=${USE_NONRT_HDMI_VALUE}
    NEOPICO_VIDEO_DVI_ONLY=${VIDEO_DVI_ONLY_VALUE}
    NEOPICO_VIDEO_TEST_PATTERN=${VIDEO_TEST_PATTERN_VALUE}
    NEOPICO_ENABLE_OSD=${ENABLE_OSD_VALUE}
    NEOPICO_ENABLE_SELFTEST=${ENABLE_SELFTEST_VALUE}
    ENABLE_DARK_SHADOW=${ENABLE_DARK_SHADOW_VALUE}
    NEOPICO_EXP_GENLOCK_STATIC=${GENLOCK_STATIC_VALUE}
    NEOPICO_GENLOCK_TARGET_FPS_X100=${NEOPICO_GENLOCK_TARGET_FPS_X100}
    NEOPICO_EXP_VTOTAL_MATCH=${VTOTAL_MATCH_VALUE}
    NEOPICO_EXP_VTOTAL_LINES=${NEOPICO_EXP_VTOTAL_LINES}
    NEOPICO_EXP_GENLOCK_DYNAMIC=${GENLOCK_DYNAMIC_VALUE}
    NEOPICO_EXP_REBOOT_MODE_SWITCH=${REBOOT_MODE_SWITCH_VALUE}
    NEOPICO_AUDIO_PCM1802=${AUDIO_PCM1802_VALUE}
    NEOPICO_CAPTURE_FREEZE_AFTER_FRAME=${CAPTURE_FREEZE_AFTER_FRAME_VALUE}
    NEOPICO_LINE_RING_SIZE=${NEOPICO_LINE_RING_SIZE}
)

pico_enable_stdio_usb(neopico_hd 1)
pico_enable_stdio_uart(neopico_hd 0)
pico_add_extra_outputs(neopico_hd)

# =============================================================================
# Standalone Self-Test Firmware (HDMI + OSD, no MVS capture)
# =============================================================================
option(NEOPICO_BUILD_SELFTEST "Build standalone selftest firmware" OFF)

if(NEOPICO_BUILD_SELFTEST)
add_executable(neopico_selftest
    selftest.c
    osd/fast_osd.c
    osd/selftest_layout.c
)

target_include_directories(neopico_selftest PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}
    ${CMAKE_CURRENT_LIST_DIR}/osd
)

target_link_libraries(neopico_selftest
    pico_hdmi
    pico_stdlib
    pico_multicore
    hardware_clocks
    hardware_gpio
)

target_compile_definitions(neopico_selftest PRIVATE
    PICO_CORE1_STACK_SIZE=1536
)

pico_enable_stdio_usb(neopico_selftest 1)
pico_enable_stdio_uart(neopico_selftest 0)
pico_add_extra_outputs(neopico_selftest)
endif()
