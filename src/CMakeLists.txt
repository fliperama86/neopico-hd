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

# =============================================================================
# Standalone PCM1802 to USB Capture Diagnostic (no video or HDMI)
# =============================================================================
option(NEOPICO_BUILD_PCM1802_USB_CAPTURE "Build standalone framed PCM1802 USB capture firmware" OFF)
set(NEOPICO_PCM1802_USB_BITS_PER_SAMPLE "24" CACHE STRING
    "Standalone PCM1802 USB payload width: 24 (full samples) or 16 (reduced-bandwidth diagnostic)")
set_property(CACHE NEOPICO_PCM1802_USB_BITS_PER_SAMPLE PROPERTY STRINGS 24 16)

if(NEOPICO_BUILD_PCM1802_USB_CAPTURE)
    if(NOT NEOPICO_PCM1802_USB_BITS_PER_SAMPLE STREQUAL "24" AND
       NOT NEOPICO_PCM1802_USB_BITS_PER_SAMPLE STREQUAL "16")
        message(FATAL_ERROR "NEOPICO_PCM1802_USB_BITS_PER_SAMPLE must be 24 or 16")
    endif()

    add_executable(neopico_pcm1802_usb_capture
        diagnostics/pcm1802_usb_capture/main.c
        diagnostics/pcm1802_usb_capture/usb_descriptors.c
    )

    target_include_directories(neopico_pcm1802_usb_capture PRIVATE
        ${CMAKE_CURRENT_LIST_DIR}/diagnostics/pcm1802_usb_capture
    )

    target_compile_options(neopico_pcm1802_usb_capture PRIVATE
        -Wall
        -Wextra
        -Werror
    )

    target_compile_definitions(neopico_pcm1802_usb_capture PRIVATE
        NEOPICO_PCM1802_USB_BITS_PER_SAMPLE=${NEOPICO_PCM1802_USB_BITS_PER_SAMPLE}
    )

    pico_generate_pio_header(neopico_pcm1802_usb_capture
        ${CMAKE_CURRENT_LIST_DIR}/audio/i2s_capture.pio
    )

    target_link_libraries(neopico_pcm1802_usb_capture
        pico_stdlib
        pico_unique_id
        tinyusb_device
        tinyusb_board
        hardware_pio
        hardware_dma
        hardware_irq
        hardware_gpio
    )

    pico_enable_stdio_usb(neopico_pcm1802_usb_capture 0)
    pico_enable_stdio_uart(neopico_pcm1802_usb_capture 0)
    pico_add_extra_outputs(neopico_pcm1802_usb_capture)
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
    settings.c
)

# Product configuration and active experiments
option(NEOPICO_VIDEO_DVI_ONLY "Disable HDMI Data Islands/audio output for video isolation tests" OFF)
option(NEOPICO_VIDEO_TEST_PATTERN "Render a static Core 1 video test pattern instead of captured video" OFF)
option(NEOPICO_DIAG_COUNTERS "Capture-health counters dumped over USB-serial (720p glitch diagnosis)" OFF)
option(NEOPICO_EXP_GENLOCK_DYNAMIC
    "Genlock the HDMI output to the capture source (dynamic VTOTAL acquire + sub-line blanking-trim servo). Output refresh follows the source (~59.19 Hz for MVS), a nonstandard rate some sinks may reject" ON)
# Compiling the genlock machinery in does NOT turn genlock on: the actual
# on/off state is a flash-persisted runtime setting (default OFF), applied at
# boot from the OSD "Genlock" toggle, exactly like the resolution selector.
# Shipping this flag ON just makes the toggle available; a fresh unit still
# boots on standard 60.000/60.114/59.979 Hz timing until the user opts in.
# Hardware-validated 2026-08-01 (operator directive to ship as the default).
# NOTE: OSD overlay/menu (incl. the root menu, self-test screen, translucent
# panel, and controller-tap inputs), the reboot-based resolution selector
# (incl. its 720p mode and post-switch confirmation), flash-backed settings
# persistence, the first-boot reboot workaround, and the frame-retention ring
# guard are permanently ON -- this is the only shipped path, so their cache
# options have been removed (see git history for the prior toggles). The
# compile-time fixed 240p/720p modes, the compile-time (non-RT) pico_hdmi
# backend, and the precomposed-scanout experiment (which required the non-RT
# backend) have been deleted outright: the runtime resolution selector is now
# the ONLY output-mode path, and copy-to-RAM execution is unconditional (every
# remaining configuration is overclocked; see below). The controller-tap GPIO
# assignments and debounce time are board/tuning constants, not build
# variants: they are plain #defines in src/capture_pins.h now, not cache
# options.
# DARK/SHADOW conversion is ON by default as of v0.11.0. Ignoring the two
# effect signals renders every dark-bit shade identical to its light
# counterpart and renders SHADOW scenes at full brightness, so enabling it is
# strictly more accurate than leaving it off. Turning it OFF restores the live
# Colors selector, which cannot coexist with it (see below).
option(NEOPICO_ENABLE_DARK_SHADOW "Four-state DARK/SHADOW conversion in MVS capture" ON)
set(NEOPICO_MVS_EFFECT_MODEL "MISTER" CACHE STRING
    "MVS DARK/SHADOW color model when enabled: MISTER (digital reference), MAME (analog resistor model), or DIGIAV (cps2_digiav three-state: SHADOW forces DARK)")
