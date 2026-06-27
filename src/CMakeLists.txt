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
    video/scale_pixels.S
    settings.c
)

# Compile-time feature flags (cache params for fast A/B testing)
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
option(NEOPICO_OSD_ROOT_MENU "Root OSD menu with Resolution / Self Test entries (requires OSD and at least one entry feature)" ON)
option(NEOPICO_OSD_BOOT_OPEN "Boot with the OSD open on the Genlock telemetry screen (soak aid); default OFF = OSD starts hidden, opened by the MENU button" OFF)
set(NEOPICO_EXP_STRESS_CORE1_US "0" CACHE STRING "Diagnostics: busy-wait this many microseconds per Core 1 background tick to accelerate IRQ-timing repro (0 = off)")
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
option(NEOPICO_EXP_REBOOT_MODE_SWITCH "Reboot-based 240p/480p/720p HDMI mode switching via the OSD resolution menu" ON)
option(NEOPICO_EXP_REBOOT_MODE_SWITCH_720P "Let the OSD resolution menu cycle through 720p" ON)
option(NEOPICO_EXP_FIRST_BOOT_REBOOT "Auto-reboot once on cold power-on (cold-boot scratchy-audio workaround: re-establishes the HDMI link warm); requires the reboot-mode path. ~1-2s extra on cold boot." ON)
option(NEOPICO_SETTINGS_FLASH "Persist settings (resolution) to flash so they survive power-off; loaded at cold boot, written at the reboot point" ON)
option(NEOPICO_OSD_RES_CONFIRM "Resolution-change safety net: after switching, show a 10s keep/revert countdown (MENU keeps + persists, BACK/timeout reverts). Requires the reboot-mode path + OSD." ON)
option(NEOPICO_EXP_REBOOT_BUTTON_CYCLER "Enable bare BACK-button reboot-mode cycling without the OSD/menu module" OFF)
option(NEOPICO_EXP_DISABLE_REBOOT_SELECTOR_UI "Compile reboot-mode plumbing but disable the OSD resolution selector UI for timing isolation" OFF)
option(NEOPICO_EXP_RAM_SELECTOR_UI "Place reboot selector UI/render helpers in SRAM for timing isolation" OFF)
option(NEOPICO_EXP_STATIC_OSD_TOGGLE_ONLY "Pre-render reboot selector OSD once and only toggle visibility with MENU for timing isolation" OFF)
option(NEOPICO_EXP_STATIC_OSD_SELECT_ONLY "Pre-render reboot selector OSD once; MENU toggles visibility and BACK only updates selection glyphs" OFF)
option(NEOPICO_EXP_STATIC_OSD_APPLY "Pre-render reboot selector OSD once; MENU applies when visible and BACK only updates selection glyphs" OFF)
option(NEOPICO_EXP_RAM_OSD_APPLY_PATH "Place cold OSD apply/reboot request path in SRAM and keep it noinline for timing isolation" OFF)
option(NEOPICO_EXP_DISABLE_BACKGROUND_TASK "Disable the Core 1 background task for timing isolation" OFF)
option(NEOPICO_EXP_DISABLE_AUDIO_BACKGROUND "Disable only the audio branch of the Core 1 background task" OFF)
option(NEOPICO_EXP_DISABLE_OSD_BACKGROUND "Disable only the OSD/menu branch of the Core 1 background task" OFF)
option(NEOPICO_EXP_AUDIO_NO_CAPTURE_START "Initialize audio but do not start I2S capture DMA/PIO for timing isolation" OFF)
option(NEOPICO_EXP_AUDIO_CAPTURE_ONLY "Start I2S capture DMA/PIO but skip audio processing/output for timing isolation" OFF)
option(NEOPICO_EXP_AUDIO_PROCESS_NO_OUTPUT "Process captured audio but suppress HDMI audio enqueue for timing isolation" OFF)
option(NEOPICO_EXP_AUDIO_ENCODE_NO_QUEUE "Encode HDMI audio packets but suppress DI queue push for timing isolation" OFF)
option(NEOPICO_EXP_AUDIO_LIMIT_BACKGROUND_WORK "Limit Core 1 audio background processing to one chunk per pass" OFF)
option(NEOPICO_EXP_AUDIO_LIMIT_OUTPUT_PACKETS "Limit Core 1 audio output to one HDMI packet per background pass" OFF)
option(NEOPICO_EXP_AUDIO_PUSH_STATIC_SILENCE "Push a pre-encoded static silent HDMI audio packet for timing isolation" OFF)
option(NEOPICO_EXP_AUDIO_STARTUP_REARM "After initial muted audio warmup, stop/restart I2S PIO/DMA once and re-sync before unmuting (scratchy-boot alignment workaround)" ON)
option(NEOPICO_EXP_AUDIO_REARM_ON_VIDEO_REACQUIRE "Audio-only re-arm after MVS video/sync loss and reacquire (flashcart reset distortion fix)" ON)
option(NEOPICO_EXP_AUDIO_FRAME_RESYNC "Use I2S PIO capture that re-syncs to WS every stereo frame, preventing persistent mis-framing after brief audio-clock glitches" ON)
option(NEOPICO_EXP_AUDIO_MANUAL_REARM "Temporary debug: hold BACK to audio-only re-arm I2S PIO/DMA" OFF)
option(NEOPICO_EXP_LEGACY_240P_AVI_INFOFRAME "Use v0.1.5-style 240p AVI InfoFrame metadata for compatibility isolation" OFF)
option(NEOPICO_EXP_ALIGN_HSTX_DI_BUFFERS "Align runtime HSTX Data Island command-list buffers for timing isolation" OFF)
option(NEOPICO_EXP_RAM_DI_QUEUE_PUSH "Place hstx_di_queue_push in SRAM for timing isolation" OFF)
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

