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
#include "hardware/structs/bus_ctrl.h"
#include "hardware/structs/hstx_ctrl.h"
#include "hardware/structs/hstx_fifo.h"
#include "pico/multicore.h"
#include "pico/stdlib.h"
#include "video_capture.h"
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

// ============================================================================
// DVI/HDMI Constants
// ============================================================================

#define TMDS_CTRL_00 0x354u // vsync=0 hsync=0
#define TMDS_CTRL_01 0x0abu // vsync=0 hsync=1
#define TMDS_CTRL_10 0x154u // vsync=1 hsync=0
#define TMDS_CTRL_11 0x2abu // vsync=1 hsync=1

// Sync symbols: Lane 0 carries sync, Lanes 1&2 are always CTRL_00
#define SYNC_V0_H0 (TMDS_CTRL_00 | (TMDS_CTRL_00 << 10) | (TMDS_CTRL_00 << 20))
#define SYNC_V0_H1 (TMDS_CTRL_01 | (TMDS_CTRL_00 << 10) | (TMDS_CTRL_00 << 20))
#define SYNC_V1_H0 (TMDS_CTRL_10 | (TMDS_CTRL_00 << 10) | (TMDS_CTRL_00 << 20))
#define SYNC_V1_H1 (TMDS_CTRL_11 | (TMDS_CTRL_00 << 10) | (TMDS_CTRL_00 << 20))

// Data Island preamble: Lane 0 = sync, Lanes 1&2 = CTRL_01 pattern
// Used during hsync pulse (hsync=0), so use H0 variants
#define PREAMBLE_V0_H0                                                         \
  (TMDS_CTRL_00 | (TMDS_CTRL_01 << 10) | (TMDS_CTRL_01 << 20))
#define PREAMBLE_V1_H0                                                         \
  (TMDS_CTRL_10 | (TMDS_CTRL_01 << 10) | (TMDS_CTRL_01 << 20))

// 480p timing (output resolution with 2×2 pixel doubling)
#define MODE_H_FRONT_PORCH 16
#define MODE_H_SYNC_WIDTH 96
#define MODE_H_BACK_PORCH 48
#define MODE_H_ACTIVE_PIXELS 640

#define MODE_V_FRONT_PORCH 10
#define MODE_V_SYNC_WIDTH 2
#define MODE_V_BACK_PORCH 33
#define MODE_V_ACTIVE_LINES 480

// Internal framebuffer resolution (MVS native, 2×2 doubled to 640×480)
#define FRAMEBUF_WIDTH 320
#define FRAMEBUF_HEIGHT 240

#define MODE_H_TOTAL_PIXELS                                                    \
  (MODE_H_FRONT_PORCH + MODE_H_SYNC_WIDTH + MODE_H_BACK_PORCH +                \
   MODE_H_ACTIVE_PIXELS)
#define MODE_V_TOTAL_LINES                                                     \
  (MODE_V_FRONT_PORCH + MODE_V_SYNC_WIDTH + MODE_V_BACK_PORCH +                \
   MODE_V_ACTIVE_LINES)

// HSTX commands
#define HSTX_CMD_RAW (0x0u << 12)
#define HSTX_CMD_RAW_REPEAT (0x1u << 12)
#define HSTX_CMD_TMDS (0x2u << 12)
#define HSTX_CMD_TMDS_REPEAT (0x3u << 12)
#define HSTX_CMD_NOP (0xfu << 12)

// ============================================================================
// Audio Configuration
// ============================================================================

// 48kHz stereo - needs 800 samples/frame
#define AUDIO_SAMPLE_RATE 48000
#define AUDIO_N_VALUE 6144    // Standard N for 48kHz
#define AUDIO_CTS_VALUE 25200 // CTS for 25.2MHz pixel clock (800×525×60Hz)

// I2S capture pins (MVS audio input) - must match pins.h!
#define I2S_DAT_PIN 0 // GPIO 0: Data
#define I2S_WS_PIN 1  // GPIO 1: Word select (LRCK)
#define I2S_BCK_PIN 2 // GPIO 2: Bit clock

// Audio pipeline instance
static audio_pipeline_t audio_pipeline;