set_property(CACHE NEOPICO_MVS_EFFECT_MODEL PROPERTY STRINGS MISTER MAME DIGIAV)
# NEOPICO_MVS_DIGITAL_EFFECT_PROCESSING is no longer an independent cache
# option: register processing is used automatically whenever DARK/SHADOW is on
# and the selected model has a closed form (MISTER or DIGIAV). It must not be
# a user toggle, because the alternative split-LUT backend produces
# bottom-screen pixel jitter on hardware while the register path soaked clean.
# MAME has no closed form (it is a measured resistor table) and therefore
# falls back to the split LUT, which is why it is not a shippable model.
# NEOPICO_MVS_COLOR_MODEL_MENU is no longer an independent cache option: the
# live Colors selector is on whenever DARK/SHADOW is off (MVS only). This
# preserves the original mutual exclusion with NEOPICO_ENABLE_DARK_SHADOW
# without a separate toggle a user could set inconsistently.
# The static genlock experiment, the fixed-vtotal frame-rate-match
# experiment, the hand-written M33 asm pixel-scale kernels (scale_pixels.S,
# never hardware-validated against the C path), and the PC-monitor resolution
# modes have all been deleted (dead experiments; see git history). The
# dynamic genlock was deleted with them but is resurrected above as
# NEOPICO_EXP_GENLOCK_DYNAMIC: the free-running ~60 Hz output beats against
# the 59.186 Hz MVS and repeats one frame every ~1.2 s (visible as a scroll
# hiccup); phase-locking the output to the source removes it. See
# docs/GENLOCK.md.
# NEOPICO_AUDIO_MODE is no longer an independent cache option: MVS is always
# SELECTABLE (OSD audio-source picker) and SNES is always DIGITAL.
option(NEOPICO_DIAG_AUDIO_OSD "Show HDMI audio underrun (silence splice) counter on the selftest OSD screen" OFF)
# Full-precision Core 1 scanout: one 32-bit RGB888 word per output pixel
# (instead of two packed RGB565 pixels per word) at 240p/480p/720p, fixing
# the RGB565 5/6-bit quantization that hid the DARK half-step in red/blue and
# the RP2350 EXPAND_TMDS zero-fill dimming (pico-feedback#454). Plain
# bit-replication for the fill/no-signal paths; the live-content path stores
# raw entropy in the line ring and applies the colour model on Core 1 (see
# mvs_effect_lut888). Hardware-validated end-to-end at 240p/480p/720p
# 2026-08-01; MVS only (see the capture-target guard below) -- SNES capture
# writes converted RGB565 straight into the ring, not entropy, so this is
# meaningless there. Selects the matching pico_hdmi-side
# PICO_HDMI_PIXEL_FORMAT_RGB888 option.
option(NEOPICO_EXP_RGB888_SCANOUT
    "Full-precision 32-bit RGB888 Core 1 scanout (MVS only; fixes RGB565 DARK/SHADOW quantization and HSTX dimming)" ON)
# Pipelines the 720p (3x) active-line scanout in pico_hdmi across two
# buffers, so the per-line scanline callback -- which measured 85-94% of its
# single-line budget at 720p RGB888 -- gets a full 3-line-period budget
# instead of 1. Required for 720p RGB888 (no clock headroom is available
# there: pixel clock = sysclk/5 pins sysclk at 320 MHz); 240p/480p RGB888
# don't need it. Selects the matching pico_hdmi-side
# PICO_HDMI_ACTIVE_LINE_DOUBLE_BUFFER option. Hardware-validated alongside
# NEOPICO_EXP_RGB888_SCANOUT above 2026-08-01; harmless (no-op) when that is
# off, so it is not target-gated separately.
option(NEOPICO_EXP_RGB888_DOUBLE_BUFFER
    "Pipeline the 720p active-line scanout across two buffers in pico_hdmi (required for 720p RGB888; requires NEOPICO_EXP_RGB888_SCANOUT)" ON)
