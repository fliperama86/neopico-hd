/**
 * HSTX Lab - MVS Video Capture + HDMI Output
 *
 * Captures live video from Neo Geo MVS and outputs via HSTX to HDMI.
 * Uses RP2350's hardware HSTX encoder instead of PIO-based PicoDVI.
 *
 * Architecture:
 *   Core 0: Continuous MVS video capture (PIO1 + DMA)
 *   Core 1: HSTX HDMI output at 640x480 @ 60Hz (2x scaled from 320x240)
 *
 * HSTX Pin Assignment (GPIO 12-19):
 *     GPIO 12: CLKN    GPIO 13: CLKP
 *     GPIO 14: D0N     GPIO 15: D0P  (Lane 0 - Blue)
 *     GPIO 16: D1N     GPIO 17: D1P  (Lane 1 - Green)
 *     GPIO 18: D2N     GPIO 19: D2P  (Lane 2 - Red)
 *
 * MVS Capture (GPIO 25-43 via PIO1):
 *     GPIO 25: PCLK, GPIO 26-30: Red, GPIO 31-35: Green,
 *     GPIO 36-40: Blue, GPIO 41: DARK, GPIO 42: SHADOW, GPIO 43: CSYNC
 *
 * Audio: 48kHz stereo via HDMI Data Islands (I2S capture from MVS, GPIO 0-2)
 *
 * Target: RP2350B (WeAct Studio board)
 */

#include "audio_subsystem.h"
#include "hardware/clocks.h"
#include "hdmi_data_island_queue.h"
#include "pico/multicore.h"
#include "pico/stdlib.h"
#include "pins.h"
#include "video_capture.h"
#include "video_output.h"
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

  printf("\n\n");
  printf("=================================\n");
  printf("HSTX Lab - MVS Capture + HDMI\n");
  printf("=================================\n");
  printf("Core 0: MVS video capture\n");
  printf("Core 1: HSTX 640x480 output\n\n");
  stdio_flush();

  // Initialize shared resources before launching Core 1
  init_background();
  hdmi_di_queue_init();
  video_output_init();

  // 1. Initialize video capture
  printf("Initializing video capture...\n");
  video_capture_init(framebuf, FRAMEBUF_WIDTH, FRAMEBUF_HEIGHT, 224);
  sleep_ms(200);

  // 2. Initialize audio subsystem
  audio_subsystem_init();
  audio_subsystem_start();
  stdio_flush();

  // 3. Launch Core 1 for HSTX output
  printf("Launching Core 1 for HSTX...\n");
  multicore_launch_core1(video_output_core1_run);
  sleep_ms(100);
  printf("Core 1 running.\n\n");

  printf("Starting continuous capture...\n");

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
