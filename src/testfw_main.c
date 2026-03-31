#include "pico_hdmi/video_output_rt.h"

#include "pico/multicore.h"
#include "pico/stdlib.h"

#include "hardware/clocks.h"

#include <stdint.h>

#include "experiments/lock_telemetry_experiment.h"
#include "mvs_pins.h"
#include "osd/fast_osd.h"
#include "pico.h"
#include "testfw/sync_probe.h"

#ifndef NEOPICO_VIDEO_240P
#define NEOPICO_VIDEO_240P 0
#endif

#define TESTFW_SYS_CLK_60HZ_KHZ 126000U

// Gray background (RGB565 mid-gray)
#define TESTFW_BG_RGB565 0x7BEF

static void __scratch_y("") testfw_fill_rgb565(uint32_t *dst, uint32_t words, uint16_t color)
{
    const uint32_t packed = ((uint32_t)color << 16) | color;
    for (uint32_t i = 0; i < words; i++) {
        dst[i] = packed;
    }
}

static void __scratch_y("") testfw_double_pixels_fast(uint32_t *restrict dst, const uint16_t *restrict src, int count)
{
    const uint32_t *src32 = (const uint32_t *)src;
    uint32_t *d = dst;
    int pairs = count >> 1;
    for (int i = 0; i < pairs; i++) {
        uint32_t pair = src32[i];
        uint32_t p0 = pair & 0xFFFF;
        uint32_t p1 = pair >> 16;
        d[0] = p0 | (p0 << 16);
        d[1] = p1 | (p1 << 16);
        d += 2;
    }
}

static void __scratch_y("") testfw_quadruple_pixels_fast(uint32_t *dst, const uint16_t *src, int count)
{
    const uint32_t *src32 = (const uint32_t *)src;
    int pairs = count / 2;
    for (int i = 0; i < pairs; i++) {
        uint32_t two = src32[i];
        uint32_t p0 = two & 0xFFFF;
        uint32_t p1 = two >> 16;
        uint32_t d0 = p0 | (p0 << 16);
        uint32_t d1 = p1 | (p1 << 16);
        dst[i * 4] = d0;
        dst[(i * 4) + 1] = d0;
        dst[(i * 4) + 2] = d1;
        dst[(i * 4) + 3] = d1;
    }
}

static void __scratch_x("") testfw_scanline_callback(uint32_t v_scanline, uint32_t active_line, uint32_t *dst)
{
    (void)v_scanline;

    const uint16_t h_active = video_output_get_h_active_pixels();
    const uint16_t v_active = video_output_get_v_active_lines();
    const bool mode_is_240p = (h_active == 1280U && v_active == 240U);
    const uint32_t h_words = (uint32_t)h_active / 2U;
    const uint32_t fb_line = mode_is_240p ? active_line : (active_line >> 1);
    const uint32_t osd_x_words = mode_is_240p ? ((uint32_t)OSD_BOX_X * 2U) : (uint32_t)OSD_BOX_X;
    const uint32_t osd_w_words = mode_is_240p ? ((uint32_t)OSD_BOX_W * 2U) : (uint32_t)OSD_BOX_W;

    const uint32_t osd_line_u32 = fb_line - OSD_BOX_Y;
    const bool osd_line_active = osd_visible && (osd_line_u32 < OSD_BOX_H);

    if (!osd_line_active) {
        testfw_fill_rgb565(dst, h_words, TESTFW_BG_RGB565);
        return;
    }

    const uint16_t *osd_src = osd_framebuffer[osd_line_u32];
    testfw_fill_rgb565(dst, osd_x_words, TESTFW_BG_RGB565);
    if (mode_is_240p) {
        testfw_quadruple_pixels_fast(dst + osd_x_words, osd_src, OSD_BOX_W);
    } else {
        testfw_double_pixels_fast(dst + osd_x_words, osd_src, OSD_BOX_W);
    }
    testfw_fill_rgb565(dst + osd_x_words + osd_w_words, h_words - osd_x_words - osd_w_words, TESTFW_BG_RGB565);
}

static void telemetry_background_task(void)
{
    // Keep test firmware minimal: OSD telemetry only.
    lock_telemetry_experiment_tick_background();
}

int main(void)
{
    sleep_ms(1000);
    // Match main firmware baseline timing so output FPS starts near 60 Hz.
    set_sys_clock_khz(TESTFW_SYS_CLK_60HZ_KHZ, true);
    stdio_init_all();
    sleep_ms(300);

    fast_osd_init();
    sync_probe_init(PIN_MVS_CSYNC);
    lock_telemetry_experiment_init();

#if NEOPICO_VIDEO_240P
    video_output_set_mode(&video_mode_240_p);
#endif
    video_output_init(320, 240);
    video_output_set_scanline_callback(testfw_scanline_callback);
    video_output_set_background_task(telemetry_background_task);

    multicore_launch_core1(video_output_core1_run);
    sleep_ms(50);

    while (true) {
        tight_loop_contents();
    }
}
