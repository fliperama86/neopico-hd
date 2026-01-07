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

#include "audio_pipeline.h"
#include "data_packet.h"
#include "hardware/clocks.h"
#include "hardware/dma.h"
#include "hardware/gpio.h"
#include "hardware/irq.h"
#include "hardware/resets.h"
#include "hardware/structs/bus_ctrl.h"
#include "hardware/structs/hstx_ctrl.h"
#include "hardware/structs/hstx_fifo.h"
#include "hardware/vreg.h"
#include "pico/multicore.h"
#include "pico/stdlib.h"
#include "pins.h"
#include "video_capture.h"
#include "video_output.h"
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

// Audio pipeline instance
audio_pipeline_t audio_pipeline;

// Set to 1 to use real MVS audio capture, 0 for test tone
// NOTE: Requires I2S signals on GPIO 0-2 (BCK, WS, DAT)
#define USE_MVS_AUDIO 1

// Audio pipeline output callback - collects samples and encodes into data
// islands
static volatile uint32_t audio_samples_received = 0; // Debug counter

void audio_output_callback(const ap_sample_t *samples, uint32_t count,
                           void *ctx) {
  (void)ctx;
  audio_samples_received += count; // Debug: track total samples
  video_output_push_audio_samples((const audio_sample_t *)samples, count);
}

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

  // Set PIO GPIO bases (critical for RP2350B Bank 0/1 access)
  // PIO2 needs base=0 for I2S on GPIO 0-2
  // PIO1 will set its own base in video_capture_init
  pio_clear_instruction_memory(pio0);
  pio_clear_instruction_memory(pio1);
  pio_clear_instruction_memory(pio2);
  pio_set_gpio_base(pio0, 0);
  pio_set_gpio_base(pio1, 0);
  pio_set_gpio_base(pio2, 0);

  // Initialize shared resources before launching Core 1
  init_background();
  video_output_init();

  // 1. Claim DMA channels for HSTX FIRST (channels 0 and 1)
  dma_channel_claim(0); // DMACH_PING
  dma_channel_claim(1); // DMACH_PONG

  // 2. Initialize video capture
  printf("Initializing video capture...\n");
  video_capture_init(framebuf, FRAMEBUF_WIDTH, FRAMEBUF_HEIGHT, 224);
  sleep_ms(200);

  // 3. Initialize audio pipeline
#if USE_MVS_AUDIO
  printf("Initializing audio pipeline...\n");
  audio_pipeline_config_t audio_config = {.pin_bck = 2, // I2S_BCK_PIN
                                          .pin_dat = 0, // I2S_DAT_PIN
                                          .pin_ws = 1,  // I2S_WS_PIN
                                          .pin_btn1 = PIN_OSD_BTN_MENU,
                                          .pin_btn2 = PIN_OSD_BTN_BACK,
                                          .pio = pio2,
                                          .sm = 0};
  if (!audio_pipeline_init(&audio_pipeline, &audio_config)) {
    printf("ERROR: Failed to init audio pipeline!\n");
  } else {
    audio_pipeline_start(&audio_pipeline);
    printf("Audio pipeline started (I2S: BCK=GP2, WS=GP1, DAT=GP0)\n");
  }
  stdio_flush();
#endif

  // Launch Core 1 for HSTX output
  printf("Launching Core 1 for HSTX...\n");
  multicore_launch_core1(video_output_core1_run);
  sleep_ms(100);
  printf("Core 1 running.\n\n");

  printf("Starting continuous capture...\n");

  uint32_t led_toggle_frame = 0;
  bool led_state = false;

  while (1) {
    if (!video_capture_frame()) {
#if USE_MVS_AUDIO
      audio_pipeline_stop(&audio_pipeline);
      audio_pipeline_start(&audio_pipeline);
#endif
    }

    if (video_frame_count >= led_toggle_frame + 30) {
      led_state = !led_state;
      gpio_put(PICO_DEFAULT_LED_PIN, led_state);
      led_toggle_frame = video_frame_count;
    }
  }

  return 0;
}