# Diagnostic for the unexplained 240p/720p RGB888 instability. Records per
# scanline callback cycle counts into a RAM ring on Core 1 (two PPB reads and
# one store, ~0.1% of a 480p line) and dumps them as raw binary from Core 0 on
# host request. No formatting on Core 1: snprintf there is a documented cause
# of HSTX FIFO underruns in this firmware.
option(NEOPICO_EXP_SCANLINE_TRACE
    "EXPERIMENTAL: record per-scanline callback cycle counts for USB dump" OFF)
# The line-ring size is a fixed board/tuning constant now (plain #define in
# src/video/line_ring.h), not a cache option.

string(TOUPPER "${NEOPICO_MVS_EFFECT_MODEL}" NEOPICO_MVS_EFFECT_MODEL_UPPER)
if(NEOPICO_MVS_EFFECT_MODEL_UPPER STREQUAL "MISTER")
    set(MVS_EFFECT_MODEL_VALUE 1)
elseif(NEOPICO_MVS_EFFECT_MODEL_UPPER STREQUAL "MAME")
    set(MVS_EFFECT_MODEL_VALUE 2)
elseif(NEOPICO_MVS_EFFECT_MODEL_UPPER STREQUAL "DIGIAV")
    set(MVS_EFFECT_MODEL_VALUE 3)
else()
    message(FATAL_ERROR "NEOPICO_MVS_EFFECT_MODEL must be MISTER, MAME, or DIGIAV")
endif()

# MVS is always the SELECTABLE audio source (OSD picker between the digital
# NEO-YSA2 tap and an external PCM1802 ADC); SNES has no PCM1802 tap and is
# always DIGITAL. This is a per-capture-target constant, not a build choice.
if(NEOPICO_CAPTURE_TARGET_UPPER STREQUAL "MVS")
    set(AUDIO_MODE_VALUE 2) # NEOPICO_AUDIO_MODE_SELECTABLE
else()
    set(AUDIO_MODE_VALUE 0) # NEOPICO_AUDIO_MODE_DIGITAL
endif()

# The DARK and SHADOW signals exist only on the MVS capture harness, so the
# feature switches itself off for other targets rather than failing the build.
if(NEOPICO_ENABLE_DARK_SHADOW AND NOT NEOPICO_CAPTURE_TARGET_UPPER STREQUAL "MVS")
    message(STATUS "DARK/SHADOW is MVS-only: disabling it for capture target ${NEOPICO_CAPTURE_TARGET}")
    set(NEOPICO_ENABLE_DARK_SHADOW OFF)
endif()

# RGB888 scanout's live-content path expects the line ring to carry raw
# entropy ([DARK][raw RGB555], see mvs_effect_lut888) so the colour model can
# be applied on Core 1 with full 8-bit precision. That is an MVS capture
# convention: SNES (src/video/video_capture_snes.c) writes already-converted
# RGB565 straight into the ring. Feeding those RGB565 words through the
# entropy LUT as if they were [DARK][RGB555] is nonsensical (undefined
# colours, not merely unvalidated), so -- same precedent as DARK/SHADOW
# above -- the feature switches itself off for other targets rather than
# failing the build or shipping wrong colours silently.
if(NEOPICO_EXP_RGB888_SCANOUT AND NOT NEOPICO_CAPTURE_TARGET_UPPER STREQUAL "MVS")
    message(STATUS "RGB888 scanout is MVS-only (entropy ring convention): disabling it for capture target ${NEOPICO_CAPTURE_TARGET}")
    set(NEOPICO_EXP_RGB888_SCANOUT OFF)
endif()

if(NEOPICO_ENABLE_DARK_SHADOW)
    set(ENABLE_DARK_SHADOW_VALUE 1)
else()
    set(ENABLE_DARK_SHADOW_VALUE 0)
endif()

# Derived, not user-selectable: use register processing whenever the selected
# model has a closed form. MAME does not, so it falls back to the split LUT.
if(NEOPICO_ENABLE_DARK_SHADOW
   AND (NEOPICO_MVS_EFFECT_MODEL_UPPER STREQUAL "MISTER"
        OR NEOPICO_MVS_EFFECT_MODEL_UPPER STREQUAL "DIGIAV"))
    set(MVS_DIGITAL_EFFECT_PROCESSING_VALUE 1)
else()
    set(MVS_DIGITAL_EFFECT_PROCESSING_VALUE 0)
endif()

