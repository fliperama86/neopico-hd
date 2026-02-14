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

#include <stdio.h>
#include <string.h>

#include "audio_subsystem.h"
#include "mvs_pins.h"
#include "osd/osd.h"
#include "osd/selftest.h"
#include "video/line_ring.h"
#include "video/video_config.h"
#include "video/video_pipeline.h"
#include "video_capture.h"

// Line ring buffer (shared between Core 0 and Core 1)
line_ring_t g_line_ring __attribute__((aligned(64)));

// ============================================================================
// Main (Core 0)
// ============================================================================

int main(void)
{
    sleep_ms(2000);
    // Set system clock to 126 MHz for HSTX timing
    set_sys_clock_khz(126000, true);

    stdio_init_all();

    // Initialize OSD buttons (active low with internal pull-up)
    gpio_init(PIN_OSD_BTN_MENU);
    gpio_set_dir(PIN_OSD_BTN_MENU, GPIO_IN);
    gpio_pull_up(PIN_OSD_BTN_MENU);

    gpio_init(PIN_OSD_BTN_BACK);
    gpio_set_dir(PIN_OSD_BTN_BACK, GPIO_IN);
    gpio_pull_up(PIN_OSD_BTN_BACK);

    sleep_ms(1000);
    stdio_flush();

    // Initialize line ring buffer
    memset(&g_line_ring, 0, sizeof(g_line_ring));

    // Initialize OSD (before video pipeline so framebuffer is ready)
    osd_init();

    // Initialize HDMI output pipeline
    hstx_di_queue_init();
    video_pipeline_init(FRAME_WIDTH, FRAME_HEIGHT);
    video_output_set_background_task(audio_subsystem_background_task);

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
