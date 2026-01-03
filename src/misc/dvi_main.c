/**
 * NeoPico-HD - MVS Capture with DVI Output
 *
 * Main coordinator - orchestrates video and audio subsystems
 */

#include "hardware/clocks.h"
#include "hardware/vreg.h"
#include "pico/stdlib.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "audio_ring.h"
#include "dvi.h"
#include "dvi_serialiser.h"
#include "pins.h"

// Video modules
#include "video/video_capture.h"
#include "video/video_config.h"
#include "video/video_output.h"

// Audiomodules
#include "audio/audio_config.h"
#include "audio/audio_output.h"
#include "audio/audio_pipeline.h"

// Test utilities
#include "misc/debug_render.h"
#include "misc/test_patterns.h"

// Debug configuration
#define DEBUG_AUDIO_INFO 0 // Set to 1 to enable audio debug screen

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

#define ENABLE_USB_FRAME_OUTPUT                                                \
  0 // Disabled for GPIO debug (binary data clutters serial output)

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

  stdio_init_all(); // Re-enabled for software reset
  sleep_ms(2000);   // Brief delay for USB

  // Set PIO GPIO bases
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

  // Initialize HDMI audio output
  dvi_audio_sample_buffer_set(video_output_get_dvi(), audio_buffer,
                              AUDIO_BUFFER_SIZE);
  audio_output_init(video_output_get_dvi(), AUDIO_OUTPUT_RATE);

  // Initialize video capture (MVS)
  video_capture_init(g_framebuf, FRAME_WIDTH, FRAME_HEIGHT, MVS_HEIGHT);

  // Initialize audio pipeline (I2S capture → processing → HDMI)
  audio_pipeline_config_t audio_cfg = {
      .pin_dat = PIN_I2S_DAT, // GP0
      .pin_ws = PIN_I2S_WS,   // GP1
      .pin_bck = PIN_I2S_BCK, // GP2
      .pin_btn1 = 0,          // Buttons disabled for now
      .pin_btn2 = 0,
      .pio = pio2, // Use PIO2 for audio (PIO0=DVI, PIO1=video)
      .sm = 0};
  audio_pipeline_init(&audio_pipeline, &audio_cfg);
  audio_pipeline_start(&audio_pipeline);

  // Start DVI output on Core 1
  video_output_start();

  // Sync header for viewer
  const uint8_t sync_header[] = {0x55, 0xAA, 0xAA, 0x55};

  // Main capture loop
  uint32_t timeout_count = 0;
  while (true) {
    uint32_t frame_count = video_capture_get_frame_count();
    gpio_put(PICO_DEFAULT_LED_PIN, (frame_count / 10) & 1); // Blink 3x faster

#if DEBUG_AUDIO_INFO
    // Replace game video with audio debug info
    audio_pipeline_status_t status;
    audio_pipeline_get_status(&audio_pipeline, &status);

    // Clear background
    memset(g_framebuf, 0, sizeof(g_framebuf));

    char buf[64];
    int y = 10;
    int x = 10;

    debug_draw_string(g_framebuf, FRAME_WIDTH, FRAME_HEIGHT, x, y,
                      "--- AUDIO DEBUG INFO ---", DEBUG_COLOR_YELLOW, 0);
    y += 12;

    // Hardware Probing (Direct silicon counters)
    debug_draw_string(g_framebuf, FRAME_WIDTH, FRAME_HEIGHT, x, y,
                      "[HARDWARE PROBE]", DEBUG_COLOR_GREEN, 0);
    y += 10;

    // Use simple logic to check for pin toggling (direct GPIO read)
    // Frequency counters are complex to setup for arbitrary GPIOs on RP2350
    static uint32_t ws_toggles = 0;
    static uint32_t bck_toggles = 0;
    static bool last_ws = false;
    static bool last_bck = false;

    for (int i = 0; i < 1000; i++) {
      bool ws = gpio_get(PIN_I2S_WS);
      bool bck = gpio_get(PIN_I2S_BCK);
      if (ws != last_ws) {
        ws_toggles++;
        last_ws = ws;
      }
      if (bck != last_bck) {
        bck_toggles++;
        last_bck = bck;
      }
    }

    sprintf(buf, "GP1 (WS) ACTIVITY:  %s", ws_toggles > 0 ? "YES" : "NO");
    debug_draw_string(g_framebuf, FRAME_WIDTH, FRAME_HEIGHT, x + 8, y, buf,
                      ws_toggles > 0 ? DEBUG_COLOR_WHITE : DEBUG_COLOR_RED, 0);
    y += 9;

    sprintf(buf, "GP2 (BCK) ACTIVITY: %s", bck_toggles > 0 ? "YES" : "NO");
    debug_draw_string(g_framebuf, FRAME_WIDTH, FRAME_HEIGHT, x + 8, y, buf,
                      bck_toggles > 0 ? DEBUG_COLOR_WHITE : DEBUG_COLOR_RED, 0);
    y += 12;

    // PIO Internals
    debug_draw_string(g_framebuf, FRAME_WIDTH, FRAME_HEIGHT, x, y,
                      "[PIO INTERNALS]", DEBUG_COLOR_GREEN, 0);
    y += 10;

    uint32_t pio_pc =
        pio_sm_get_pc(audio_pipeline.config.pio, audio_pipeline.config.sm);
    sprintf(buf, "PIO PC:   %lu", pio_pc);
    debug_draw_string(g_framebuf, FRAME_WIDTH, FRAME_HEIGHT, x + 8, y, buf,
                      DEBUG_COLOR_WHITE, 0);
    y += 9;

    uint32_t dma_addr = dma_hw->ch[audio_pipeline.capture.dma_chan].write_addr;
    sprintf(buf, "DMA ADDR: %08lX", dma_addr);
    debug_draw_string(g_framebuf, FRAME_WIDTH, FRAME_HEIGHT, x + 8, y, buf,
                      DEBUG_COLOR_WHITE, 0);
    y += 12;

    // Capture Stats (PIO Processing)
    debug_draw_string(g_framebuf, FRAME_WIDTH, FRAME_HEIGHT, x, y,
                      "[PIO CAPTURE]", DEBUG_COLOR_GREEN, 0);
    y += 10;

    sprintf(buf, "LRCK (MEAS): %lu HZ", status.capture_sample_rate);
    debug_draw_string(g_framebuf, FRAME_WIDTH, FRAME_HEIGHT, x + 8, y, buf,
                      DEBUG_COLOR_WHITE, 0);
    y += 9;

    sprintf(buf, "SAMPLES:     %lu", status.samples_captured);
    debug_draw_string(g_framebuf, FRAME_WIDTH, FRAME_HEIGHT, x + 8, y, buf,
                      DEBUG_COLOR_WHITE, 0);
    y += 9;

    sprintf(buf, "OVERFLOWS:   %lu", status.capture_overflows);
    debug_draw_string(
        g_framebuf, FRAME_WIDTH, FRAME_HEIGHT, x + 8, y, buf,
        status.capture_overflows > 0 ? DEBUG_COLOR_RED : DEBUG_COLOR_WHITE, 0);
    y += 12;

    // Processing Stats
    debug_draw_string(g_framebuf, FRAME_WIDTH, FRAME_HEIGHT, x, y, "[PIPELINE]",
                      DEBUG_COLOR_GREEN, 0);
    y += 10;

    sprintf(buf, "DC FILTER: %s", status.dc_filter_enabled ? "ON" : "OFF");
    debug_draw_string(g_framebuf, FRAME_WIDTH, FRAME_HEIGHT, x + 8, y, buf,
                      DEBUG_COLOR_WHITE, 0);
    y += 9;

    sprintf(buf, "LOWPASS:   %s", status.lowpass_enabled ? "ON" : "OFF");
    debug_draw_string(g_framebuf, FRAME_WIDTH, FRAME_HEIGHT, x + 8, y, buf,
                      DEBUG_COLOR_WHITE, 0);
    y += 9;

    const char *src_modes[] = {"NONE", "DROP", "LINEAR"};
    sprintf(buf, "SRC MODE:  %s", src_modes[status.src_mode]);
    debug_draw_string(g_framebuf, FRAME_WIDTH, FRAME_HEIGHT, x + 8, y, buf,
                      DEBUG_COLOR_WHITE, 0);
    y += 12;

    // Output Stats
    debug_draw_string(g_framebuf, FRAME_WIDTH, FRAME_HEIGHT, x, y, "[OUTPUT]",
                      DEBUG_COLOR_GREEN, 0);
    y += 10;

    sprintf(buf, "RATE: %lu HZ", status.output_sample_rate);
    debug_draw_string(g_framebuf, FRAME_WIDTH, FRAME_HEIGHT, x + 8, y, buf,
                      DEBUG_COLOR_WHITE, 0);
    y += 9;

    sprintf(buf, "SAMPLES:   %lu", status.samples_output);
    debug_draw_string(g_framebuf, FRAME_WIDTH, FRAME_HEIGHT, x + 8, y, buf,
                      DEBUG_COLOR_WHITE, 0);
    y += 9;

    sprintf(buf, "UNDERRUNS: %lu", status.output_underruns);
    debug_draw_string(
        g_framebuf, FRAME_WIDTH, FRAME_HEIGHT, x + 8, y, buf,
        status.output_underruns > 0 ? DEBUG_COLOR_RED : DEBUG_COLOR_WHITE, 0);

    // Maintain ~60fps loop rate when capture is disabled
    sleep_ms(16);
#else
    // Capture one frame (blocks until complete)
    if (!video_capture_frame()) {
      timeout_count++;
      continue; // Timeout, try again
    }
#endif

    timeout_count = 0;

    // USB frame output for Python viewer
#if ENABLE_USB_FRAME_OUTPUT
    if (frame_count % 10 == 0) {
      fwrite(sync_header, 1, 4, stdout);
      fwrite(g_framebuf, 1, FRAME_WIDTH * FRAME_HEIGHT * 2, stdout);
      fflush(stdout);
    }
#endif

    // Process all available audio samples to avoid ring buffer overflow
    // The capture rate is 55.5kHz, so we need to drain the ring frequently.
    // Kick the process once to trigger the hardware poll
    audio_pipeline_process(&audio_pipeline, audio_output_callback, NULL);

    uint32_t audio_available;
    while ((audio_available = ap_ring_available(&audio_pipeline.capture_ring)) >
           0) {
      audio_pipeline_process(&audio_pipeline, audio_output_callback, NULL);
    }
  }

  return 0;
}
