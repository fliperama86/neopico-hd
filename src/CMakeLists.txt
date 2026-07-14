# =============================================================================
# NeoPico-HD Main Firmware (MVS capture + HSTX output)
# =============================================================================
set(NEOPICO_CAPTURE_TARGET "MVS" CACHE STRING "Capture target: MVS or SNES")
set_property(CACHE NEOPICO_CAPTURE_TARGET PROPERTY STRINGS MVS SNES)
string(TOUPPER "${NEOPICO_CAPTURE_TARGET}" NEOPICO_CAPTURE_TARGET_UPPER)
if(NEOPICO_CAPTURE_TARGET_UPPER STREQUAL "MVS")
    set(CAPTURE_TARGET_VALUE 0)
    set(NEOPICO_CAPTURE_SOURCE video/video_capture_mvs.c)
    set(NEOPICO_CAPTURE_PIO video/video_capture_mvs.pio)
elseif(NEOPICO_CAPTURE_TARGET_UPPER STREQUAL "SNES")
    set(CAPTURE_TARGET_VALUE 1)
    set(NEOPICO_CAPTURE_SOURCE video/video_capture_snes.c)
    set(NEOPICO_CAPTURE_PIO video/video_capture_snes.pio)
else()
    message(FATAL_ERROR "NEOPICO_CAPTURE_TARGET must be MVS or SNES")
endif()

add_executable(neopico_hd
    main.c
    experiments/menu_diag_experiment.c
    ${NEOPICO_CAPTURE_SOURCE}
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
    video/scale_pixels.S
    settings.c
)

# Product configuration and active experiments
option(NEOPICO_VIDEO_240P "Start HDMI output in 240p mode via runtime mode API" OFF)
option(NEOPICO_VIDEO_720P "Build HDMI output in 1280x720 @ 60Hz mode (requires sys_clk 372 MHz / 1.30V)" OFF)
# When ON, link the compile-time pico_hdmi path (video_output.c). Loses the
# rt-only API surface (set_mode, update_acr, dynamic vtotal genlock, etc.) —
# code that uses those is #ifdef'd out. Use as an A/B against the rt path
# when chasing 720p stability.
option(NEOPICO_USE_NONRT_HDMI "Use compile-time pico_hdmi path instead of runtime-mode" OFF)
option(NEOPICO_COPY_TO_RAM "Copy the entire binary to SRAM at boot and execute from RAM (eliminates XIP flash-fetch timing at runtime; text+data must fit alongside bss)" OFF)
option(NEOPICO_EXP_PRECOMPOSED_HDMI "EXPERIMENTAL: pico_hdmi 2.0 precomposed scanout + native-pixel pointer path (requires NEOPICO_USE_NONRT_HDMI; 480p only)" OFF)
option(NEOPICO_VIDEO_DVI_ONLY "Disable HDMI Data Islands/audio output for video isolation tests" OFF)
option(NEOPICO_VIDEO_TEST_PATTERN "Render a static Core 1 video test pattern instead of captured video" OFF)
option(NEOPICO_DIAG_COUNTERS "Capture-health counters dumped over USB-serial (720p glitch diagnosis)" OFF)
option(NEOPICO_ENABLE_OSD "Enable OSD overlay and button handling" ON)
option(NEOPICO_ENABLE_SELFTEST "Enable self-test diagnostic layout (requires OSD)" ON)
option(NEOPICO_OSD_ROOT_MENU "Root OSD menu for enabled settings and diagnostics (requires OSD and at least one entry feature)" ON)
if(NEOPICO_CAPTURE_TARGET_UPPER STREQUAL "MVS")
    set(_NEOPICO_OSD_CONTROLLER_INPUTS_DEFAULT ON)
else()
    set(_NEOPICO_OSD_CONTROLLER_INPUTS_DEFAULT OFF)
