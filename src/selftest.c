/**
 * NeoPico-HD Standalone Self-Test Firmware
 *
 * Minimal HDMI output + OSD selftest. No MVS capture.
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
#include "hardware/gpio.h"
#include "hardware/structs/sio.h"

#include <string.h>

#include "osd/fast_osd.h"
#include "osd/selftest_layout.h"

// ---------------------------------------------------------------------------
// Video constants
// ---------------------------------------------------------------------------
#define FRAME_W 320
#define FRAME_H 240
#define H_WORDS (640 / 2)

#define BG_COLOR 0x18E3 // dark gray

#define OSD_X_WORDS ((uint32_t)OSD_BOX_X)
#define OSD_W_WORDS ((uint32_t)OSD_BOX_W)

// ---------------------------------------------------------------------------
// Scanline callback — Core 1 DMA ISR
// ---------------------------------------------------------------------------

static void __scratch_x("") fill_rgb565(uint32_t *dst, uint32_t words, uint16_t color)
{
    const uint32_t packed = ((uint32_t)color << 16) | color;
    for (uint32_t i = 0; i < words; i++)
        dst[i] = packed;
}

static void __scratch_y("") double_pixels(uint32_t *restrict dst, const uint16_t *restrict src, int count)
{
    const uint32_t *s32 = (const uint32_t *)src;
    int pairs = count >> 1;
    for (int i = 0; i < pairs; i++) {
        uint32_t pair = s32[i];
        uint32_t p0 = pair & 0xFFFF;
        uint32_t p1 = pair >> 16;
        dst[i * 2] = p0 | (p0 << 16);
        dst[(i * 2) + 1] = p1 | (p1 << 16);
    }
}

static void __scratch_x("") scanline_cb(uint32_t v_scanline, uint32_t active_line, uint32_t *dst)
{
    (void)v_scanline;
    const uint32_t fb_line = active_line >> 1;
    const uint32_t osd_line = fb_line - OSD_BOX_Y;

    if (osd_line >= OSD_BOX_H) {
        fill_rgb565(dst, H_WORDS, BG_COLOR);
        return;
    }

    fill_rgb565(dst, OSD_X_WORDS, BG_COLOR);
    double_pixels(dst + OSD_X_WORDS, osd_framebuffer[osd_line], OSD_BOX_W);
    fill_rgb565(dst + OSD_X_WORDS + OSD_W_WORDS, H_WORDS - OSD_X_WORDS - OSD_W_WORDS, BG_COLOR);
}

static void vsync_cb(void)
{
}

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------

int main(void)
{
    sleep_ms(2000);
    set_sys_clock_khz(126000, true);
    stdio_init_all();
    sleep_ms(1000);
    stdio_flush();

    // Draw selftest layout into OSD framebuffer
    fast_osd_init();
    selftest_layout_reset();

    // Init HDMI
    hstx_di_queue_init();
    video_output_init(FRAME_W, FRAME_H);
    video_output_set_scanline_callback(scanline_cb);
    video_output_set_vsync_callback(vsync_cb);

    sleep_ms(200);
    stdio_flush();

    // Launch Core 1
    multicore_launch_core1(video_output_core1_run);

    // Init GPIO sampling for MVS + audio pins (GPIOs 22-45)
    for (uint gpio = 22; gpio <= 45; gpio++) {
        gpio_init(gpio);
        gpio_set_dir(gpio, GPIO_IN);
        gpio_disable_pulls(gpio);
    }

    // Core 0: poll GPIOs and update selftest at ~1 Hz
    uint32_t accum_hi = 0;
    uint32_t accum_lo = 0;
    absolute_time_t next_update = make_timeout_time_ms(1000);

    while (true) {
        // Read GPIOs 22-31 from bank 0, GPIOs 32-45 from bank 1
        uint32_t lo = (gpio_get_all() >> 22) & 0x3FF;
        uint32_t hi = sio_hw->gpio_hi_in & 0x3FFF;
        uint32_t s = lo | (hi << 10);
        accum_hi |= s;
        accum_lo |= ~s;

        if (absolute_time_diff_us(get_absolute_time(), next_update) <= 0) {
            uint32_t toggled = accum_hi & accum_lo & 0xFFFFFF;
            selftest_layout_update(video_frame_count, true, toggled);
            accum_hi = 0;
            accum_lo = 0;
            next_update = make_timeout_time_ms(1000);
        }
    }
}
