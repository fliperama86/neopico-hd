/**
 * NeoPico-HD - MVS Capture with DVI Output
 *
 * Main coordinator - orchestrates video and audio subsystems
 */

#include "hardware/clocks.h"
#include "hardware/vreg.h"
#include "pico/stdlib.h"
#include <stdio.h>
#include <string.h>

#include "dvi.h"
#include "pins.h"

// Video modules
#include "video/video_capture.h"
#include "video/video_config.h"
#include "video/video_output.h"

// Audio modules
#include "audio/audio_config.h"
#include "audio/audio_output.h"
#include "audio/audio_pipeline.h"

// Test utilities
#include "misc/test_patterns.h"

// System configuration
#define VREG_VSEL VREG_VOLTAGE_1_20         // Voltage regulator setting
#define DVI_TIMING dvi_timing_640x480p_60hz // 480p timing for HDMI audio

// =============================================================================
// Framebuffer
// =============================================================================

static uint16_t g_framebuf[FRAME_WIDTH * FRAME_HEIGHT];

// =============================================================================
// Audio
// =============================================================================

static audio_sample_t audio_buffer[AUDIO_BUFFER_SIZE];
static audio_pipeline_t audio_pipeline;

// Audio output callback - writes processed samples to HDMI
static void audio_output_callback(const ap_sample_t *samples, uint32_t count,
                                  void *ctx) {
  (void)ctx; // Unused
  audio_output_write(samples, count);
}

// =============================================================================
// USB Frame Output (for Python viewer)
// =============================================================================

#define ENABLE_USB_FRAME_OUTPUT 0  // Disabled for GPIO debug (binary data clutters serial output)

#define SYNC_BYTE_1 0x55
#define SYNC_BYTE_2 0xAA

static void send_frame_usb(const uint16_t *framebuf, uint width, uint height) {
  // Send sync header (little-endian: 0xAA55, 0x55AA)
  putchar_raw(SYNC_BYTE_1);
  putchar_raw(SYNC_BYTE_2);
  putchar_raw(SYNC_BYTE_2);
  putchar_raw(SYNC_BYTE_1);

  // Send frame data as RGB565 (little-endian, 2 bytes per pixel)
  const uint8_t *bytes = (const uint8_t *)framebuf;
  uint32_t frame_bytes = width * height * 2;

  for (uint32_t i = 0; i < frame_bytes; i++) {
    putchar_raw(bytes[i]);
  }
}

// =============================================================================
// Main - Coordinator
// =============================================================================

int main() {
  vreg_set_voltage(VREG_VSEL);
  sleep_ms(10);
  set_sys_clock_khz(DVI_TIMING.bit_clk_khz, true);

  // stdio_init_all();  // DISABLED - USB serial causes glitches
  // sleep_ms(5000);

  // Set PIO GPIO bases (no printf to avoid USB overhead)
  pio_set_gpio_base(pio0, 0);
  pio_set_gpio_base(pio1, 0);
  pio_set_gpio_base(pio2, 0);

  gpio_init(PICO_DEFAULT_LED_PIN);
  gpio_set_dir(PICO_DEFAULT_LED_PIN, GPIO_OUT);
  gpio_put(PICO_DEFAULT_LED_PIN, 1);

  // Initialize framebuffer to black
  memset(g_framebuf, 0, sizeof(g_framebuf));

  // Color bars test (disabled - HDMI verified working)
  // fill_color_bars(g_framebuf, FRAME_WIDTH, FRAME_HEIGHT);

  // Initialize video output (DVI)
  video_output_init(g_framebuf, FRAME_WIDTH, FRAME_HEIGHT);

  // Initialize video capture (MVS)
  video_capture_init(g_framebuf, FRAME_WIDTH, FRAME_HEIGHT, MVS_HEIGHT);

  // ... audio skipped ...

  // Start DVI output on Core 1
  video_output_start();

  // Sync header for viewer
  const uint8_t sync_header[] = {0x55, 0xAA, 0xAA, 0x55};

  // Main capture loop
  uint32_t timeout_count = 0;
  while (true) {
    uint32_t frame_count = video_capture_get_frame_count();
    gpio_put(PICO_DEFAULT_LED_PIN, (frame_count / 10) & 1); // Blink 3x faster

    // Capture one frame (blocks until complete)
    if (!video_capture_frame()) {
      timeout_count++;
      // MINIMAL OVERHEAD TEST: Diagnostics disabled
      /*
      if (timeout_count % 10 == 0) {
        printf("Frame capture timeout (count: %lu)\n", timeout_count);
      }
      if (timeout_count % 100 == 0 && timeout_count > 0) {
        // Print diagnostic once to help debug
        extern PIO g_pio_mvs;  // Declare external from video_capture.c
        if (g_pio_mvs) {
          // Read ACTUAL GPIOBASE register (RP2350 offset 0x168)
          uint32_t actual_gpiobase = *(volatile uint32_t*)((uintptr_t)g_pio_mvs + 0x168);
          printf("*** GPIOBASE DIAGNOSTIC ***\n");
          printf("  RP2350 ACTUAL GPIOBASE: %lu (Expected: 16)\n", actual_gpiobase);
          printf("  PIO Instance: PIO%d\n", pio_get_index(g_pio_mvs));
        }
      }
      */
      continue; // Timeout, try again
    }

    // MINIMAL OVERHEAD TEST: Diagnostics disabled
    /*
    if (timeout_count > 0) {
      printf("Frame captured after %lu timeouts!\n", timeout_count);
      timeout_count = 0;
    }
    */
    timeout_count = 0;

    // USB frame output for Python viewer
#if ENABLE_USB_FRAME_OUTPUT
    if (frame_count % 10 == 0) {
      fwrite(sync_header, 1, 4, stdout);
      fwrite(g_framebuf, 1, FRAME_WIDTH * FRAME_HEIGHT * 2, stdout);
      fflush(stdout);
    }
#endif

    // Process audio: I2S → filters → SRC → HDMI
    // TEMPORARILY DISABLED
    // audio_pipeline_process(&audio_pipeline, audio_output_callback, NULL);
  }

  return 0;
}