endif()
option(NEOPICO_OSD_CONTROLLER_INPUTS "Use active-low controller taps as additional OSD inputs (default ON for MVS/AES, OFF for SNES)" ${_NEOPICO_OSD_CONTROLLER_INPUTS_DEFAULT})
option(NEOPICO_OSD_FAKE_BLEND "EXPERIMENTAL: fake a translucent OSD panel by dimming game pixels where OSD background pixels are black; OSD text/colors remain opaque" OFF)
set(NEOPICO_OSD_CONTROLLER_MENU_PIN "0" CACHE STRING "GPIO for controller START/MENU when controller OSD inputs are enabled")
set(NEOPICO_OSD_CONTROLLER_BACK_PIN "1" CACHE STRING "GPIO for controller SELECT/BACK when controller OSD inputs are enabled")
set(NEOPICO_OSD_CONTROLLER_UP_PIN "3" CACHE STRING "GPIO for controller UP when controller OSD inputs are enabled")
set(NEOPICO_OSD_CONTROLLER_DOWN_PIN "2" CACHE STRING "GPIO for controller DOWN when controller OSD inputs are enabled")
set(NEOPICO_OSD_CONTROLLER_DEBOUNCE_MS "20" CACHE STRING "Controller OSD stable-release debounce time in milliseconds")
option(NEOPICO_ENABLE_DARK_SHADOW "Enable true DARK/SHADOW LUT path in capture (128KB quantized LUT)" OFF)
option(NEOPICO_EXP_GENLOCK_STATIC "Enable static boot-time genlock sysclk target" OFF)
set(NEOPICO_GENLOCK_TARGET_FPS_X100 "5920" CACHE STRING "Static genlock target FPS x100 (e.g. 5920 = 59.20)")
option(NEOPICO_EXP_VTOTAL_MATCH "Enable fixed-pixel-clock output with custom V total for near-source frame rate" OFF)
set(NEOPICO_EXP_VTOTAL_LINES "532" CACHE STRING "Custom V total lines when NEOPICO_EXP_VTOTAL_MATCH is ON")
option(NEOPICO_EXP_GENLOCK_DYNAMIC "Enable dynamic VTOTAL genlock (runtime phase tracking)" OFF)
option(NEOPICO_TRIPLE_ASM "Use hand-written M33 asm for the 3x/4x pixel-scale kernels (scale_pixels.S); boot self-test gates equivalence" OFF)
# Baseline: the resolution selector (reboot-based 240p/480p/720p switching via
# the OSD) is the default firmware. One build does all three modes; pass these
# OFF for a fixed-resolution build (then select a single mode with VIDEO_240P /
# VIDEO_720P, which are mutually exclusive with the selector).
option(NEOPICO_RESOLUTION_MENU "Reboot-based 240p/480p/720p HDMI mode switching via the OSD resolution menu" ON)
option(NEOPICO_RESOLUTION_MENU_720P "Let the OSD resolution menu cycle through 720p" ON)
option(NEOPICO_FIRST_BOOT_REBOOT "Auto-reboot once on cold power-on (cold-boot scratchy-audio workaround: re-establishes the HDMI link warm); requires the reboot-mode path. ~1-2s extra on cold boot." ON)
option(NEOPICO_SETTINGS_FLASH "Persist settings to flash so they survive power-off; written only at reboot points" ON)
option(NEOPICO_OSD_RES_CONFIRM "Resolution-change safety net: after switching, show a 10s keep/revert countdown (MENU keeps + persists, BACK/timeout reverts). Requires the reboot-mode path + OSD." ON)
if(NEOPICO_CAPTURE_TARGET_UPPER STREQUAL "MVS")
    set(_NEOPICO_AUDIO_MODE_DEFAULT "SELECTABLE")
else()
    set(_NEOPICO_AUDIO_MODE_DEFAULT "DIGITAL")
endif()
set(NEOPICO_AUDIO_MODE "${_NEOPICO_AUDIO_MODE_DEFAULT}" CACHE STRING
    "Audio capture mode: DIGITAL, PCM1802, or SELECTABLE (MVS OSD selector)")
