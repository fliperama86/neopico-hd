/**
 * NeoPico-HD - MVS Capture with DVI Output
 *
 * Main coordinator - orchestrates video and audio subsystems
 *
 * Pin Configuration:
 *   MVS PCLK:      GPIO 0 (C2)
 *   MVS Green:     GPIO 1-5 (G4-G0, reversed bit order)
 *   MVS Blue:      GPIO 6-10 (B0-B4)
 *   MVS Red:       GPIO 11-15 (R0-R4)
 *   MVS CSYNC:     GPIO 22
 *   DVI Clock:     GPIO 25-26
 *   DVI Data:      GPIO 27-32
 *   I2S DAT:       GPIO 36
 *   I2S WS:        GPIO 37
 *   I2S BCK:       GPIO 38
 */

#include "hardware/clocks.h"
#include "hardware/dma.h"
#include "hardware/vreg.h"
#include "pico/stdlib.h"
#include <stdio.h>
#include <string.h>

#include "audio_ring.h"
#include "dvi.h"

// Video modules
#include "video/video_capture.h"
#include "video/video_output.h"

// Audio modules
#include "audio/audio_buffer.h"
#include "audio/dc_filter.h"
#include "audio/i2s_capture.h"
#include "audio/lowpass.h"
#include "audio/src.h"

#define FRAME_WIDTH 320
#define FRAME_HEIGHT 240 // 480p: 240 * 2 = 480 DVI lines
#define MVS_HEIGHT 224
#define VREG_VSEL VREG_VOLTAGE_1_20

// Use 480p timing (252 MHz) - required for HDMI audio
#define DVI_TIMING dvi_timing_640x480p_60hz

// =============================================================================
// Framebuffer
// =============================================================================

static uint16_t g_framebuf[FRAME_WIDTH * FRAME_HEIGHT];

// =============================================================================
// Audio
// =============================================================================

#define AUDIO_BUFFER_SIZE 1024 // Larger buffer to handle irregular fill timing

// I2S pins (directly on RP2350, GPIO 36-38)
#define I2S_PIN_DAT 36
#define I2S_PIN_WS 37
#define I2S_PIN_BCK 38

static audio_sample_t audio_buffer[AUDIO_BUFFER_SIZE];
static ap_ring_t i2s_ring;
static i2s_capture_t i2s_cap;
static src_t src;
static dc_filter_t dc_filter;
static lowpass_t lowpass;

// Temporary buffers for SRC processing
static ap_sample_t src_in_buf[256];
static ap_sample_t src_out_buf[256];

// =============================================================================
// Audio Processing
// =============================================================================

static void poll_i2s_capture(void) { i2s_capture_poll(&i2s_cap); }

static void fill_audio_buffer(void) {
  struct dvi_inst *dvi = video_output_get_dvi();
  int space = get_write_size(&dvi->audio_ring, false);
  if (space == 0)
    return;

  // Read available samples from I2S ring
  uint32_t available = ap_ring_available(&i2s_ring);
  if (available == 0)
    return;

  uint32_t to_read = (available < 256) ? available : 256;
  for (uint32_t i = 0; i < to_read; i++) {
    src_in_buf[i] = ap_ring_read(&i2s_ring);
  }

  // Apply filters (before SRC, at input sample rate)
  dc_filter_process_buffer(&dc_filter, src_in_buf, to_read);
  lowpass_process_buffer(&lowpass, src_in_buf, to_read);

  // Apply SRC (55.5kHz -> 48kHz)
  uint32_t in_consumed = 0;
  uint32_t out_max = (space < 256) ? space : 256;
  uint32_t out_count = src_process(&src, src_in_buf, to_read, src_out_buf,
                                   out_max, &in_consumed);

  // Write to HDMI audio ring
  audio_sample_t *audio_ptr = get_write_pointer(&dvi->audio_ring);
  for (uint32_t i = 0; i < out_count; i++) {
    audio_ptr->channels[0] = src_out_buf[i].left;
    audio_ptr->channels[1] = src_out_buf[i].right;
    audio_ptr++;
  }
  increase_write_pointer(&dvi->audio_ring, out_count);
}

// =============================================================================
// Main - Coordinator
// =============================================================================

int main() {
  vreg_set_voltage(VREG_VSEL);
  sleep_ms(10);
  set_sys_clock_khz(DVI_TIMING.bit_clk_khz, true);

  stdio_init_all();

  gpio_init(PICO_DEFAULT_LED_PIN);
  gpio_set_dir(PICO_DEFAULT_LED_PIN, GPIO_OUT);
  gpio_put(PICO_DEFAULT_LED_PIN, 1);

  printf("NeoPico-HD: MVS Capture + DVI Output (Line-by-line 60fps)\n");

  // Initialize framebuffer to black
  memset(g_framebuf, 0, sizeof(g_framebuf));

  // Initialize video output (DVI)
  video_output_init(g_framebuf, FRAME_WIDTH, FRAME_HEIGHT);

  // Initialize video capture (MVS)
  video_capture_init(g_framebuf, FRAME_WIDTH, FRAME_HEIGHT, MVS_HEIGHT);

  // HDMI Audio setup (no blank_settings, vblank-driven)
  struct dvi_inst *dvi = video_output_get_dvi();
  dvi_audio_sample_buffer_set(dvi, audio_buffer, AUDIO_BUFFER_SIZE);
  dvi_set_audio_freq(dvi, 48000, 25200, 6144);
  // Pre-fill audio buffer before starting DVI
  for (int i = 0; i < 4; i++)
    fill_audio_buffer();
  printf("HDMI audio initialized (vblank-driven)\n");

  // Initialize I2S audio capture on PIO2 (separate from MVS on PIO1)
  ap_ring_init(&i2s_ring);
  i2s_capture_config_t i2s_cfg = {
      .pin_dat = I2S_PIN_DAT,
      .pin_ws = I2S_PIN_WS,
      .pin_bck = I2S_PIN_BCK,
      .pio = pio2, // Use PIO2 (RP2350 has 3 PIOs!)
      .sm = 0,
  };
  i2s_capture_init(&i2s_cap, &i2s_cfg, &i2s_ring);
  i2s_capture_start(&i2s_cap);

  // Initialize SRC (55.5kHz -> 48kHz)
  src_init(&src, SRC_INPUT_RATE_DEFAULT, SRC_OUTPUT_RATE_DEFAULT);
  src_set_mode(&src,
               SRC_MODE_LINEAR); // Linear interpolation for better quality

  // Initialize audio filters
  dc_filter_init(&dc_filter);
  dc_filter_set_enabled(&dc_filter, true);
  lowpass_init(&lowpass);
  lowpass_set_enabled(&lowpass, true);

  // Start DVI output on Core 1
  printf("Starting Core 1 (DVI)\n");
  video_output_start();

  printf("Starting line-by-line capture loop\n");

  // Frame rate measurement
  uint32_t last_time = time_us_32();
  uint32_t fps_frame_count = 0;

  // Main capture loop
  while (true) {
    fps_frame_count++;
    uint32_t frame_count = video_capture_get_frame_count();
    gpio_put(PICO_DEFAULT_LED_PIN, (frame_count / 30) & 1);

    // Capture one frame (blocks until complete)
    if (!video_capture_frame()) {
      continue; // Timeout, try again
    }

    // Poll I2S and fill HDMI audio (safe - no capture running)
    poll_i2s_capture();
    fill_audio_buffer(); // Consumes from i2s_ring, outputs to HDMI
  }

  return 0;
}