# The live persistent MVS Digital/Analog normal-color selector is on whenever
# DARK/SHADOW is off (MVS only): the two features are mutually exclusive (both
# repurpose the same capture LUT machinery), so this is derived rather than an
# independently settable option a user could misconfigure.
if(NEOPICO_CAPTURE_TARGET_UPPER STREQUAL "MVS" AND NOT NEOPICO_ENABLE_DARK_SHADOW)
    set(MVS_COLOR_MODEL_MENU_VALUE 1)
else()
    set(MVS_COLOR_MODEL_MENU_VALUE 0)
endif()

if(NEOPICO_DIAG_AUDIO_OSD)
    set(DIAG_AUDIO_OSD_VALUE 1)
else()
    set(DIAG_AUDIO_OSD_VALUE 0)
endif()

# Overclock builds MUST execute from RAM. Above ~150 MHz sys_clk, XIP flash
# reads corrupt (the boot-clock QMI timing samples too early) -> the next
# instruction fetch faults -> hard crash with no recovery. copy_to_ram reads
# flash once at the safe boot clock, then runs from RAM. The runtime
# resolution selector is the only output-mode path and its 480p mode alone
# already runs at 252 MHz (scanline-IRQ headroom) with 720p and the
# PC-monitor experiment going higher still, so every configuration is
# overclocked and copy-to-RAM execution is unconditional (no cache option;
# pico_set_binary_type() is called below, after the pico_hdmi library target
# exists).

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

if(NEOPICO_EXP_GENLOCK_DYNAMIC)
    set(GENLOCK_DYNAMIC_VALUE 1)
else()
    set(GENLOCK_DYNAMIC_VALUE 0)
endif()

if(NEOPICO_EXP_SCANLINE_TRACE)
    set(EXP_SCANLINE_TRACE_VALUE 1)
else()
    set(EXP_SCANLINE_TRACE_VALUE 0)
endif()

if(NEOPICO_EXP_RGB888_SCANOUT)
    set(EXP_RGB888_SCANOUT_VALUE 1)
else()
    set(EXP_RGB888_SCANOUT_VALUE 0)
endif()

# The runtime pico_hdmi path is the only backend now (the compile-time
# non-RT backend, its fixed 240p/720p modes, and the precomposed-scanout
# experiment that required it have all been deleted).
set(PICO_HDMI_RUNTIME_MODES ON CACHE BOOL "" FORCE)
set(PICO_HDMI_PRECOMPOSED_ACTIVE_LINES OFF CACHE BOOL "" FORCE)

# Feasibility spike: mirror NEOPICO_EXP_RGB888_SCANOUT into the pico_hdmi
# library's own option so the HSTX expand/DMA reconfiguration in
# video_output_rt.c matches the firmware-side scanout kernel selection.
# Default OFF on both sides.
set(PICO_HDMI_PIXEL_FORMAT_RGB888 ${NEOPICO_EXP_RGB888_SCANOUT} CACHE BOOL "" FORCE)

# Feasibility spike: mirror NEOPICO_EXP_RGB888_DOUBLE_BUFFER into the
# pico_hdmi library's own option. Independent of PICO_HDMI_PIXEL_FORMAT_RGB888
# above so the two can be toggled separately (validation needs both
# combinations); the double-buffer scheme itself only engages for the mode
# descriptor whose v_active_lines==720, decided at runtime in the library.
set(PICO_HDMI_ACTIVE_LINE_DOUBLE_BUFFER ${NEOPICO_EXP_RGB888_DOUBLE_BUFFER} CACHE BOOL "" FORCE)

# Add pico_hdmi library (MUST be after add_compile_definitions and option overrides)
add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/../lib/pico_hdmi ${CMAKE_CURRENT_BINARY_DIR}/pico_hdmi)

# The resolution selector's 720p mode is always available, so the runtime
# mode-attribute table is always needed.
target_compile_definitions(pico_hdmi PRIVATE PICO_HDMI_RT_RUNTIME_MODE_ATTRS=1)

# 240p uses a non-CEA VIC-0 timing. Keep the conservative AVI payload that was
# hardware-validated with the normal HDMI audio stream: PB1=0x00, PB2=0x08,
# and PR=0. The newer active-format/aspect/PR=3 payload causes black video on
# tested scaler paths. This define changes only the 240p descriptor; runtime
# exact-clock 720p independently uses VIC 0 with an explicit 16:9 aspect.
target_compile_definitions(pico_hdmi PRIVATE PICO_HDMI_LEGACY_240P_AVI_INFOFRAME=1)

