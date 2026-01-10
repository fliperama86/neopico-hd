/**
 * NeoPico-HD - MVS Video Capture + HSTX Output
 */

#include "audio_subsystem.h"
#include "hardware/clocks.h"
#include "mvs_pins.h"
#include "pico/multicore.h"
#include "pico/stdlib.h"
#include "pico_dvi2/hstx_data_island_queue.h"
#include "pico_dvi2/video_output.h"
#include "video_capture.h"
#include <stdio.h>
#include <string.h>

// Framebuffer resolution (MVS native)
#define FRAMEBUF_WIDTH 320
#define FRAMEBUF_HEIGHT 240

// Main video framebuffer
uint16_t framebuf[FRAMEBUF_HEIGHT * FRAMEBUF_WIDTH] __attribute__((aligned(4)));

// Initialize framebuffer to black (called once at init)
static void init_background(void) { memset(framebuf, 0, sizeof(framebuf)); }

// ============================================================================
// DVI Scanline Callback (Core 1)
// ============================================================================

/**
 * This function is called by the DVI library on Core 1 for every active video line.
 * It performs 2x vertical scaling (line doubling) from 240p to 480p.
 */
void __scratch_x("") mvs_scanline_doubler(uint32_t v_scanline, uint32_t active_line, uint32_t *line_buffer) {
  // 2x Vertical Scaling: Every 240p line is shown twice to reach 480p
  uint32_t fb_line = active_line / 2;
  
  if (fb_line >= FRAMEBUF_HEIGHT) {
    memset(line_buffer, 0, MODE_H_ACTIVE_PIXELS * 2);
    return;
  }

  const uint16_t *src = &framebuf[fb_line * FRAMEBUF_WIDTH];
  uint32_t *dst = line_buffer;

  // 2x Horizontal Scaling: Each 16-bit pixel is duplicated into a 32-bit word
  // This effectively doubles the pixel width from 320 to 640.
  for (uint32_t i = 0; i < FRAMEBUF_WIDTH; i++) {
    uint32_t p = src[i];
    dst[i] = p | (p << 16);
  }
}

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
  
  // Register the scanline doubler callback
  video_output_set_scanline_callback(mvs_scanline_doubler);

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