// Audio generation (fallback test tone)
#define TONE_FREQUENCY 440  // A4 note (440 Hz)
#define TONE_AMPLITUDE 8000 // ~25% of int16 max

// Audio state
static volatile uint32_t video_frame_count =
    0; // volatile: updated on Core 1, read on Core 0
static int audio_frame_counter = 0;       // IEC60958 frame counter (0-191)
static uint32_t audio_phase = 0x40000000; // Start at 1/4 cycle (90 degrees)

// Error detection and LED heartbeat
static volatile bool error_detected = false;
static const char *error_message = NULL;

#define ASSERT_NO_ERROR(condition, msg)                                        \
  do {                                                                         \
    if (!(condition)) {                                                        \
      error_detected = true;                                                   \
      error_message = msg;                                                     \
      printf("ERROR: %s\n", msg);                                              \
      while (1) {                                                              \
        gpio_put(PICO_DEFAULT_LED_PIN, 1);                                     \
        tight_loop_contents();                                                 \
      }                                                                        \
    }                                                                          \
  } while (0)
static uint32_t audio_sample_accum = 0; // Fixed-point accumulator

// Pre-computed sine table for fast audio generation
#define SINE_TABLE_SIZE 256
static int16_t sine_table[SINE_TABLE_SIZE];

// Phase increment for 440Hz tone at 48kHz sample rate
// phase_inc = (440 << 32) / 48000 = 39370534
#define TONE_PHASE_INC 39370534

// Fast inline sine sample using table lookup
static inline int16_t fast_sine_sample(void) {
  int16_t s = sine_table[(audio_phase >> 24) & 0xFF];
  audio_phase += TONE_PHASE_INC;
  return s;
}

// Audio ring buffer for pre-encoded data islands
// Increased from 64 to reduce underruns (64 = 5ms, 256 = 21ms buffering)
#define AUDIO_RING_BUFFER_SIZE 256
static hstx_data_island_t audio_ring_buffer[AUDIO_RING_BUFFER_SIZE];
static volatile uint32_t audio_ring_head = 0;
static volatile uint32_t audio_ring_tail = 0;

// Set to 1 to use real MVS audio capture, 0 for test tone
// NOTE: Requires I2S signals on GPIO 0-2 (BCK, WS, DAT)
#define USE_MVS_AUDIO 1

// Buffer for collecting audio samples before encoding into data islands
#define AUDIO_COLLECT_SIZE 128
static ap_sample_t audio_collect_buffer[AUDIO_COLLECT_SIZE];
static uint32_t audio_collect_count = 0;

// Audio pipeline output callback - collects samples and encodes into data
// islands
static volatile uint32_t audio_samples_received = 0; // Debug counter

static void audio_output_callback(const ap_sample_t *samples, uint32_t count,
                                  void *ctx) {
  (void)ctx;

  audio_samples_received += count; // Debug: track total samples

  // Copy samples to collection buffer
  for (uint32_t i = 0; i < count; i++) {
    if (audio_collect_count < AUDIO_COLLECT_SIZE) {
      audio_collect_buffer[audio_collect_count++] = samples[i];
    }

    // When we have 4 samples, encode into a data island
    if (audio_collect_count >= 4) {
      // Check if we have room in ring buffer
      uint32_t next_head = (audio_ring_head + 1) % AUDIO_RING_BUFFER_SIZE;
      if (next_head != audio_ring_tail) {
        audio_sample_t encoded_samples[4];
        for (int j = 0; j < 4; j++) {
          encoded_samples[j].left = audio_collect_buffer[j].left;
          encoded_samples[j].right = audio_collect_buffer[j].right;
        }

        data_packet_t packet;
        audio_frame_counter = packet_set_audio_samples(&packet, encoded_samples,
                                                       4, audio_frame_counter);

        // Audio sample packets are sent outside VSync lines
        hstx_encode_data_island(&audio_ring_buffer[audio_ring_head], &packet,
                                false, true);
        audio_ring_head = next_head;

        // Shift remaining samples
        audio_collect_count -= 4;
        for (uint32_t j = 0; j < audio_collect_count; j++) {
          audio_collect_buffer[j] = audio_collect_buffer[j + 4];
        }
      } else {
        // Ring buffer full, we have to drop these samples or wait.
        // For now, just stop collecting to avoid overflow.
        break;
      }
    }
  }
}