set_property(CACHE NEOPICO_AUDIO_MODE PROPERTY STRINGS DIGITAL PCM1802 SELECTABLE)
option(NEOPICO_DIAG_AUDIO_OSD "Show HDMI audio underrun (silence splice) counter on the selftest OSD screen" OFF)
set(NEOPICO_LINE_RING_SIZE "256" CACHE STRING "Number of 320px lines in the capture/display ring")

string(TOUPPER "${NEOPICO_AUDIO_MODE}" NEOPICO_AUDIO_MODE_UPPER)
if(NEOPICO_AUDIO_MODE_UPPER STREQUAL "DIGITAL")
    set(AUDIO_MODE_VALUE 0)
elseif(NEOPICO_AUDIO_MODE_UPPER STREQUAL "PCM1802")
    set(AUDIO_MODE_VALUE 1)
elseif(NEOPICO_AUDIO_MODE_UPPER STREQUAL "SELECTABLE")
    set(AUDIO_MODE_VALUE 2)
    if(NOT NEOPICO_CAPTURE_TARGET_UPPER STREQUAL "MVS")
        message(FATAL_ERROR "NEOPICO_AUDIO_MODE=SELECTABLE currently supports only the MVS capture target")
    endif()
    if(NOT NEOPICO_ENABLE_OSD OR NOT NEOPICO_OSD_ROOT_MENU)
        message(FATAL_ERROR "NEOPICO_AUDIO_MODE=SELECTABLE requires NEOPICO_ENABLE_OSD and NEOPICO_OSD_ROOT_MENU")
    endif()
    if(NOT NEOPICO_SETTINGS_FLASH)
        message(FATAL_ERROR "NEOPICO_AUDIO_MODE=SELECTABLE requires NEOPICO_SETTINGS_FLASH")
    endif()
    if(NOT NEOPICO_RESOLUTION_MENU OR NEOPICO_USE_NONRT_HDMI)
        message(FATAL_ERROR "NEOPICO_AUDIO_MODE=SELECTABLE requires the runtime HDMI reboot-mode selector")
    endif()
else()
    message(FATAL_ERROR "NEOPICO_AUDIO_MODE must be DIGITAL, PCM1802, or SELECTABLE")
endif()

if(NEOPICO_ENABLE_OSD)
    set(ENABLE_OSD_VALUE 1)
else()
    set(ENABLE_OSD_VALUE 0)
endif()

if(NEOPICO_OSD_FAKE_BLEND)
    if(NOT NEOPICO_ENABLE_OSD)
        message(FATAL_ERROR "NEOPICO_OSD_FAKE_BLEND requires NEOPICO_ENABLE_OSD=ON")
    endif()
    if(NEOPICO_EXP_PRECOMPOSED_HDMI)
        message(FATAL_ERROR "NEOPICO_OSD_FAKE_BLEND does not support NEOPICO_EXP_PRECOMPOSED_HDMI")
    endif()
    set(OSD_FAKE_BLEND_VALUE 1)
else()
    set(OSD_FAKE_BLEND_VALUE 0)
endif()

if(NEOPICO_ENABLE_SELFTEST)
    set(ENABLE_SELFTEST_VALUE 1)
else()
    set(ENABLE_SELFTEST_VALUE 0)
endif()

if(NEOPICO_OSD_CONTROLLER_INPUTS)
    set(OSD_CONTROLLER_INPUTS_VALUE 1)
else()
    set(OSD_CONTROLLER_INPUTS_VALUE 0)
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

if(NEOPICO_TRIPLE_ASM)
    set(TRIPLE_ASM_VALUE 1)
else()
    set(TRIPLE_ASM_VALUE 0)
endif()

if(NEOPICO_RESOLUTION_MENU)
    set(RESOLUTION_MENU_VALUE 1)
else()
    set(RESOLUTION_MENU_VALUE 0)
endif()

