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

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "audio_subsystem.h"
#include "experiments/menu_diag_experiment.h"
#include "mvs_pins.h"
#include "osd/fast_osd.h"
#include "video/line_ring.h"
#include "video/video_config.h"
#include "video/video_pipeline.h"
#include "video_capture.h"

// Line ring buffer (shared between Core 0 and Core 1)
line_ring_t g_line_ring __attribute__((aligned(64)));

#ifndef NEOPICO_EXP_GENLOCK_STATIC
#define NEOPICO_EXP_GENLOCK_STATIC 0
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

#define SYS_CLK_60HZ_KHZ 126000U

static inline uint32_t compute_sysclk_khz_for_fps_x100(uint32_t fps_x100)
{
    // 126 MHz corresponds to 60.00 Hz output in current timing setup.
    return (SYS_CLK_60HZ_KHZ * fps_x100 + 3000U) / 6000U;
}

static inline uint32_t get_current_pixel_clock_hz(void)
{
    const video_mode_t *mode = video_output_active_mode;
    return clock_get_hz(clk_sys) / ((uint32_t)mode->hstx_clk_div * (uint32_t)mode->hstx_csr_clkdiv);
}

#if NEOPICO_EXP_VTOTAL_MATCH
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

static void combined_background_task(void)
{
    audio_subsystem_background_task();
    menu_diag_experiment_tick_background();
}

// ============================================================================
// Main (Core 0)
// ============================================================================

int main(void)
{
    sleep_ms(1000);
    // Set system clock before starting video pipeline.
    uint32_t sys_clk_khz = SYS_CLK_60HZ_KHZ;
#if NEOPICO_EXP_GENLOCK_STATIC && !NEOPICO_EXP_VTOTAL_MATCH
    sys_clk_khz = compute_sysclk_khz_for_fps_x100((uint32_t)NEOPICO_GENLOCK_TARGET_FPS_X100);
#endif
    set_sys_clock_khz(sys_clk_khz, true);

    stdio_init_all();

    // Initialize OSD button (active low with internal pull-up)
    gpio_init(PIN_OSD_BTN_MENU);
    gpio_set_dir(PIN_OSD_BTN_MENU, GPIO_IN);
    gpio_pull_up(PIN_OSD_BTN_MENU);

    gpio_init(PIN_OSD_BTN_BACK);
    gpio_set_dir(PIN_OSD_BTN_BACK, GPIO_IN);
    gpio_pull_up(PIN_OSD_BTN_BACK);

    sleep_ms(500);
    stdio_flush();

    // Initialize line ring buffer
    memset(&g_line_ring, 0, sizeof(g_line_ring));

    // Initialize OSD (before video pipeline so framebuffer is ready)
    fast_osd_init();
    menu_diag_experiment_init();

    // Initialize HDMI output pipeline
    hstx_di_queue_init();
#if NEOPICO_EXP_VTOTAL_MATCH
    video_output_set_mode(build_vtotal_match_mode());
#endif
    video_pipeline_init(FRAME_WIDTH, FRAME_HEIGHT);
#if NEOPICO_EXP_GENLOCK_STATIC && !NEOPICO_EXP_VTOTAL_MATCH
    // Sysclk is already set before video init; only ACR needs custom CTS.
    video_output_update_acr(get_current_pixel_clock_hz());
#endif
    video_output_set_background_task(combined_background_task);

    // Initialize video capture
    video_capture_init(MVS_HEIGHT);
    sleep_ms(200);
    stdio_flush();

    // Launch Core 1 for HSTX output
    multicore_launch_core1(video_output_core1_run);
    sleep_ms(100);

    // Core 0: video capture loop (never returns)
    video_capture_run();
}