// Audio samples per frame (48kHz / 60fps = 800 samples)
#define SAMPLES_PER_FRAME (AUDIO_SAMPLE_RATE / 60)
#define LINES_PER_FRAME (MODE_V_TOTAL_LINES)

// Fixed-point accumulator for fractional samples per line (16.16 format)
#define SAMPLES_PER_LINE_FP ((SAMPLES_PER_FRAME << 16) / LINES_PER_FRAME)

// Set to 1 to enable audio during active video lines
#define ENABLE_ACTIVE_LINE_AUDIO 1

// Audio samples per frame (48kHz / 60fps = 800 samples)

// ============================================================================
// Framebuffer (320×240 MVS native, 2×2 doubled to 640×480, RGB565)
// ============================================================================

static uint16_t framebuf[FRAMEBUF_HEIGHT * FRAMEBUF_WIDTH]
    __attribute__((aligned(4)));

// Line buffer for horizontal pixel doubling (320 → 640 pixels, RGB565)
// Aligned for fast DMA access
static uint16_t line_buffer[MODE_H_ACTIVE_PIXELS] __attribute__((aligned(4)));

// Initialize framebuffer to black (called once at init)
static void init_background(void) {
  memset(framebuf, 0, sizeof(framebuf));
}

// ============================================================================
// Command Lists (pre-computed at init time)
// ============================================================================

static uint32_t vblank_line_vsync_off[] = {
    HSTX_CMD_RAW_REPEAT | MODE_H_FRONT_PORCH,
    SYNC_V1_H1,
    HSTX_CMD_NOP,
    HSTX_CMD_RAW_REPEAT | MODE_H_SYNC_WIDTH,
    SYNC_V1_H0,
    HSTX_CMD_NOP,
    HSTX_CMD_RAW_REPEAT | (MODE_H_BACK_PORCH + MODE_H_ACTIVE_PIXELS),
    SYNC_V1_H1,
    HSTX_CMD_NOP};

static uint32_t vblank_line_vsync_on[] = {
    HSTX_CMD_RAW_REPEAT | MODE_H_FRONT_PORCH,
    SYNC_V0_H1,
    HSTX_CMD_NOP,
    HSTX_CMD_RAW_REPEAT | MODE_H_SYNC_WIDTH,
    SYNC_V0_H0,
    HSTX_CMD_NOP,
    HSTX_CMD_RAW_REPEAT | (MODE_H_BACK_PORCH + MODE_H_ACTIVE_PIXELS),
    SYNC_V0_H1,
    HSTX_CMD_NOP};

static uint32_t vactive_line[] = {HSTX_CMD_RAW_REPEAT | MODE_H_FRONT_PORCH,
                                  SYNC_V1_H1,
                                  HSTX_CMD_NOP,
                                  HSTX_CMD_RAW_REPEAT | MODE_H_SYNC_WIDTH,
                                  SYNC_V1_H0,
                                  HSTX_CMD_NOP,
                                  HSTX_CMD_RAW_REPEAT | MODE_H_BACK_PORCH,
                                  SYNC_V1_H1,
                                  HSTX_CMD_TMDS | MODE_H_ACTIVE_PIXELS};

// Data island placement: At START of hsync pulse (matching pico_lib)
#define SYNC_BEFORE_DI 0 // DI starts immediately at hsync transition
#define SYNC_AFTER_DI                                                          \
  (MODE_H_SYNC_WIDTH - W_PREAMBLE - W_DATA_ISLAND) // Remaining hsync after DI

#define VBLANK_DI_MAX_WORDS 64
static uint32_t vblank_acr_vsync_on[VBLANK_DI_MAX_WORDS];
static uint32_t vblank_acr_vsync_on_len;
static uint32_t vblank_infoframe_vsync_on[VBLANK_DI_MAX_WORDS];
static uint32_t vblank_infoframe_vsync_on_len;
static uint32_t vblank_avi_infoframe[VBLANK_DI_MAX_WORDS];
static uint32_t vblank_avi_infoframe_len;

