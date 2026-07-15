/**
 * NeoPico-HD - MVS Video Capture + HSTX Output
 *
 * Core 0: Audio capture + processing
 * Core 1: HSTX HDMI output (DMA IRQ handler consumes DI queue)
 */

#include "pico_hdmi/hstx_data_island_queue.h"
#if NEOPICO_USE_NONRT_HDMI
#include "pico_hdmi/video_output.h"
#else
#include "pico_hdmi/video_output_rt.h"
#endif

#include "pico/multicore.h"
#include "pico/stdlib.h"

#include "hardware/clocks.h"
#include "hardware/vreg.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "audio_subsystem.h"
#include "capture_pins.h"
#if NEOPICO_ENABLE_OSD
#include "experiments/menu_diag_experiment.h"
#include "osd/fast_osd.h"
#endif
#include "video/line_ring.h"
#include "video/video_config.h"
#include "video/video_pipeline.h"
#include "video_capture.h"
#if NEOPICO_SETTINGS_FLASH
#include "settings.h"
#endif

// Line ring buffer (shared between Core 0 and Core 1)
line_ring_t g_line_ring __attribute__((aligned(64)));

#ifndef NEOPICO_EXP_GENLOCK_STATIC
#define NEOPICO_EXP_GENLOCK_STATIC 0
#endif

#ifndef NEOPICO_EXP_GENLOCK_DYNAMIC
#define NEOPICO_EXP_GENLOCK_DYNAMIC 0
#endif

#ifndef NEOPICO_RESOLUTION_MENU
#define NEOPICO_RESOLUTION_MENU 0
#endif

#ifndef NEOPICO_RESOLUTION_MENU_720P
#define NEOPICO_RESOLUTION_MENU_720P 0
#endif

#if NEOPICO_EXP_GENLOCK_DYNAMIC && (NEOPICO_EXP_GENLOCK_STATIC || NEOPICO_EXP_VTOTAL_MATCH)
#error "GENLOCK_DYNAMIC is mutually exclusive with GENLOCK_STATIC and VTOTAL_MATCH"
#endif

#ifndef NEOPICO_GENLOCK_TARGET_FPS_X100
#define NEOPICO_GENLOCK_TARGET_FPS_X100 5920
#endif

#ifndef NEOPICO_EXP_VTOTAL_MATCH
#define NEOPICO_EXP_VTOTAL_MATCH 0
#endif

#ifndef NEOPICO_EXP_VTOTAL_LINES
#define NEOPICO_EXP_VTOTAL_LINES 532
#endif

#ifndef NEOPICO_VIDEO_240P
#define NEOPICO_VIDEO_240P 0
#endif

#ifndef NEOPICO_VIDEO_720P
#define NEOPICO_VIDEO_720P 0
#endif

#ifndef NEOPICO_VIDEO_DVI_ONLY
#define NEOPICO_VIDEO_DVI_ONLY 0
#endif

#if NEOPICO_VIDEO_720P && NEOPICO_VIDEO_240P
#error "NEOPICO_VIDEO_720P and NEOPICO_VIDEO_240P are mutually exclusive"
#endif

#define SYS_CLK_60HZ_KHZ 126000U
#define SYS_CLK_720P_KHZ 372000U
// 480p is line-doubled (31.5 kHz scanline IRQ): at 126 MHz that is only ~4000
// cyc/line and the per-line ISR occasionally underruns -> desync. Run 480p at
// 252 MHz (~8000 cyc/line, matching the stable 240p/720p budgets). The HSTX
// divider is doubled in video_mode_480_p so the pixel clock stays 25.2 MHz
// (picture identical); requires VREG 1.30V and copy_to_ram (252 MHz overclock).
#define SYS_CLK_480P_KHZ 252000U

static inline uint32_t compute_sysclk_khz_for_fps_x100(uint32_t fps_x100)
{
    // 126 MHz corresponds to 60.00 Hz output in current timing setup.
    return ((SYS_CLK_60HZ_KHZ * fps_x100) + 3000U) / 6000U;
}

static inline uint32_t get_current_pixel_clock_hz(void)
{
#if NEOPICO_USE_NONRT_HDMI
    // Compile-time path: hstx_clk_div=1, hstx_csr_clkdiv=5 across all modes.
    return clock_get_hz(clk_sys) / 5U;
#else
    const video_mode_t *mode = video_output_active_mode;
    return clock_get_hz(clk_sys) / ((uint32_t)mode->hstx_clk_div * (uint32_t)mode->hstx_csr_clkdiv);
#endif
}