if(NEOPICO_TRIPLE_ASM)
    set(TRIPLE_ASM_VALUE 1)
else()
    set(TRIPLE_ASM_VALUE 0)
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

if(NEOPICO_EXP_REBOOT_MODE_SWITCH_720P)
    set(REBOOT_MODE_SWITCH_720P_VALUE 1)
else()
    set(REBOOT_MODE_SWITCH_720P_VALUE 0)
endif()

if(NEOPICO_EXP_REBOOT_BUTTON_CYCLER)
    set(REBOOT_BUTTON_CYCLER_VALUE 1)
else()
    set(REBOOT_BUTTON_CYCLER_VALUE 0)
endif()

if(NEOPICO_EXP_DISABLE_REBOOT_SELECTOR_UI)
    set(DISABLE_REBOOT_SELECTOR_UI_VALUE 1)
else()
    set(DISABLE_REBOOT_SELECTOR_UI_VALUE 0)
endif()

if(NEOPICO_EXP_RAM_SELECTOR_UI)
    set(RAM_SELECTOR_UI_VALUE 1)
else()
    set(RAM_SELECTOR_UI_VALUE 0)
endif()

if(NEOPICO_EXP_STATIC_OSD_TOGGLE_ONLY)
    set(STATIC_OSD_TOGGLE_ONLY_VALUE 1)
else()
    set(STATIC_OSD_TOGGLE_ONLY_VALUE 0)
endif()

if(NEOPICO_EXP_STATIC_OSD_SELECT_ONLY)
    set(STATIC_OSD_SELECT_ONLY_VALUE 1)
else()
    set(STATIC_OSD_SELECT_ONLY_VALUE 0)
endif()

if(NEOPICO_EXP_STATIC_OSD_APPLY)
    set(STATIC_OSD_APPLY_VALUE 1)
else()
    set(STATIC_OSD_APPLY_VALUE 0)
endif()

if(NEOPICO_EXP_RAM_OSD_APPLY_PATH)
    set(RAM_OSD_APPLY_PATH_VALUE 1)
else()
    set(RAM_OSD_APPLY_PATH_VALUE 0)
endif()

if(NEOPICO_EXP_DISABLE_BACKGROUND_TASK)
    set(DISABLE_BACKGROUND_TASK_VALUE 1)
else()
    set(DISABLE_BACKGROUND_TASK_VALUE 0)
endif()

if(NEOPICO_EXP_DISABLE_AUDIO_BACKGROUND)
    set(DISABLE_AUDIO_BACKGROUND_VALUE 1)
else()
    set(DISABLE_AUDIO_BACKGROUND_VALUE 0)
endif()

if(NEOPICO_EXP_DISABLE_OSD_BACKGROUND)
    set(DISABLE_OSD_BACKGROUND_VALUE 1)
else()
    set(DISABLE_OSD_BACKGROUND_VALUE 0)
endif()

if(NEOPICO_EXP_AUDIO_NO_CAPTURE_START)
    set(AUDIO_NO_CAPTURE_START_VALUE 1)
else()
    set(AUDIO_NO_CAPTURE_START_VALUE 0)
endif()

if(NEOPICO_EXP_AUDIO_CAPTURE_ONLY)
    set(AUDIO_CAPTURE_ONLY_VALUE 1)
else()
    set(AUDIO_CAPTURE_ONLY_VALUE 0)