#define VACTIVE_DI_MAX_WORDS 128
static uint32_t vactive_di_ping[VACTIVE_DI_MAX_WORDS];
static uint32_t vactive_di_pong[VACTIVE_DI_MAX_WORDS];
static uint32_t vactive_di_len;

static uint32_t vactive_di_null[VACTIVE_DI_MAX_WORDS];
static uint32_t vactive_di_null_len;

static uint32_t vblank_di_ping[VACTIVE_DI_MAX_WORDS];
static uint32_t vblank_di_pong[VACTIVE_DI_MAX_WORDS];
static uint32_t vblank_di_len;

static uint32_t vblank_di_null[VACTIVE_DI_MAX_WORDS];
static uint32_t vblank_di_null_len;

static void init_sine_table(void) {
  for (int i = 0; i < SINE_TABLE_SIZE; i++) {
    float angle = (float)i * 2.0f * 3.14159265f / SINE_TABLE_SIZE;
    sine_table[i] = (int16_t)(sinf(angle) * TONE_AMPLITUDE);
  }
}

static uint32_t vblank_acr_vsync_off[VBLANK_DI_MAX_WORDS];
static uint32_t vblank_acr_vsync_off_len;

static uint32_t vblank_infoframe_vsync_off[VBLANK_DI_MAX_WORDS];
static uint32_t vblank_infoframe_vsync_off_len;

static uint32_t build_line_with_di(uint32_t *buf, const uint32_t *di_words,
                                   bool vsync, bool active) {
  uint32_t *p = buf;

  // Sync bit logic: active-low (0=active, 1=idle)
  uint32_t sync_h0 = vsync ? SYNC_V0_H0 : SYNC_V1_H0;
  uint32_t sync_h1 = vsync ? SYNC_V0_H1 : SYNC_V1_H1;
  uint32_t preamble = vsync ? PREAMBLE_V0_H0 : PREAMBLE_V1_H0;

  // 1. Front Porch (16 clocks)
  *p++ = HSTX_CMD_RAW_REPEAT | MODE_H_FRONT_PORCH;
  *p++ = sync_h1;
  *p++ = HSTX_CMD_NOP;

  // 2. Sync pulse with Data Island
  // Preamble (8 clocks)
  *p++ = HSTX_CMD_RAW_REPEAT | W_PREAMBLE;
  *p++ = preamble;
  *p++ = HSTX_CMD_NOP;

  // Data Island (36 clocks)
  *p++ = HSTX_CMD_RAW | W_DATA_ISLAND;
  for (int i = 0; i < W_DATA_ISLAND; i++) {
    *p++ = di_words[i];
  }
  *p++ = HSTX_CMD_NOP;

  // Remaining hsync after DI (52 clocks, H=0)
  *p++ = HSTX_CMD_RAW_REPEAT | SYNC_AFTER_DI;
  *p++ = sync_h0;
  *p++ = HSTX_CMD_NOP;

  // 3. Back Porch (48 clocks)
  if (active) {
    // For active lines, we chain to the pixels after the back porch.
    *p++ = HSTX_CMD_RAW_REPEAT | MODE_H_BACK_PORCH;
    *p++ = sync_h1;
    *p++ = HSTX_CMD_TMDS | MODE_H_ACTIVE_PIXELS;
  } else {
    // For blanking lines, we include the active area in the back porch.
    *p++ = HSTX_CMD_RAW_REPEAT | (MODE_H_BACK_PORCH + MODE_H_ACTIVE_PIXELS);
    *p++ = sync_h1;
    *p++ = HSTX_CMD_NOP;
  }

  return (uint32_t)(p - buf);
}

static uint32_t build_vblank_with_di(uint32_t *buf, const uint32_t *di_words,
                                     bool vsync) {
  return build_line_with_di(buf, di_words, vsync, false);
}

