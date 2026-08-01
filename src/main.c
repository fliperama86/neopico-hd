/**
 * NeoPico-HD - MVS Video Capture + HSTX Output
 *
 * Core 0: Audio capture + processing
 * Core 1: HSTX HDMI output (DMA IRQ handler consumes DI queue)
 */

#include "pico_hdmi/hstx_data_island_queue.h"
#include "pico_hdmi/video_output_rt.h"

#include "pico/multicore.h"
#include "pico/stdlib.h"

#include "hardware/clocks.h"
#include "hardware/vreg.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "audio_subsystem.h"
#include "capture_pins.h"
#include "experiments/menu_diag_experiment.h"
#include "osd/fast_osd.h"
#include "settings.h"
#include "video/line_ring.h"
#include "video/video_config.h"
#include "video/video_pipeline.h"
#include "video_capture.h"

// Line ring buffer (shared between Core 0 and Core 1)
line_ring_t g_line_ring __attribute__((aligned(64)));

#ifndef NEOPICO_VIDEO_DVI_ONLY
#define NEOPICO_VIDEO_DVI_ONLY 0
#endif

#define SYS_CLK_60HZ_KHZ 126000U
#define SYS_CLK_720P_RUNTIME_KHZ 320000U
// 480p is line-doubled (31.5 kHz scanline IRQ): at 126 MHz that is only ~4000
// cyc/line and the per-line ISR occasionally underruns -> desync. Run 480p at
// 252 MHz (~8000 cyc/line, matching the stable 240p/720p budgets). The HSTX
// divider is doubled in video_mode_480_p so the pixel clock stays 25.2 MHz
// (picture identical); requires VREG 1.30V and copy_to_ram (252 MHz overclock).
#define SYS_CLK_480P_KHZ 252000U

#if NEOPICO_EXP_GENLOCK_DYNAMIC
// 480p/720p need no genlock-specific mode: their genlock is vtotal/htrim
// dithered at runtime around the same descriptor. 240p's genlock raster
// differs in h_total (1613 vs 1600, see video_mode_240_p_genlock), which is
// baked into command lists at mode apply, so it needs a distinct descriptor
// selected here instead of a runtime-only knob.
//
// This whole function is duplicated (rather than adding a parameter to the
// one below) so that a non-genlock build's compiled main.c is byte-for-byte
// identical to before this feature existed -- scratch_x is exactly full in
// default builds, and while this function itself isn't scratch-resident,
// byte-identity here is still how the gates in this task were verified.
static const video_mode_t *video_output_mode_for_reboot_mode(video_pipeline_reboot_mode_t mode, bool genlock_enabled)
{
    if (mode == VIDEO_PIPELINE_REBOOT_MODE_720P) {
        return &video_mode_720_p;
    }
    if (mode != VIDEO_PIPELINE_REBOOT_MODE_240P) {
        return &video_mode_480_p;
    }
#if PICO_HDMI_VBLANK_HTRIM
    if (genlock_enabled) {
        return &video_mode_240_p_genlock;
    }
#else
    (void)genlock_enabled;
#endif
    return &video_mode_240_p;
}
#else
static const video_mode_t *video_output_mode_for_reboot_mode(video_pipeline_reboot_mode_t mode)
{
    if (mode == VIDEO_PIPELINE_REBOOT_MODE_720P) {
        return &video_mode_720_p;
    }
    return (mode == VIDEO_PIPELINE_REBOOT_MODE_240P) ? &video_mode_240_p : &video_mode_480_p;
}
#endif

static void combined_background_task(void)
{
#if !NEOPICO_VIDEO_DVI_ONLY
    audio_subsystem_background_task();
#endif
    menu_diag_experiment_tick_background();
}

// ============================================================================
// Main (Core 0)
// ============================================================================