endif()

if(NEOPICO_EXP_AUDIO_PROCESS_NO_OUTPUT)
    set(AUDIO_PROCESS_NO_OUTPUT_VALUE 1)
else()
    set(AUDIO_PROCESS_NO_OUTPUT_VALUE 0)
endif()

if(NEOPICO_EXP_AUDIO_ENCODE_NO_QUEUE)
    set(AUDIO_ENCODE_NO_QUEUE_VALUE 1)
else()
    set(AUDIO_ENCODE_NO_QUEUE_VALUE 0)
endif()

if(NEOPICO_EXP_AUDIO_LIMIT_BACKGROUND_WORK)
    set(AUDIO_LIMIT_BACKGROUND_WORK_VALUE 1)
else()
    set(AUDIO_LIMIT_BACKGROUND_WORK_VALUE 0)
endif()

if(NEOPICO_EXP_AUDIO_LIMIT_OUTPUT_PACKETS)
    set(AUDIO_LIMIT_OUTPUT_PACKETS_VALUE 1)
else()
    set(AUDIO_LIMIT_OUTPUT_PACKETS_VALUE 0)
endif()

if(NEOPICO_EXP_AUDIO_PUSH_STATIC_SILENCE)
    set(AUDIO_PUSH_STATIC_SILENCE_VALUE 1)
else()
    set(AUDIO_PUSH_STATIC_SILENCE_VALUE 0)
endif()

if(NEOPICO_EXP_AUDIO_STARTUP_REARM)
    set(AUDIO_STARTUP_REARM_VALUE 1)
else()
    set(AUDIO_STARTUP_REARM_VALUE 0)
endif()

if(NEOPICO_EXP_AUDIO_REARM_ON_VIDEO_REACQUIRE)
    set(AUDIO_REARM_ON_VIDEO_REACQUIRE_VALUE 1)
else()
    set(AUDIO_REARM_ON_VIDEO_REACQUIRE_VALUE 0)
endif()

if(NEOPICO_EXP_AUDIO_FRAME_RESYNC)
    set(AUDIO_FRAME_RESYNC_VALUE 1)
else()
    set(AUDIO_FRAME_RESYNC_VALUE 0)
endif()

if(NEOPICO_EXP_AUDIO_MANUAL_REARM)
    set(AUDIO_MANUAL_REARM_VALUE 1)
else()
    set(AUDIO_MANUAL_REARM_VALUE 0)
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
        message(FATAL_ERROR "NEOPICO_EXP_REBOOT_MODE_SWITCH selects 720p at boot from watchdog scratch; use NEOPICO_EXP_REBOOT_MODE_SWITCH_720P instead of NEOPICO_VIDEO_720P")
    endif()
    if(NOT NEOPICO_ENABLE_OSD AND NOT NEOPICO_EXP_REBOOT_BUTTON_CYCLER)
        message(FATAL_ERROR "NEOPICO_EXP_REBOOT_MODE_SWITCH requires NEOPICO_ENABLE_OSD=ON or NEOPICO_EXP_REBOOT_BUTTON_CYCLER=ON")
    endif()
endif()

if(NEOPICO_EXP_REBOOT_BUTTON_CYCLER AND NOT NEOPICO_EXP_REBOOT_MODE_SWITCH)
    message(FATAL_ERROR "NEOPICO_EXP_REBOOT_BUTTON_CYCLER requires NEOPICO_EXP_REBOOT_MODE_SWITCH=ON")
endif()

if(NEOPICO_EXP_REBOOT_MODE_SWITCH_720P AND NOT NEOPICO_EXP_REBOOT_MODE_SWITCH)
    message(FATAL_ERROR "NEOPICO_EXP_REBOOT_MODE_SWITCH_720P requires NEOPICO_EXP_REBOOT_MODE_SWITCH=ON")
endif()

# Overclock builds MUST execute from RAM. Above ~150 MHz sys_clk, XIP flash
# reads corrupt (the boot-clock QMI timing samples too early) -> the next
# instruction fetch faults -> hard crash with no recovery. copy_to_ram reads
# flash once at the safe boot clock, then runs from RAM. Overclocked modes:
#   - 720p:    372 MHz (VIDEO_720P, or the reboot-switch 720p mode)
#   - RT 480p: 252 MHz (scanline-IRQ headroom; default 480p + the selector)
set(_neopico_overclocked OFF)
if(NEOPICO_VIDEO_720P OR NEOPICO_EXP_REBOOT_MODE_SWITCH_720P)
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
    if(NEOPICO_EXP_REBOOT_MODE_SWITCH)
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