if(NEOPICO_RESOLUTION_MENU_720P)
    set(RESOLUTION_MENU_720P_VALUE 1)
else()
    set(RESOLUTION_MENU_720P_VALUE 0)
endif()

if(NEOPICO_DIAG_AUDIO_OSD)
    set(DIAG_AUDIO_OSD_VALUE 1)
else()
    set(DIAG_AUDIO_OSD_VALUE 0)
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

if(NEOPICO_RESOLUTION_MENU)
    if(NEOPICO_USE_NONRT_HDMI)
        message(FATAL_ERROR "NEOPICO_RESOLUTION_MENU requires the runtime pico_hdmi path")
    endif()
    if(NEOPICO_VIDEO_720P)
        message(FATAL_ERROR "NEOPICO_RESOLUTION_MENU selects 720p at boot from watchdog scratch; use NEOPICO_RESOLUTION_MENU_720P instead of NEOPICO_VIDEO_720P")
    endif()
    if(NOT NEOPICO_ENABLE_OSD)
        message(FATAL_ERROR "NEOPICO_RESOLUTION_MENU requires NEOPICO_ENABLE_OSD=ON")
    endif()
endif()

if(NEOPICO_RESOLUTION_MENU_720P AND NOT NEOPICO_RESOLUTION_MENU)
    message(FATAL_ERROR "NEOPICO_RESOLUTION_MENU_720P requires NEOPICO_RESOLUTION_MENU=ON")
endif()

# Overclock builds MUST execute from RAM. Above ~150 MHz sys_clk, XIP flash
# reads corrupt (the boot-clock QMI timing samples too early) -> the next
# instruction fetch faults -> hard crash with no recovery. copy_to_ram reads
# flash once at the safe boot clock, then runs from RAM. Overclocked modes:
#   - 720p:    372 MHz (VIDEO_720P, or the reboot-switch 720p mode)
#   - RT 480p: 252 MHz (scanline-IRQ headroom; default 480p + the selector)
set(_neopico_overclocked OFF)
if(NEOPICO_VIDEO_720P OR NEOPICO_RESOLUTION_MENU_720P)
    set(_neopico_overclocked ON)
endif()
if((NOT NEOPICO_USE_NONRT_HDMI) AND (NOT NEOPICO_VIDEO_240P) AND (NOT NEOPICO_VIDEO_720P))
    # RT 480p (default build) and the runtime selector both run 480p at 252 MHz.
    set(_neopico_overclocked ON)
endif()
if(_neopico_overclocked AND NOT NEOPICO_COPY_TO_RAM)
    message(STATUS "Overclocked build (>150 MHz sys_clk) selected: forcing NEOPICO_COPY_TO_RAM=ON to avoid XIP flash corruption")
    set(NEOPICO_COPY_TO_RAM ON CACHE BOOL
        "Copy the entire binary to SRAM at boot and execute from RAM" FORCE)
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

if(NEOPICO_DIAG_COUNTERS)
    set(DIAG_COUNTERS_VALUE 1)
else()
    set(DIAG_COUNTERS_VALUE 0)
endif()

if(NEOPICO_EXP_PRECOMPOSED_HDMI)
    # pico_hdmi 2.1: precomposed mode is supported on BOTH library paths.
    # Still 480p-only on the app side (native pixel mode is a 2x doubler);
    # runtime mode switching into 240p/720p is not yet precomposed-aware.
    if (NEOPICO_VIDEO_240P)
        message(FATAL_ERROR "NEOPICO_EXP_PRECOMPOSED_HDMI does not support 240p yet (S3b: pre-doubled ring)")
    endif()
    if(NEOPICO_RESOLUTION_MENU)
        message(FATAL_ERROR "NEOPICO_EXP_PRECOMPOSED_HDMI + mode switching is stage S4 of the 2.1 plan; not wired yet")
    endif()
endif()