static void init_audio_packets(void) {
  data_packet_t packet;
  hstx_data_island_t island;

  packet_set_acr(&packet, AUDIO_N_VALUE, AUDIO_CTS_VALUE);
  hstx_encode_data_island(&island, &packet, true, true); // ACR in VSync
  vblank_acr_vsync_on_len =
      build_vblank_with_di(vblank_acr_vsync_on, island.words, true);

  hstx_encode_data_island(&island, &packet, false, true); // ACR outside VSync
  vblank_acr_vsync_off_len =
      build_vblank_with_di(vblank_acr_vsync_off, island.words, false);

  packet_set_audio_infoframe(&packet, AUDIO_SAMPLE_RATE, 2, 16);
  hstx_encode_data_island(&island, &packet, true, true); // InfoFrame in VSync
  vblank_infoframe_vsync_on_len =
      build_vblank_with_di(vblank_infoframe_vsync_on, island.words, true);

  hstx_encode_data_island(&island, &packet, false,
                          true); // InfoFrame outside VSync
  vblank_infoframe_vsync_off_len =
      build_vblank_with_di(vblank_infoframe_vsync_off, island.words, false);

  packet_set_avi_infoframe(&packet, 1);
  hstx_encode_data_island(&island, &packet, false, true); // AVI outside VSync
  vblank_avi_infoframe_len =
      build_vblank_with_di(vblank_avi_infoframe, island.words, false);

  // Pre-calculate NULL Data Island lines for both blank and active states
  // hsync_active=true (0) because Data Islands are sent during the HSync pulse.
  vblank_di_null_len = build_line_with_di(
      vblank_di_null, hstx_get_null_data_island(false, true), false, false);
  vactive_di_null_len = build_line_with_di(
      vactive_di_null, hstx_get_null_data_island(false, true), false, true);

  printf("Audio initialized:\n");
  printf("  48kHz, N=%d, CTS=%d\n", AUDIO_N_VALUE, AUDIO_CTS_VALUE);
}

// ============================================================================
// DMA Handler
// ============================================================================

#define DMACH_PING 0
#define DMACH_PONG 1

static bool dma_pong = false;
static uint32_t v_scanline = 2;
static bool vactive_cmdlist_posted = false;