#if NEOPICO_EXP_VTOTAL_MATCH && !NEOPICO_USE_NONRT_HDMI
static video_mode_t s_vtotal_match_mode;

static const video_mode_t *build_vtotal_match_mode(void)
{
    s_vtotal_match_mode = video_mode_480_p;

    const uint32_t base_lines = (uint32_t)s_vtotal_match_mode.v_front_porch +
                                (uint32_t)s_vtotal_match_mode.v_sync_width +
                                (uint32_t)s_vtotal_match_mode.v_active_lines;
    uint32_t target_total = (uint32_t)NEOPICO_EXP_VTOTAL_LINES;
    if (target_total <= base_lines) {
        target_total = base_lines + 1U;
    }

    s_vtotal_match_mode.v_total_lines = (uint16_t)target_total;
    s_vtotal_match_mode.v_back_porch = (uint16_t)(target_total - base_lines);
    return &s_vtotal_match_mode;
}
#endif

#if NEOPICO_RESOLUTION_MENU && !NEOPICO_USE_NONRT_HDMI
static const video_mode_t *video_output_mode_for_reboot_mode(video_pipeline_reboot_mode_t mode)
{
#if NEOPICO_RESOLUTION_MENU_720P
    if (mode == VIDEO_PIPELINE_REBOOT_MODE_720P) {
        return &video_mode_720_p;
    }
#else
    if (mode == VIDEO_PIPELINE_REBOOT_MODE_720P) {
        mode = VIDEO_PIPELINE_REBOOT_MODE_480P;
    }
#endif
    return (mode == VIDEO_PIPELINE_REBOOT_MODE_240P) ? &video_mode_240_p : &video_mode_480_p;
}
#endif

#if NEOPICO_EXP_PRECOMPOSED_HDMI
// Desync events recovered by the frame-pacing watchdog (shown on the
// selftest OSD as "RS"). A desynced HSTX command stream makes scanlines
// complete at bus speed; >12 vsyncs per 100 ms window (expected: 6) means
// the expander lost framing and a full restart is needed.
volatile uint32_t g_neopico_resync_count;

static void precomposed_desync_watchdog(void)
{
    static uint32_t window_ms;
    static uint32_t last_frame_count;
    const uint32_t now_ms = to_ms_since_boot(get_absolute_time());
    const uint32_t elapsed_ms = now_ms - window_ms;
    if (elapsed_ms >= 100U) {
        const uint32_t frames = video_frame_count - last_frame_count;
        last_frame_count = video_frame_count;
        window_ms = now_ms;
        // Compare RATES, not counts: this background task can legitimately
        // stall for hundreds of ms (audio SRC bursts), stretching the window
        // and accumulating normal frames. Fire only above ~120 fps
        // equivalent (expected: 60), i.e. frames/elapsed > 12/100.
        if (frames * 100U > elapsed_ms * 12U) {
            video_output_force_resync();
            g_neopico_resync_count++;
        }
    }
}
#endif

static void combined_background_task(void)
{
#if NEOPICO_EXP_PRECOMPOSED_HDMI
    // One-time precomposed header build (no-op afterwards); island patching
    // itself happens in the scanline ISR and cannot be starved from here.
    video_output_compose_service();
    precomposed_desync_watchdog();
#if NEOPICO_VIDEO_720P
    video_pipeline_precomp_background();
#endif
#endif
#if !NEOPICO_VIDEO_DVI_ONLY
    audio_subsystem_background_task();
#endif
#if NEOPICO_ENABLE_OSD
    menu_diag_experiment_tick_background();
#endif
}

// ============================================================================
// Main (Core 0)
// ============================================================================