# Pick rt vs compile-time pico_hdmi path
if(NEOPICO_USE_NONRT_HDMI)
    set(PICO_HDMI_RUNTIME_MODES OFF CACHE BOOL "" FORCE)
else()
    set(PICO_HDMI_RUNTIME_MODES ON CACHE BOOL "" FORCE)
endif()

if(NEOPICO_EXP_PRECOMPOSED_HDMI)
    set(PICO_HDMI_PRECOMPOSED_ACTIVE_LINES ON CACHE BOOL "" FORCE)
else()
    set(PICO_HDMI_PRECOMPOSED_ACTIVE_LINES OFF CACHE BOOL "" FORCE)
endif()

# Add pico_hdmi library (MUST be after add_compile_definitions and option overrides)
add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/../lib/pico_hdmi ${CMAKE_CURRENT_BINARY_DIR}/pico_hdmi)

# 720p static builds require compile-time polarity / DI placement in pico_hdmi (VIDEO_MODE_1280x720).
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

if(NEOPICO_RESOLUTION_MENU_720P)
    target_compile_definitions(pico_hdmi PRIVATE PICO_HDMI_RT_RUNTIME_MODE_ATTRS=1)
endif()

# 240p uses a non-CEA VIC-0 timing. Keep the conservative AVI payload that was
# hardware-validated with the normal HDMI audio stream: PB1=0x00, PB2=0x08,
# and PR=0. The newer active-format/aspect/PR=3 payload causes black video on
# tested scaler paths. This define changes only VIC-0/240p metadata; 480p and
# 720p keep their standard AVI payloads.
target_compile_definitions(pico_hdmi PRIVATE PICO_HDMI_LEGACY_240P_AVI_INFOFRAME=1)

# RT 480p runs at 252 MHz for scanline-IRQ headroom (see main.c SYS_CLK_480P_KHZ).
# Halve the 480p HSTX clk divider in the library so the pixel clock stays
# 25.2 MHz (252/(2*5)) and the 480p signal is byte-identical to the 126 MHz one.
if(NOT NEOPICO_USE_NONRT_HDMI)
    target_compile_definitions(pico_hdmi PRIVATE PICO_HDMI_480P_HSTX_CLK_DIV=2)
endif()

if(NEOPICO_EXP_PRECOMPOSED_HDMI)
    target_compile_definitions(neopico_hd PRIVATE NEOPICO_EXP_PRECOMPOSED_HDMI=1)
endif()

if(NEOPICO_COPY_TO_RAM)
    pico_set_binary_type(neopico_hd copy_to_ram)
endif()

if(NEOPICO_OSD_ROOT_MENU)
    if(NOT NEOPICO_ENABLE_OSD)
        message(FATAL_ERROR "NEOPICO_OSD_ROOT_MENU requires NEOPICO_ENABLE_OSD=ON")
    endif()
    if(NOT NEOPICO_ENABLE_SELFTEST AND NOT NEOPICO_RESOLUTION_MENU
       AND NOT NEOPICO_AUDIO_MODE_UPPER STREQUAL "SELECTABLE")
        message(FATAL_ERROR "NEOPICO_OSD_ROOT_MENU needs at least one enabled entry")
    endif()
    target_compile_definitions(neopico_hd PRIVATE NEOPICO_OSD_ROOT_MENU=1)
endif()

if(NEOPICO_FIRST_BOOT_REBOOT)
    if(NEOPICO_USE_NONRT_HDMI OR NOT NEOPICO_RESOLUTION_MENU)
        message(FATAL_ERROR "NEOPICO_FIRST_BOOT_REBOOT requires the runtime reboot-mode path (NEOPICO_RESOLUTION_MENU=ON, NEOPICO_USE_NONRT_HDMI=OFF)")
    endif()
    target_compile_definitions(neopico_hd PRIVATE NEOPICO_FIRST_BOOT_REBOOT=1)
endif()

if(NEOPICO_SETTINGS_FLASH)
    target_compile_definitions(neopico_hd PRIVATE NEOPICO_SETTINGS_FLASH=1)
