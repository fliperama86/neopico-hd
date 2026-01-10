/**
 * NeoPico-HD - MVS Video Capture + HSTX Output
 *
 * Streaming architecture: line ring buffer between Core 0 (capture) and Core 1 (output)
 */

#include "audio_subsystem.h"
#include "hardware/clocks.h"
#include "mvs_pins.h"
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
 * Scanline callback - reads from line ring buffer
 * Called by Core 1 DMA ISR for every active video line
 * Performs 2x vertical scaling (line doubling) from 240p to 480p
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
    if (line_ring_ready(mvs_line)) {
        const uint16_t *src = line_ring_read_ptr(mvs_line);

        // 2x horizontal doubling (320 -> 640)
        for (int i = 0; i < LINE_WIDTH; i++) {
            uint32_t p = src[i];
            dst[i] = p | (p << 16);
        }
    } else {
        // Line not ready - output black (underrun)
        memset(dst, 0, MODE_H_ACTIVE_PIXELS * 2);
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

    sleep_ms(1000);
    stdio_flush();

    // Initialize line ring buffer
    memset(&g_line_ring, 0, sizeof(g_line_ring));

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