void __scratch_x("") dma_irq_handler() {
  uint32_t ch_num = dma_pong ? DMACH_PONG : DMACH_PING;
  dma_channel_hw_t *ch = &dma_hw->ch[ch_num];
  dma_hw->intr = 1u << ch_num;
  dma_pong = !dma_pong;

  bool vsync_active = (v_scanline >= MODE_V_FRONT_PORCH &&
                       v_scanline < (MODE_V_FRONT_PORCH + MODE_V_SYNC_WIDTH));
  bool front_porch = (v_scanline < MODE_V_FRONT_PORCH);
  bool back_porch =
      (v_scanline >= MODE_V_FRONT_PORCH + MODE_V_SYNC_WIDTH &&
       v_scanline < MODE_V_FRONT_PORCH + MODE_V_SYNC_WIDTH + MODE_V_BACK_PORCH);
  bool active_video = (!vsync_active && !front_porch && !back_porch);

  // Send ACR packet every 4 scanlines in VBLANK to improve sink lock
  bool send_acr = (v_scanline >= (MODE_V_FRONT_PORCH + MODE_V_SYNC_WIDTH) &&
                   v_scanline < (MODE_V_TOTAL_LINES - MODE_V_ACTIVE_LINES) &&
                   (v_scanline % 4 == 0));

  if (vsync_active) {
    // Increment accumulator during vsync to account for all 525 lines
    audio_sample_accum += SAMPLES_PER_LINE_FP;

    if (v_scanline == MODE_V_FRONT_PORCH) {
      ch->read_addr = (uintptr_t)vblank_acr_vsync_on;
      ch->transfer_count = vblank_acr_vsync_on_len;
      video_frame_count++;
    } else {
      ch->read_addr = (uintptr_t)vblank_infoframe_vsync_on;
      ch->transfer_count = vblank_infoframe_vsync_on_len;
    }
  } else if (active_video && !vactive_cmdlist_posted) {
    audio_sample_accum += SAMPLES_PER_LINE_FP;

    // Prepare pixel-doubled line BEFORE DMA reads it (critical for glitch-free
    // output!)
    uint32_t active_line =
        v_scanline - (MODE_V_TOTAL_LINES - MODE_V_ACTIVE_LINES);
    uint32_t fb_line = active_line / 2; // Vertical: each line shown twice
    const uint16_t *src = &framebuf[fb_line * FRAMEBUF_WIDTH];
    uint32_t *dst32 = (uint32_t *)line_buffer;

    // Normal 2x pixel doubling (320→640 horizontal, 240→480 vertical via
    // fb_line/2)
    for (uint32_t i = 0; i < FRAMEBUF_WIDTH; i++) {
      uint32_t p = src[i];
      dst32[i] = p | (p << 16);
    }

    if (audio_sample_accum >= (4 << 16) && audio_ring_tail != audio_ring_head) {
      audio_sample_accum -= (4 << 16);
      uint32_t *buf = dma_pong ? vactive_di_ping : vactive_di_pong;
      const uint32_t *di_words = audio_ring_buffer[audio_ring_tail].words;
      vactive_di_len = build_line_with_di(buf, di_words, false, true);
      audio_ring_tail = (audio_ring_tail + 1) % AUDIO_RING_BUFFER_SIZE;

      ch->read_addr = (uintptr_t)buf;
      ch->transfer_count = vactive_di_len;
    } else {
      // No audio sample available, send NULL Data Island to maintain HDMI sync
      ch->read_addr = (uintptr_t)vactive_di_null;
      ch->transfer_count = vactive_di_null_len;
    }
    vactive_cmdlist_posted = true;
  } else if (active_video && vactive_cmdlist_posted) {
    // Data phase: DMA reads the line_buffer we prepared earlier
    ch->read_addr = (uintptr_t)line_buffer;
    ch->transfer_count = (MODE_H_ACTIVE_PIXELS * sizeof(uint16_t)) /
                         sizeof(uint32_t); // RGB565: 640 pixels * 2 bytes / 4
    vactive_cmdlist_posted = false;
  } else {
    audio_sample_accum += SAMPLES_PER_LINE_FP;

    if (send_acr) {
      ch->read_addr = (uintptr_t)vblank_acr_vsync_off;
      ch->transfer_count = vblank_acr_vsync_off_len;
    } else if (v_scanline == 0) {
      ch->read_addr = (uintptr_t)vblank_avi_infoframe;
      ch->transfer_count = vblank_avi_infoframe_len;
    } else if (v_scanline != 0 && audio_sample_accum >= (4 << 16) &&
               audio_ring_tail != audio_ring_head) {
      audio_sample_accum -= (4 << 16);
      uint32_t *buf = dma_pong ? vblank_di_ping : vblank_di_pong;
      const uint32_t *di_words = audio_ring_buffer[audio_ring_tail].words;
      vblank_di_len = build_line_with_di(buf, di_words, false, false);
      audio_ring_tail = (audio_ring_tail + 1) % AUDIO_RING_BUFFER_SIZE;

      ch->read_addr = (uintptr_t)buf;
      ch->transfer_count = vblank_di_len;
    } else {
      // No audio sample available, send NULL Data Island to maintain HDMI sync
      ch->read_addr = (uintptr_t)vblank_di_null;
      ch->transfer_count = vblank_di_null_len;
    }
  }

  if (!vactive_cmdlist_posted) {
    v_scanline = (v_scanline + 1) % MODE_V_TOTAL_LINES;
  }
}

// ============================================================================
// Core 1: HSTX Output (runs independently on Core 1)
// ============================================================================