if(NEOPICO_EXP_REBOOT_MODE_SWITCH_720P)
    target_compile_definitions(pico_hdmi PRIVATE PICO_HDMI_RT_RUNTIME_MODE_ATTRS=1)
endif()

# RT 480p runs at 252 MHz for scanline-IRQ headroom (see main.c SYS_CLK_480P_KHZ).
# Halve the 480p HSTX clk divider in the library so the pixel clock stays
# 25.2 MHz (252/(2*5)) and the 480p signal is byte-identical to the 126 MHz one.
if(NOT NEOPICO_USE_NONRT_HDMI)
    target_compile_definitions(pico_hdmi PRIVATE PICO_HDMI_480P_HSTX_CLK_DIV=2)
endif()

if(NEOPICO_EXP_ALIGN_HSTX_DI_BUFFERS)
    target_compile_definitions(pico_hdmi PRIVATE PICO_HDMI_ALIGN_DI_BUFFERS=1)
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
    if(NOT NEOPICO_ENABLE_SELFTEST AND NOT NEOPICO_EXP_REBOOT_MODE_SWITCH)
        message(FATAL_ERROR "NEOPICO_OSD_ROOT_MENU needs at least one entry: NEOPICO_ENABLE_SELFTEST or NEOPICO_EXP_REBOOT_MODE_SWITCH")
    endif()
    target_compile_definitions(neopico_hd PRIVATE NEOPICO_OSD_ROOT_MENU=1)
endif()

if(NEOPICO_OSD_BOOT_OPEN)
    target_compile_definitions(neopico_hd PRIVATE NEOPICO_OSD_BOOT_OPEN=1)
endif()

if(NEOPICO_EXP_FIRST_BOOT_REBOOT)
    if(NEOPICO_USE_NONRT_HDMI OR NOT NEOPICO_EXP_REBOOT_MODE_SWITCH)
        message(FATAL_ERROR "NEOPICO_EXP_FIRST_BOOT_REBOOT requires the runtime reboot-mode path (NEOPICO_EXP_REBOOT_MODE_SWITCH=ON, NEOPICO_USE_NONRT_HDMI=OFF)")
    endif()
    target_compile_definitions(neopico_hd PRIVATE NEOPICO_EXP_FIRST_BOOT_REBOOT=1)
endif()

if(NEOPICO_SETTINGS_FLASH)
    target_compile_definitions(neopico_hd PRIVATE NEOPICO_SETTINGS_FLASH=1)
endif()

if(NEOPICO_OSD_RES_CONFIRM)
    if(NEOPICO_USE_NONRT_HDMI OR NOT NEOPICO_EXP_REBOOT_MODE_SWITCH OR NOT NEOPICO_ENABLE_OSD
       OR NEOPICO_EXP_DISABLE_REBOOT_SELECTOR_UI)
        message(FATAL_ERROR "NEOPICO_OSD_RES_CONFIRM requires the runtime reboot-mode resolution menu (NEOPICO_EXP_REBOOT_MODE_SWITCH=ON, NEOPICO_ENABLE_OSD=ON, NEOPICO_USE_NONRT_HDMI=OFF, NEOPICO_EXP_DISABLE_REBOOT_SELECTOR_UI=OFF)")
    endif()
    target_compile_definitions(neopico_hd PRIVATE NEOPICO_OSD_RES_CONFIRM=1)
endif()

if(NEOPICO_EXP_STRESS_CORE1_US GREATER 0)
    target_compile_definitions(neopico_hd PRIVATE NEOPICO_EXP_STRESS_CORE1_US=${NEOPICO_EXP_STRESS_CORE1_US})
endif()

if(NEOPICO_EXP_RAM_DI_QUEUE_PUSH)
    target_compile_definitions(pico_hdmi PRIVATE PICO_HDMI_RAM_DI_QUEUE_PUSH=1)
endif()

if(NEOPICO_EXP_LEGACY_240P_AVI_INFOFRAME)
    target_compile_definitions(pico_hdmi PRIVATE PICO_HDMI_LEGACY_240P_AVI_INFOFRAME=1)
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
    hardware_flash
)