int main(void)
{
    sleep_ms(1000);

#if NEOPICO_RESOLUTION_MENU && !NEOPICO_USE_NONRT_HDMI
    video_pipeline_reboot_mode_t reboot_boot_mode =
        (NEOPICO_VIDEO_240P != 0) ? VIDEO_PIPELINE_REBOOT_MODE_240P : VIDEO_PIPELINE_REBOOT_MODE_480P;
    const bool warm_reboot = video_pipeline_take_reboot_mode_boot_request(&reboot_boot_mode);
#if NEOPICO_OSD_RES_CONFIRM && NEOPICO_ENABLE_OSD
    // If this (warm) boot is a pending resolution change, arm the keep/revert
    // countdown. reboot_boot_mode already holds the new mode from the scratch.
    {
        video_pipeline_reboot_mode_t res_confirm_previous;
        if (video_pipeline_take_pending_confirmation(&res_confirm_previous)) {
            menu_diag_experiment_arm_res_confirm(reboot_boot_mode, res_confirm_previous);
        }
    }
#endif
#if NEOPICO_SETTINGS_FLASH
    neopico_settings_t persisted;
    settings_load(&persisted);
#if NEOPICO_AUDIO_MODE == NEOPICO_AUDIO_MODE_SELECTABLE
    // Audio-source changes reboot through the existing warm-reboot path, so
    // unlike resolution the persisted source must be loaded on every boot.
    if (persisted.audio_source_valid == NEOPICO_SETTINGS_AUDIO_SOURCE_VALID &&
        audio_source_is_valid(persisted.audio_source)) {
        audio_subsystem_set_source((audio_source_t)persisted.audio_source);
    }
#endif
#if NEOPICO_MVS_COLOR_MODEL_MENU
    // Load the persisted model before capture starts. The capture module builds
    // both LUTs and publishes this initial selection before its first frame.
    if (persisted.color_model_valid == NEOPICO_SETTINGS_COLOR_MODEL_VALID &&
        mvs_color_model_is_valid(persisted.color_model)) {
        video_capture_set_color_model((mvs_color_model_t)persisted.color_model);
    }
#endif
    // Cold boot (power-on): the warm-reboot scratch is gone, so recover the
    // last-selected resolution from flash. A warm reboot already carries the
    // chosen mode in the scratch, so only apply flash resolution on cold boot.
    if (!warm_reboot) {
        if (persisted.resolution <= (uint8_t)VIDEO_PIPELINE_REBOOT_MODE_720P) {
            reboot_boot_mode = (video_pipeline_reboot_mode_t)persisted.resolution;
        }
    }
#endif
#if NEOPICO_FIRST_BOOT_REBOOT
    // Quick-and-dirty cold-boot scratchy-audio workaround: on a cold (power-on)
    // boot, immediately reboot once into the default mode -- the same path the
    // OSD resolution-select uses. Replicates the manual reset that clears the
    // cold-boot scratchiness (MVS audio DAC settle + the TV gets a warm HDMI
    // re-lock so its audio decoder doesn't latch Data Islands before TMDS lock).
    // The watchdog-scratch magic makes this fire exactly once (the warm boot
    // sees warm_reboot==true and proceeds normally).
    if (!warm_reboot) {
        video_pipeline_request_reboot_mode(reboot_boot_mode); // sets scratch + arms watchdog
        while (true) {
            tight_loop_contents(); // wait for the watchdog reboot; run no init
        }
    }
#endif
#endif

    // Set system clock before starting video pipeline.
#if NEOPICO_VIDEO_720P
    // 720p60 needs ~74.25 MHz pixel clock; closest on 12 MHz XOSC is 372 MHz sysclk.
    // 1.30V VREG is required for stable operation above 150 MHz.
    vreg_set_voltage(VREG_VOLTAGE_1_30);
    sleep_ms(10);
    set_sys_clock_khz(SYS_CLK_720P_KHZ, true);
#else
#if NEOPICO_RESOLUTION_MENU && !NEOPICO_USE_NONRT_HDMI && NEOPICO_RESOLUTION_MENU_720P
    if (reboot_boot_mode == VIDEO_PIPELINE_REBOOT_MODE_720P) {
        vreg_set_voltage(VREG_VOLTAGE_1_30);
        sleep_ms(10);
        set_sys_clock_khz(SYS_CLK_720P_KHZ, true);
    } else
#endif
    {
        uint32_t sys_clk_khz = SYS_CLK_60HZ_KHZ;
        bool overclock_480p = false;
#if NEOPICO_RESOLUTION_MENU && !NEOPICO_USE_NONRT_HDMI
        // Selector: the 480p reboot mode runs at 252 MHz for scanline-IRQ
        // headroom (240p/720p reboot modes keep their own clocks).
        overclock_480p = (reboot_boot_mode == VIDEO_PIPELINE_REBOOT_MODE_480P);
#elif !NEOPICO_VIDEO_240P && !NEOPICO_USE_NONRT_HDMI
        // Compile-time RT 480p: same headroom fix (non-RT keeps its own dividers).
        overclock_480p = true;
#endif
        if (overclock_480p) {
            sys_clk_khz = SYS_CLK_480P_KHZ;
            vreg_set_voltage(VREG_VOLTAGE_1_30);
            sleep_ms(10);
        }
#if NEOPICO_EXP_GENLOCK_STATIC && !NEOPICO_EXP_VTOTAL_MATCH
        sys_clk_khz = compute_sysclk_khz_for_fps_x100((uint32_t)NEOPICO_GENLOCK_TARGET_FPS_X100);
#endif
        set_sys_clock_khz(sys_clk_khz, true);
    }
#endif

    stdio_init_all();

#if NEOPICO_ENABLE_OSD
    // Initialize OSD button (active low with internal pull-up)
    gpio_init(PIN_OSD_BTN_MENU);
    gpio_set_dir(PIN_OSD_BTN_MENU, GPIO_IN);
    gpio_pull_up(PIN_OSD_BTN_MENU);

    gpio_init(PIN_OSD_BTN_BACK);
    gpio_set_dir(PIN_OSD_BTN_BACK, GPIO_IN);
    gpio_pull_up(PIN_OSD_BTN_BACK);

#if NEOPICO_OSD_CONTROLLER_INPUTS
    // Controller taps are active low. Weak pull-ups keep untapped default MVS
    // builds idle and sit in parallel with the external pulls when wired.
    gpio_init(NEOPICO_OSD_CONTROLLER_MENU_PIN);
    gpio_set_dir(NEOPICO_OSD_CONTROLLER_MENU_PIN, GPIO_IN);
    gpio_pull_up(NEOPICO_OSD_CONTROLLER_MENU_PIN);

    gpio_init(NEOPICO_OSD_CONTROLLER_BACK_PIN);
    gpio_set_dir(NEOPICO_OSD_CONTROLLER_BACK_PIN, GPIO_IN);
    gpio_pull_up(NEOPICO_OSD_CONTROLLER_BACK_PIN);

    gpio_init(NEOPICO_OSD_CONTROLLER_UP_PIN);
    gpio_set_dir(NEOPICO_OSD_CONTROLLER_UP_PIN, GPIO_IN);
    gpio_pull_up(NEOPICO_OSD_CONTROLLER_UP_PIN);

    gpio_init(NEOPICO_OSD_CONTROLLER_DOWN_PIN);
    gpio_set_dir(NEOPICO_OSD_CONTROLLER_DOWN_PIN, GPIO_IN);
    gpio_pull_up(NEOPICO_OSD_CONTROLLER_DOWN_PIN);
#endif
#endif

    sleep_ms(500);
    stdio_flush();

    // Initialize line ring buffer
    memset(&g_line_ring, 0, sizeof(g_line_ring));

#if NEOPICO_ENABLE_OSD
    // Initialize OSD (before video pipeline so framebuffer is ready)
    fast_osd_init();
    menu_diag_experiment_init();
#endif

    // Initialize HDMI output pipeline
    hstx_di_queue_init();
#if NEOPICO_VIDEO_DVI_ONLY
    video_output_set_dvi_mode(true);
#endif
#if !NEOPICO_USE_NONRT_HDMI
#if NEOPICO_VIDEO_720P
    video_output_set_mode(&video_mode_720_p);
#elif NEOPICO_RESOLUTION_MENU
    if (reboot_boot_mode != VIDEO_PIPELINE_REBOOT_MODE_480P) {
        video_output_set_mode(video_output_mode_for_reboot_mode(reboot_boot_mode));
    }
#elif NEOPICO_VIDEO_240P
    video_output_set_mode(&video_mode_240_p);
#elif NEOPICO_EXP_VTOTAL_MATCH
    video_output_set_mode(build_vtotal_match_mode());
#endif
#endif
    video_pipeline_init(FRAME_WIDTH, FRAME_HEIGHT);
#if NEOPICO_EXP_GENLOCK_STATIC && !NEOPICO_EXP_VTOTAL_MATCH && !NEOPICO_USE_NONRT_HDMI
    // Sysclk is already set before video init; only ACR needs custom CTS.
    // The non-rt path doesn't expose video_output_update_acr; skip it.
    video_output_update_acr(get_current_pixel_clock_hz());
#endif
#if !NEOPICO_VIDEO_DVI_ONLY || NEOPICO_ENABLE_OSD
    video_output_set_background_task(combined_background_task);
#endif

    // Initialize video capture
    video_capture_init(SOURCE_HEIGHT);
    sleep_ms(200);
    stdio_flush();

    // Launch Core 1 for HSTX output
    multicore_launch_core1(video_output_core1_run);
    sleep_ms(100);

    // Core 0: video capture loop (never returns)
    video_capture_run();
}
