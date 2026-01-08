/**
 * NeoPico-HD - MVS Video Capture + HSTX Output
 */

#include "audio_subsystem.h"
#include "hardware/clocks.h"
#include "pico/multicore.h"
#include "pico/stdlib.h"
#include "pico_dvi2/hstx_data_island_queue.h"
#include "pico_dvi2/hstx_pins.h"
#include "pico_dvi2/video_output.h"
#include "video_capture.h"
#include <stdio.h>
#include <string.h>

// Initialize framebuffer to black (called once at init)
static void init_background(void) { memset(framebuf, 0, sizeof(framebuf)); }

// ============================================================================
// Main (Core 0)
// ============================================================================

int main(void) {
  // Set system clock to 126 MHz for exact 25.2 MHz pixel clock
  set_sys_clock_khz(126000, true);

  stdio_init_all();

  // Initialize LED for heartbeat
  gpio_init(PICO_DEFAULT_LED_PIN);
  gpio_set_dir(PICO_DEFAULT_LED_PIN, GPIO_OUT);

  sleep_ms(1000);

  // Flush any pending output
  stdio_flush();

  // Initialize shared resources before launching Core 1
  init_background();
  hstx_di_queue_init();
  video_output_init();

  // 1. Initialize video capture
  video_capture_init(framebuf, FRAMEBUF_WIDTH, FRAMEBUF_HEIGHT, 224);
  sleep_ms(200);

  // 2. Initialize audio subsystem
  audio_subsystem_init();
  audio_subsystem_start();
  stdio_flush();

  // 3. Launch Core 1 for HSTX output
  multicore_launch_core1(video_output_core1_run);
  sleep_ms(100);

  uint32_t led_toggle_frame = 0;
  bool led_state = false;

  while (1) {
    if (!video_capture_frame()) {
      audio_subsystem_stop();
      audio_subsystem_start();
    }

    if (video_frame_count >= led_toggle_frame + 30) {
      led_state = !led_state;
      gpio_put(PICO_DEFAULT_LED_PIN, led_state);
      led_toggle_frame = video_frame_count;
    }
  }

  return 0;
}