# RT 480p runs at 252 MHz for scanline-IRQ headroom (see main.c SYS_CLK_480P_KHZ).
# Halve the 480p HSTX clk divider in the library so the pixel clock stays
# 25.2 MHz (252/(2*5)) and the 480p signal is byte-identical to the 126 MHz one.
target_compile_definitions(pico_hdmi PRIVATE PICO_HDMI_480P_HSTX_CLK_DIV=2)

# RT 240p also now runs at 252 MHz unconditionally (see main.c:
# overclock_480p is OR'd with the 240p reboot mode). Two independent
# consumers need this: (1) v0.11.0's default-on DARK/SHADOW conversion made
# Core 0's per-pixel work heavy enough that at the stock 126 MHz 240p sysclk,
# conversion lateness accumulates down the frame (hardware-confirmed
# bottom-half bounce/H-shift; v0.10.0 and NEOPICO_ENABLE_DARK_SHADOW=OFF are
# both clean at 126 MHz); (2) the 32-bit RGB888 scanout path (below) also
# needs the doubled per-line cycle budget at 240p. Halve the 240p HSTX clk
# divider the same way as 480p so the pixel clock stays 25.2 MHz and the
# signal is unchanged. Kept as ONE unconditional definition (not gated on
# NEOPICO_EXP_RGB888_SCANOUT) since the DARK/SHADOW hotfix alone already
# requires it in every build.
target_compile_definitions(pico_hdmi PRIVATE PICO_HDMI_240P_HSTX_CLK_DIV=2)

# Elastic-blanking htrim servo (video_output_set_vblank_htrim_px()) is only
# reachable/registered when genlock is built in. Default builds get none of
# this new code -- scratch_x is exactly full there and cannot take even one
# extra instruction.
# PUBLIC (not PRIVATE): the app (main.c) must see this flag too, to decide
# whether video_mode_240_p_genlock exists before referencing it -- the
# struct's declaration in video_output_rt.h is gated on this same macro, so
# the app and the library must always agree on its value. Values pico_hdmi
# uses internally are identical either way; PUBLIC only adds propagation to
# consumers linking against the target.
if(NEOPICO_EXP_GENLOCK_DYNAMIC AND NOT NEOPICO_DIAG_NO_LIB_HTRIM)
    target_compile_definitions(pico_hdmi PUBLIC PICO_HDMI_VBLANK_HTRIM=1)
endif()

pico_set_binary_type(neopico_hd copy_to_ram)

# The OSD root menu, the first-boot reboot workaround, flash-backed settings,
# and the post-switch resolution confirmation prompt are permanently on and no
# longer referenced by any source file as preprocessor conditionals, so they
# are no longer passed as compile definitions. The runtime pico_hdmi path is
# now the only backend, which is the combination these used to require.

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
    hardware_pll
    hardware_dma
    hardware_irq
    hardware_gpio
    hardware_flash
)

target_compile_definitions(neopico_hd PRIVATE
    NEOPICO_VERSION="${PROJECT_VERSION}"
    NEOPICO_CAPTURE_TARGET=${CAPTURE_TARGET_VALUE}
    NEOPICO_VIDEO_DVI_ONLY=${VIDEO_DVI_ONLY_VALUE}
    NEOPICO_VIDEO_TEST_PATTERN=${VIDEO_TEST_PATTERN_VALUE}
    NEOPICO_DIAG_COUNTERS=${DIAG_COUNTERS_VALUE}
    NEOPICO_EXP_GENLOCK_DYNAMIC=${GENLOCK_DYNAMIC_VALUE}
    ENABLE_DARK_SHADOW=${ENABLE_DARK_SHADOW_VALUE}
    MVS_EFFECT_MODEL=${MVS_EFFECT_MODEL_VALUE}
    NEOPICO_MVS_DIGITAL_EFFECT_PROCESSING=${MVS_DIGITAL_EFFECT_PROCESSING_VALUE}
    NEOPICO_MVS_COLOR_MODEL_MENU=${MVS_COLOR_MODEL_MENU_VALUE}
    NEOPICO_AUDIO_MODE=${AUDIO_MODE_VALUE}
    NEOPICO_DIAG_AUDIO_OSD=${DIAG_AUDIO_OSD_VALUE}
    NEOPICO_EXP_RGB888_SCANOUT=${EXP_RGB888_SCANOUT_VALUE}
    NEOPICO_EXP_SCANLINE_TRACE=${EXP_SCANLINE_TRACE_VALUE}
    # The SDK's binary info embeds __DATE__ by default, which broke CI
    # byte-reproducibility whenever the runner's UTC date differed from the
    # local build date (every prior byte-identity match was same-day luck).
    # Drop the field entirely; NEOPICO_VERSION carries the meaningful identity.
    PICO_NO_BI_PROGRAM_BUILD_DATE=1
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
