/**
 * NeoPico-HD - MVS Video Capture + HSTX Output
 *
 * Streaming architecture: line ring buffer between Core 0 (capture) and Core 1 (output)
 */

#include "pico_hdmi/hstx_data_island_queue.h"
#include "pico_hdmi/video_output.h"

#include "pico/multicore.h"
#include "pico/stdlib.h"

#include "hardware/clocks.h"

#include <stdio.h>
#include <string.h>

#include "audio_subsystem.h"
#include "mvs_pins.h"
#include "osd/osd.h"
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

    // Initialize video pipeline (HDMI output + callbacks)
    hstx_di_queue_init();
    video_pipeline_init(FRAME_WIDTH, FRAME_HEIGHT);

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