target_compile_definitions(neopico_hd PRIVATE
    NEOPICO_VIDEO_240P=${VIDEO_240P_VALUE}
    NEOPICO_VIDEO_720P=${VIDEO_720P_VALUE}
    NEOPICO_USE_NONRT_HDMI=${USE_NONRT_HDMI_VALUE}
    NEOPICO_VIDEO_DVI_ONLY=${VIDEO_DVI_ONLY_VALUE}
    NEOPICO_VIDEO_TEST_PATTERN=${VIDEO_TEST_PATTERN_VALUE}
    NEOPICO_DIAG_COUNTERS=${DIAG_COUNTERS_VALUE}
    NEOPICO_ENABLE_OSD=${ENABLE_OSD_VALUE}
    NEOPICO_ENABLE_SELFTEST=${ENABLE_SELFTEST_VALUE}
    ENABLE_DARK_SHADOW=${ENABLE_DARK_SHADOW_VALUE}
    NEOPICO_EXP_GENLOCK_STATIC=${GENLOCK_STATIC_VALUE}
    NEOPICO_GENLOCK_TARGET_FPS_X100=${NEOPICO_GENLOCK_TARGET_FPS_X100}
    NEOPICO_EXP_VTOTAL_MATCH=${VTOTAL_MATCH_VALUE}
    NEOPICO_EXP_VTOTAL_LINES=${NEOPICO_EXP_VTOTAL_LINES}
    NEOPICO_EXP_GENLOCK_DYNAMIC=${GENLOCK_DYNAMIC_VALUE}
    NEOPICO_TRIPLE_ASM=${TRIPLE_ASM_VALUE}
    NEOPICO_EXP_REBOOT_MODE_SWITCH=${REBOOT_MODE_SWITCH_VALUE}
    NEOPICO_EXP_REBOOT_MODE_SWITCH_720P=${REBOOT_MODE_SWITCH_720P_VALUE}
    NEOPICO_EXP_REBOOT_BUTTON_CYCLER=${REBOOT_BUTTON_CYCLER_VALUE}
    NEOPICO_EXP_DISABLE_REBOOT_SELECTOR_UI=${DISABLE_REBOOT_SELECTOR_UI_VALUE}
    NEOPICO_EXP_RAM_SELECTOR_UI=${RAM_SELECTOR_UI_VALUE}
    NEOPICO_EXP_STATIC_OSD_TOGGLE_ONLY=${STATIC_OSD_TOGGLE_ONLY_VALUE}
    NEOPICO_EXP_STATIC_OSD_SELECT_ONLY=${STATIC_OSD_SELECT_ONLY_VALUE}
    NEOPICO_EXP_STATIC_OSD_APPLY=${STATIC_OSD_APPLY_VALUE}
    NEOPICO_EXP_RAM_OSD_APPLY_PATH=${RAM_OSD_APPLY_PATH_VALUE}
    NEOPICO_EXP_DISABLE_BACKGROUND_TASK=${DISABLE_BACKGROUND_TASK_VALUE}
    NEOPICO_EXP_DISABLE_AUDIO_BACKGROUND=${DISABLE_AUDIO_BACKGROUND_VALUE}
    NEOPICO_EXP_DISABLE_OSD_BACKGROUND=${DISABLE_OSD_BACKGROUND_VALUE}
    NEOPICO_EXP_AUDIO_NO_CAPTURE_START=${AUDIO_NO_CAPTURE_START_VALUE}
    NEOPICO_EXP_AUDIO_CAPTURE_ONLY=${AUDIO_CAPTURE_ONLY_VALUE}
    NEOPICO_EXP_AUDIO_PROCESS_NO_OUTPUT=${AUDIO_PROCESS_NO_OUTPUT_VALUE}
    NEOPICO_EXP_AUDIO_ENCODE_NO_QUEUE=${AUDIO_ENCODE_NO_QUEUE_VALUE}
    NEOPICO_EXP_AUDIO_LIMIT_BACKGROUND_WORK=${AUDIO_LIMIT_BACKGROUND_WORK_VALUE}
    NEOPICO_EXP_AUDIO_LIMIT_OUTPUT_PACKETS=${AUDIO_LIMIT_OUTPUT_PACKETS_VALUE}
    NEOPICO_EXP_AUDIO_PUSH_STATIC_SILENCE=${AUDIO_PUSH_STATIC_SILENCE_VALUE}
    NEOPICO_EXP_AUDIO_STARTUP_REARM=${AUDIO_STARTUP_REARM_VALUE}
    NEOPICO_EXP_AUDIO_REARM_ON_VIDEO_REACQUIRE=${AUDIO_REARM_ON_VIDEO_REACQUIRE_VALUE}
    NEOPICO_EXP_AUDIO_FRAME_RESYNC=${AUDIO_FRAME_RESYNC_VALUE}
    NEOPICO_EXP_AUDIO_MANUAL_REARM=${AUDIO_MANUAL_REARM_VALUE}
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