int main(void)
{
    sleep_ms(1000);

    video_pipeline_reboot_mode_t reboot_boot_mode = VIDEO_PIPELINE_REBOOT_MODE_480P;
    const bool warm_reboot = video_pipeline_take_reboot_mode_boot_request(&reboot_boot_mode);
    // If this (warm) boot is a pending resolution change, arm the keep/revert
    // countdown. reboot_boot_mode already holds the new mode from the scratch.
    {
        video_pipeline_reboot_mode_t res_confirm_previous;
        if (video_pipeline_take_pending_confirmation(&res_confirm_previous)) {
            menu_diag_experiment_arm_res_confirm(reboot_boot_mode, res_confirm_previous);
        }
    }
    neopico_settings_t persisted;
    settings_load(&persisted);
#if NEOPICO_EXP_GENLOCK_DYNAMIC
    // Genlock is opt-in (default off) and, like resolution, applied only at
    // boot: the menu optimistically persists the new value to flash before
    // rebooting (see menu_diag_experiment.c), so the flash copy already
    // reflects the to-be-confirmed value by the time we read it here.
    const bool genlock_enabled = persisted.genlock_enabled != 0U;
    // Same safety net as resolution, for the genlock on/off setting: if this
    // (warm) boot is a pending genlock change, arm the keep/revert countdown.
    // genlock_enabled already holds the new (to-be-confirmed) value, applied
    // above from flash; the pending marker carries only the revert-to value.
    {
        bool genlock_confirm_previous;
        if (video_pipeline_take_genlock_pending_confirmation(&genlock_confirm_previous)) {
            menu_diag_experiment_arm_genlock_confirm(genlock_enabled, genlock_confirm_previous);
        }
    }
#endif
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
        if (video_pipeline_reboot_mode_available(persisted.resolution)) {
            reboot_boot_mode = (video_pipeline_reboot_mode_t)persisted.resolution;
        }
    }
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

    // Set system clock before starting video pipeline.
    if (reboot_boot_mode == VIDEO_PIPELINE_REBOOT_MODE_720P) {
        // Runtime 720p uses exact-clock CVT reduced blanking: 320 MHz / 5 = 64 MHz.
        vreg_set_voltage(VREG_VOLTAGE_1_20);
        sleep_ms(10);
        set_sys_clock_khz(SYS_CLK_720P_RUNTIME_KHZ, true);
    } else {
        uint32_t sys_clk_khz = SYS_CLK_60HZ_KHZ;
        // The 480p reboot mode runs at 252 MHz for scanline-IRQ headroom
        // (720p reboot mode keeps its own exact-clock path above).
        bool overclock_480p = (reboot_boot_mode == VIDEO_PIPELINE_REBOOT_MODE_480P);
        // 240p always gets the same 252 MHz treatment as 480p now, for two
        // independent reasons (both need PICO_HDMI_240P_HSTX_CLK_DIV=2,
        // src/CMakeLists.txt, to hold the pixel clock at 25.2 MHz):
        // (1) v0.11.0's DARK/SHADOW-by-default made Core 0's per-pixel
        // conversion heavier -- at stock 126 MHz sysclk, conversion lateness
        // accumulates down the frame (hardware-confirmed bottom-half
        // bounce/H-shift). (2) 32-bit RGB888 scanout roughly triples the
        // per-line conversion cost; measured, the 240p callback needed 5913
        // of 8000 cycles at 126 MHz (74%) versus 60% for 480p at 252 MHz,
        // which is what made 240p the mode that failed under RGB888.
        // Unconditional (not gated on NEOPICO_EXP_RGB888_SCANOUT): the
        // DARK/SHADOW hotfix alone already requires this in every build.
        overclock_480p = overclock_480p || (reboot_boot_mode == VIDEO_PIPELINE_REBOOT_MODE_240P);
        if (overclock_480p) {
            sys_clk_khz = SYS_CLK_480P_KHZ;
            vreg_set_voltage(VREG_VOLTAGE_1_30);
            sleep_ms(10);
        }
        set_sys_clock_khz(sys_clk_khz, true);
    }

    stdio_init_all();

    // Initialize OSD button (active low with internal pull-up)
    gpio_init(PIN_OSD_BTN_MENU);
    gpio_set_dir(PIN_OSD_BTN_MENU, GPIO_IN);
    gpio_pull_up(PIN_OSD_BTN_MENU);

    gpio_init(PIN_OSD_BTN_BACK);
    gpio_set_dir(PIN_OSD_BTN_BACK, GPIO_IN);
    gpio_pull_up(PIN_OSD_BTN_BACK);

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

    sleep_ms(500);
    stdio_flush();

    // Initialize line ring buffer
    memset(&g_line_ring, 0, sizeof(g_line_ring));

    // Initialize OSD (before video pipeline so framebuffer is ready)
    fast_osd_init();
    menu_diag_experiment_init();

    // Initialize HDMI output pipeline
    hstx_di_queue_init();
#if NEOPICO_VIDEO_DVI_ONLY
    video_output_set_dvi_mode(true);
#endif
    if (reboot_boot_mode != VIDEO_PIPELINE_REBOOT_MODE_480P) {
#if NEOPICO_EXP_GENLOCK_DYNAMIC
        video_output_set_mode(video_output_mode_for_reboot_mode(reboot_boot_mode, genlock_enabled));
#else
        video_output_set_mode(video_output_mode_for_reboot_mode(reboot_boot_mode));
#endif
    }
    video_pipeline_init(FRAME_WIDTH, FRAME_HEIGHT);
    video_output_set_background_task(combined_background_task);

    // Initialize video capture
    video_capture_init(SOURCE_HEIGHT);
    sleep_ms(200);
    stdio_flush();

#if NEOPICO_EXP_GENLOCK_DYNAMIC
    // Latch once, before Core 1 launch, so the vsync callback's gate on
    // Core 1 is a single load (see video_pipeline_vsync_callback()).
    video_pipeline_set_genlock_enabled(genlock_enabled);
#endif

    // Launch Core 1 for HSTX output
    multicore_launch_core1(video_output_core1_run);
    sleep_ms(100);

    // Core 0: video capture loop (never returns)
    video_capture_run();
}