static void core1_entry(void) {
  // Configure HSTX TMDS encoder for RGB565
  // Red [15:11] = 5 bits, rotate by 8 to align with [7:3]
  // Green [10:5] = 6 bits, rotate by 3 to align with [7:2]
  // Blue [4:0] = 5 bits, rotate by 13 to align with [7:3]
  hstx_ctrl_hw->expand_tmds =
      4 << HSTX_CTRL_EXPAND_TMDS_L2_NBITS_LSB | // Red: 5 bits
      8 << HSTX_CTRL_EXPAND_TMDS_L2_ROT_LSB |
      5 << HSTX_CTRL_EXPAND_TMDS_L1_NBITS_LSB | // Green: 6 bits
      3 << HSTX_CTRL_EXPAND_TMDS_L1_ROT_LSB |
      4 << HSTX_CTRL_EXPAND_TMDS_L0_NBITS_LSB | // Blue: 5 bits
      13 << HSTX_CTRL_EXPAND_TMDS_L0_ROT_LSB;

  // RGB565: 2 shifts × 16 bits = 32 bits per word = 2 pixels
  hstx_ctrl_hw->expand_shift =
      2 << HSTX_CTRL_EXPAND_SHIFT_ENC_N_SHIFTS_LSB |
      16 << HSTX_CTRL_EXPAND_SHIFT_ENC_SHIFT_LSB | // 16-bit pixels
      1 << HSTX_CTRL_EXPAND_SHIFT_RAW_N_SHIFTS_LSB |
      0 << HSTX_CTRL_EXPAND_SHIFT_RAW_SHIFT_LSB;

  hstx_ctrl_hw->csr = 0;
  hstx_ctrl_hw->csr = HSTX_CTRL_CSR_EXPAND_EN_BITS |
                      5u << HSTX_CTRL_CSR_CLKDIV_LSB |
                      5u << HSTX_CTRL_CSR_N_SHIFTS_LSB |
                      2u << HSTX_CTRL_CSR_SHIFT_LSB | HSTX_CTRL_CSR_EN_BITS;

  // Spotpear pinout
  hstx_ctrl_hw->bit[0] = HSTX_CTRL_BIT0_CLK_BITS | HSTX_CTRL_BIT0_INV_BITS;
  hstx_ctrl_hw->bit[1] = HSTX_CTRL_BIT0_CLK_BITS;

  for (uint lane = 0; lane < 3; ++lane) {
    int bit = 2 + lane * 2;
    uint32_t lane_data_sel_bits = (lane * 10) << HSTX_CTRL_BIT0_SEL_P_LSB |
                                  (lane * 10 + 1) << HSTX_CTRL_BIT0_SEL_N_LSB;
    hstx_ctrl_hw->bit[bit] = lane_data_sel_bits | HSTX_CTRL_BIT0_INV_BITS;
    hstx_ctrl_hw->bit[bit + 1] = lane_data_sel_bits;
  }

  for (int i = 12; i <= 19; ++i) {
    gpio_set_function(i, 0);
  }

  // DMA setup
  dma_channel_config c;
  c = dma_channel_get_default_config(DMACH_PING);
  channel_config_set_chain_to(&c, DMACH_PONG);
  channel_config_set_dreq(&c, DREQ_HSTX);
  dma_channel_configure(DMACH_PING, &c, &hstx_fifo_hw->fifo,
                        vblank_line_vsync_off, count_of(vblank_line_vsync_off),
                        false);

  c = dma_channel_get_default_config(DMACH_PONG);
  channel_config_set_chain_to(&c, DMACH_PING);
  channel_config_set_dreq(&c, DREQ_HSTX);
  dma_channel_configure(DMACH_PONG, &c, &hstx_fifo_hw->fifo,
                        vblank_line_vsync_off, count_of(vblank_line_vsync_off),
                        false);

  dma_hw->ints0 = (1u << DMACH_PING) | (1u << DMACH_PONG);
  dma_hw->inte0 = (1u << DMACH_PING) | (1u << DMACH_PONG);
  irq_set_exclusive_handler(DMA_IRQ_0, dma_irq_handler);
  irq_set_enabled(DMA_IRQ_0, true);

  bus_ctrl_hw->priority =
      BUSCTRL_BUS_PRIORITY_DMA_W_BITS | BUSCTRL_BUS_PRIORITY_DMA_R_BITS;

  dma_channel_start(DMACH_PING);

  // Core 1 main loop: handle audio ring buffer
  while (1) {
#if USE_MVS_AUDIO
    // Poll hardware and process samples.
    // Draining the capture ring frequently on Core 1 to avoid jitter on Core 0.
    audio_pipeline_process(&audio_pipeline, audio_output_callback, NULL);
    while (ap_ring_available(&audio_pipeline.capture_ring) > 0) {
      audio_pipeline_process(&audio_pipeline, audio_output_callback, NULL);
    }
#endif
    tight_loop_contents();
  }
}