endif()

if(NEOPICO_OSD_RES_CONFIRM)
    if(NEOPICO_USE_NONRT_HDMI OR NOT NEOPICO_RESOLUTION_MENU OR NOT NEOPICO_ENABLE_OSD)
        message(FATAL_ERROR "NEOPICO_OSD_RES_CONFIRM requires the runtime resolution menu")
    endif()
    target_compile_definitions(neopico_hd PRIVATE NEOPICO_OSD_RES_CONFIRM=1)
endif()

pico_generate_pio_header(neopico_hd ${CMAKE_CURRENT_LIST_DIR}/${NEOPICO_CAPTURE_PIO})
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
    hardware_flash
)

target_compile_definitions(neopico_hd PRIVATE
    NEOPICO_VERSION="${PROJECT_VERSION}"
    NEOPICO_CAPTURE_TARGET=${CAPTURE_TARGET_VALUE}
    NEOPICO_VIDEO_240P=${VIDEO_240P_VALUE}
    NEOPICO_VIDEO_720P=${VIDEO_720P_VALUE}
    NEOPICO_USE_NONRT_HDMI=${USE_NONRT_HDMI_VALUE}
    NEOPICO_VIDEO_DVI_ONLY=${VIDEO_DVI_ONLY_VALUE}
    NEOPICO_VIDEO_TEST_PATTERN=${VIDEO_TEST_PATTERN_VALUE}
    NEOPICO_DIAG_COUNTERS=${DIAG_COUNTERS_VALUE}
    NEOPICO_ENABLE_OSD=${ENABLE_OSD_VALUE}
    NEOPICO_OSD_FAKE_BLEND=${OSD_FAKE_BLEND_VALUE}
    NEOPICO_ENABLE_SELFTEST=${ENABLE_SELFTEST_VALUE}
    NEOPICO_OSD_CONTROLLER_INPUTS=${OSD_CONTROLLER_INPUTS_VALUE}
    NEOPICO_OSD_CONTROLLER_MENU_PIN=${NEOPICO_OSD_CONTROLLER_MENU_PIN}
    NEOPICO_OSD_CONTROLLER_BACK_PIN=${NEOPICO_OSD_CONTROLLER_BACK_PIN}
    NEOPICO_OSD_CONTROLLER_UP_PIN=${NEOPICO_OSD_CONTROLLER_UP_PIN}
    NEOPICO_OSD_CONTROLLER_DOWN_PIN=${NEOPICO_OSD_CONTROLLER_DOWN_PIN}
    NEOPICO_OSD_CONTROLLER_DEBOUNCE_MS=${NEOPICO_OSD_CONTROLLER_DEBOUNCE_MS}
    ENABLE_DARK_SHADOW=${ENABLE_DARK_SHADOW_VALUE}
    NEOPICO_EXP_GENLOCK_STATIC=${GENLOCK_STATIC_VALUE}
    NEOPICO_GENLOCK_TARGET_FPS_X100=${NEOPICO_GENLOCK_TARGET_FPS_X100}
    NEOPICO_EXP_VTOTAL_MATCH=${VTOTAL_MATCH_VALUE}
    NEOPICO_EXP_VTOTAL_LINES=${NEOPICO_EXP_VTOTAL_LINES}
    NEOPICO_EXP_GENLOCK_DYNAMIC=${GENLOCK_DYNAMIC_VALUE}
    NEOPICO_TRIPLE_ASM=${TRIPLE_ASM_VALUE}
    NEOPICO_RESOLUTION_MENU=${RESOLUTION_MENU_VALUE}
    NEOPICO_RESOLUTION_MENU_720P=${RESOLUTION_MENU_720P_VALUE}
    NEOPICO_AUDIO_MODE=${AUDIO_MODE_VALUE}
    NEOPICO_DIAG_AUDIO_OSD=${DIAG_AUDIO_OSD_VALUE}
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
