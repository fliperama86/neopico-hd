/**
 * NeoPico-HD - MVS Video Capture + HSTX Output
 *
 * Streaming architecture: line ring buffer between Core 0 (capture) and Core 1 (output)
 */

#include "audio_subsystem.h"
#include "hardware/clocks.h"
#include "mvs_pins.h"
#include "osd/osd.h"
#include "pico/multicore.h"
#include "pico/stdlib.h"
#include "pico_dvi2/hstx_data_island_queue.h"
#include "pico_dvi2/video_config.h"
#include "pico_dvi2/video_output.h"
#include "video/line_ring.h"
#include "video_capture.h"
#include <stdio.h>
#include <string.h>

// Line ring buffer (shared between Core 0 and Core 1)
line_ring_t g_line_ring __attribute__((aligned(64)));

// ============================================================================
// DVI Scanline Callback (Core 1)
// ============================================================================

// V_OFFSET is defined in video_config.h for vertical centering

/**
 * Fast 2x pixel doubling: reads 2 pixels, writes 2 doubled words
 * Processes 32-bits at a time for efficiency
 */
static inline void __scratch_x("")
double_pixels_fast(uint32_t *dst, const uint16_t *src, int count) {
    const uint32_t *src32 = (const uint32_t *)src;
    int pairs = count / 2;

    for (int i = 0; i < pairs; i++) {
        uint32_t two = src32[i];
        uint32_t p0 = two & 0xFFFF;
        uint32_t p1 = two >> 16;
        dst[i * 2] = p0 | (p0 << 16);
        dst[i * 2 + 1] = p1 | (p1 << 16);
    }
}

/**
 * Scanline callback - reads from line ring buffer
 * Called by Core 1 DMA ISR for every active video line
 * Performs 2x vertical scaling (line doubling) from 240p to 480p
 *
 * OSD rendering uses loop splitting - no per-pixel branching
 */
void __scratch_x("") mvs_scanline_doubler(uint32_t v_scanline, uint32_t active_line, uint32_t *dst) {
    // 2x Vertical Scaling: Every 240p line is shown twice to reach 480p
    uint32_t fb_line = active_line / 2;

    // Check bounds
    if (fb_line >= FRAME_HEIGHT) {
        memset(dst, 0, MODE_H_ACTIVE_PIXELS * 2);
        return;
    }

    // Vertical centering: black bars for lines outside MVS active area
    if (fb_line < V_OFFSET || fb_line >= V_OFFSET + MVS_HEIGHT) {
        memset(dst, 0, MODE_H_ACTIVE_PIXELS * 2);
        return;
    }

    uint16_t mvs_line = fb_line - V_OFFSET;

    // Check if line is ready in ring buffer
    if (!line_ring_ready(mvs_line)) {
        memset(dst, 0, MODE_H_ACTIVE_PIXELS * 2);
        return;
    }

    const uint16_t *src = line_ring_read_ptr(mvs_line);

    // Check if OSD is visible AND this line intersects OSD box
    // OSD coordinates are in 240p space (fb_line), same as video
    if (osd_visible && fb_line >= OSD_BOX_Y && fb_line < OSD_BOX_Y + OSD_BOX_H) {
        // OSD line within the OSD box
        uint32_t osd_line = fb_line - OSD_BOX_Y;
        const uint16_t *osd_src = osd_framebuffer[osd_line];

        // === Loop splitting: 3 regions, no per-pixel branching ===

        // Region 1: Before OSD box (0 to OSD_BOX_X)
        double_pixels_fast(dst, src, OSD_BOX_X);

        // Region 2: OSD box (OSD_BOX_X to OSD_BOX_X + OSD_BOX_W)
        double_pixels_fast(dst + OSD_BOX_X, osd_src, OSD_BOX_W);

        // Region 3: After OSD box (OSD_BOX_X + OSD_BOX_W to LINE_WIDTH)
        double_pixels_fast(dst + OSD_BOX_X + OSD_BOX_W,
                          src + OSD_BOX_X + OSD_BOX_W,
                          LINE_WIDTH - OSD_BOX_X - OSD_BOX_W);
    } else {
        // Fast path: no OSD on this line, full video doubling
        double_pixels_fast(dst, src, LINE_WIDTH);
    }
}

// ============================================================================
// Main (Core 0)
// ============================================================================

int main(void) {
    // Set system clock to 126 MHz for HSTX timing
    set_sys_clock_khz(126000, true);

    stdio_init_all();

    // Initialize LED for heartbeat
    gpio_init(PICO_DEFAULT_LED_PIN);
    gpio_set_dir(PICO_DEFAULT_LED_PIN, GPIO_OUT);

    // Initialize OSD button (active low with internal pull-up)
    // Wire button between GPIO and GND
    gpio_init(PIN_OSD_BTN_MENU);
    gpio_set_dir(PIN_OSD_BTN_MENU, GPIO_IN);
    gpio_pull_up(PIN_OSD_BTN_MENU);

    sleep_ms(1000);
    stdio_flush();

    // Initialize line ring buffer
    memset(&g_line_ring, 0, sizeof(g_line_ring));

    // Initialize OSD (hidden by default, press MENU button to toggle)
    osd_init();
    osd_puts(8, 8, "NeoPico-HD");
    osd_puts(8, 24, "Press MENU to hide");
    // OSD starts hidden - press button to show
    osd_hide();

    // Initialize HDMI output
    hstx_di_queue_init();
    video_output_init();

    // Register the scanline callback
    video_output_set_scanline_callback(mvs_scanline_doubler);

    // Initialize video capture
    video_capture_init(MVS_HEIGHT);
    sleep_ms(200);

    // Initialize audio subsystem
    audio_subsystem_init();
    audio_subsystem_start();
    stdio_flush();

    // Launch Core 1 for HSTX output
    multicore_launch_core1(video_output_core1_run);
    sleep_ms(100);

    // Core 0: Run video capture loop (never returns)
    // This captures lines into the ring buffer and signals VSYNC to Core 1
    video_capture_run();

    return 0;
}