// ============================================================================
// Main (Core 0)
// ============================================================================

int main(void) {
  // Set system clock to 126 MHz for exact 25.2 MHz pixel clock
  // Pixel clock = System clock / CLKDIV = 126 MHz / 5 = 25.2 MHz
  // This matches PicoDVI's 25.2 MHz pixel clock (though they use 252 MHz with
  // PIO)
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
  init_background(); // Pre-calculate rainbow gradient
  init_sine_table();
  init_audio_packets();

  vblank_di_len = build_line_with_di(
      vblank_di_ping, hstx_get_null_data_island(false, true), false, false);
  memcpy(vblank_di_pong, vblank_di_ping, sizeof(vblank_di_ping));

  // Claim DMA channels for HSTX FIRST (channels 0 and 1)
  // Audio will get channel 2+ via dma_claim_unused_channel()
  dma_channel_claim(DMACH_PING);
  dma_channel_claim(DMACH_PONG);

#if USE_MVS_AUDIO
  // Initialize audio pipeline for MVS I2S capture
  // Must be AFTER HSTX DMA claim to avoid channel conflict
  printf("Initializing audio pipeline...\n");
  audio_pipeline_config_t audio_config = {
      .pin_bck = I2S_BCK_PIN,
      .pin_dat = I2S_DAT_PIN,
      .pin_ws = I2S_WS_PIN,
      .pin_btn1 = 21, // Not used in hstx_lab
      .pin_btn2 = 23, // Not used in hstx_lab
      .pio = pio2, // Use PIO2 for audio (per AGENTS.md: PIO1=video, PIO2=audio)
      .sm = 0};
  if (!audio_pipeline_init(&audio_pipeline, &audio_config)) {
    printf("ERROR: Failed to init audio pipeline!\n");
  } else {
    audio_pipeline_start(&audio_pipeline);
    printf("Audio pipeline started (I2S: BCK=GP%d, WS=GP%d, DAT=GP%d)\n",
           I2S_BCK_PIN, I2S_WS_PIN, I2S_DAT_PIN);
    printf("  Using PIO%d SM%d, DMA ready\n",
           audio_config.pio == pio0 ? 0 : (audio_config.pio == pio1 ? 1 : 2),
           audio_config.sm);
  }
  stdio_flush();
#endif

  // Launch Core 1 for HSTX output first
  printf("Launching Core 1 for HSTX...\n");
  multicore_launch_core1(core1_entry);
  sleep_ms(100);
  printf("Core 1 running.\n\n");

  // Initialize video capture
  printf("Initializing video capture...\n");
  video_capture_init(framebuf, FRAMEBUF_WIDTH, FRAMEBUF_HEIGHT, 224);
  printf("Starting continuous capture...\n");

  // Main loop: Core 0 captures frames, Core 1 displays them
  // No double-buffering needed - single framebuffer works without visible
  // tearing
  uint32_t led_toggle_frame = 0;
  bool led_state = false;
  uint32_t debug_frame = 0;

  while (1) {
    if (!video_capture_frame()) {
#if USE_MVS_AUDIO
      // Force audio reset on video sync loss
      audio_pipeline_stop(&audio_pipeline);
      audio_pipeline_start(&audio_pipeline);
#endif
    }

    if (video_frame_count % 60 == 0 && video_frame_count != led_toggle_frame) {
        printf("Frame %lu... \n", video_frame_count);
        stdio_flush();
    }

    // LED heartbeat (toggles every 0.5s based on display frame count)
    if (video_frame_count >= led_toggle_frame + 30) {
      led_state = !led_state;
      gpio_put(PICO_DEFAULT_LED_PIN, led_state);
      led_toggle_frame = video_frame_count;

#if USE_MVS_AUDIO
      // Debug: print audio stats every 1s
      if (++debug_frame % 2 == 0) {
        audio_pipeline_status_t status;
        audio_pipeline_get_status(&audio_pipeline, &status);
        printf("cap=%lu rate=%lu out=%lu | ring h=%lu t=%lu\n",
               status.samples_captured, status.capture_sample_rate,
               audio_samples_received, audio_ring_head, audio_ring_tail);
      }
#endif
    }
  }

  return 0;
}
