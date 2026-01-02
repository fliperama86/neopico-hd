/**
 * HSTX Audio Test - Video + HDMI Audio via Data Islands
 *
 * Copied from neopico-hd/hstx_audio_test.c (produces popcorning audio)
 * Outputs 640x480 @ 60Hz with HDMI audio (48kHz stereo).
 *
 * HSTX Pin Assignment (GPIO 12-19) - Custom Spotpear wiring:
 *     GPIO 12: CLKN    GPIO 13: CLKP
 *     GPIO 14: D0N     GPIO 15: D0P  (Lane 0 - Blue)
 *     GPIO 16: D1N     GPIO 17: D1P  (Lane 1 - Green)
 *     GPIO 18: D2N     GPIO 19: D2P  (Lane 2 - Red)
 *
 * Audio implementation:
 * - Data Islands inserted during hsync pulse in vblank
 * - ACR packet sent once per frame
 * - Audio InfoFrame sent once per frame
 *
 * Target: RP2350A (Pico 2)
 */

#include "hardware/clocks.h"
#include "hardware/dma.h"
#include "hardware/gpio.h"
#include "hardware/irq.h"
#include "hardware/structs/bus_ctrl.h"
#include "hardware/structs/hstx_ctrl.h"
#include "hardware/structs/hstx_fifo.h"
#include "pico/multicore.h"
#include "data_packet.h"
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
#define PREAMBLE_V0_H0 (TMDS_CTRL_00 | (TMDS_CTRL_01 << 10) | (TMDS_CTRL_01 << 20))
#define PREAMBLE_V1_H0 (TMDS_CTRL_10 | (TMDS_CTRL_01 << 10) | (TMDS_CTRL_01 << 20))

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

#define MODE_H_TOTAL_PIXELS \
  (MODE_H_FRONT_PORCH + MODE_H_SYNC_WIDTH + MODE_H_BACK_PORCH + MODE_H_ACTIVE_PIXELS)
#define MODE_V_TOTAL_LINES \
  (MODE_V_FRONT_PORCH + MODE_V_SYNC_WIDTH + MODE_V_BACK_PORCH + MODE_V_ACTIVE_LINES)

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

// Audio generation
#define TONE_FREQUENCY 440  // A4 note (440 Hz)
#define TONE_AMPLITUDE 8000 // ~25% of int16 max

// Audio state
static volatile uint32_t video_frame_count = 0;  // volatile: updated on Core 1, read on Core 0
static int audio_frame_counter = 0; // IEC60958 frame counter (0-191)
static uint32_t audio_phase = 0x40000000; // Start at 1/4 cycle (90 degrees)

// Error detection and LED heartbeat
static volatile bool error_detected = false;
static const char *error_message = NULL;

#define ASSERT_NO_ERROR(condition, msg) \
  do { \
    if (!(condition)) { \
      error_detected = true; \
      error_message = msg; \
      printf("ERROR: %s\n", msg); \
      while (1) { \
        gpio_put(PICO_DEFAULT_LED_PIN, 1); \
        tight_loop_contents(); \
      } \
    } \
  } while (0)
static uint32_t audio_sample_accum = 0; // Fixed-point accumulator

// Pre-computed sine table for fast audio generation
#define SINE_TABLE_SIZE 256
static int16_t sine_table[SINE_TABLE_SIZE];

// Fast inline sine lookup for background task
static inline int16_t fast_sine_sample(void) {
  int16_t s = sine_table[(audio_phase >> 24) & 0xFF];
  audio_phase += (uint32_t)(((uint64_t)TONE_FREQUENCY << 32) / AUDIO_SAMPLE_RATE);
  return s;
}

// Audio ring buffer for pre-encoded data islands
// Increased from 64 to reduce underruns (64 = 5ms, 256 = 21ms buffering)
#define AUDIO_RING_BUFFER_SIZE 256
static hstx_data_island_t audio_ring_buffer[AUDIO_RING_BUFFER_SIZE];
static volatile uint32_t audio_ring_head = 0;
static volatile uint32_t audio_ring_tail = 0;

// Audio timer for consistent sample generation (matching PicoDVI)
static struct repeating_timer audio_timer;

// Audio samples per frame (48kHz / 60fps = 800 samples)
#define SAMPLES_PER_FRAME (AUDIO_SAMPLE_RATE / 60)
#define LINES_PER_FRAME (MODE_V_TOTAL_LINES)

// Fixed-point accumulator for fractional samples per line (16.16 format)
#define SAMPLES_PER_LINE_FP ((SAMPLES_PER_FRAME << 16) / LINES_PER_FRAME)

// Set to 1 to enable audio during active video lines
#define ENABLE_ACTIVE_LINE_AUDIO 1

// Timer-based audio generation (matching PicoDVI approach)
// Called from timer interrupt at consistent 2ms intervals
static bool audio_timer_callback(struct repeating_timer *t) {
  (void)t;

  // Generate audio packets to fill ring buffer
  // Don't overfill - leave at least 16 slots free for safety
  int free_slots = (audio_ring_tail - audio_ring_head - 1 + AUDIO_RING_BUFFER_SIZE) % AUDIO_RING_BUFFER_SIZE;
  if (free_slots < 16) {
    return true;  // Buffer nearly full, skip this iteration
  }

  // Generate one packet (4 samples) per timer tick
  audio_sample_t samples[4];
  for (int i = 0; i < 4; i++) {
    int16_t s = fast_sine_sample();
    samples[i].left = s;
    samples[i].right = s;
  }

  data_packet_t packet;
  audio_frame_counter =
      packet_set_audio_samples(&packet, samples, 4, audio_frame_counter);

  hstx_encode_data_island(&audio_ring_buffer[audio_ring_head], &packet, true, false);
  audio_ring_head = (audio_ring_head + 1) % AUDIO_RING_BUFFER_SIZE;

  return true;
}

// Legacy function kept for initialization
static void update_audio_ring_buffer(void) {
  while (((audio_ring_head + 1) % AUDIO_RING_BUFFER_SIZE) != audio_ring_tail) {
    audio_sample_t samples[4];
    for (int i = 0; i < 4; i++) {
      int16_t s = fast_sine_sample();
      samples[i].left = s;
      samples[i].right = s;
    }

    data_packet_t packet;
    audio_frame_counter =
        packet_set_audio_samples(&packet, samples, 4, audio_frame_counter);

    hstx_encode_data_island(&audio_ring_buffer[audio_ring_head], &packet, true, false);
    audio_ring_head = (audio_ring_head + 1) % AUDIO_RING_BUFFER_SIZE;
  }
}

// ============================================================================
// Framebuffer (320×240 MVS native, 2×2 doubled to 640×480, RGB565)
// ============================================================================

static uint16_t framebuf[FRAMEBUF_HEIGHT * FRAMEBUF_WIDTH] __attribute__((aligned(4)));

// Line buffer for horizontal pixel doubling (320 → 640 pixels, RGB565)
// Aligned for fast DMA access
static uint16_t line_buffer[MODE_H_ACTIVE_PIXELS] __attribute__((aligned(4)));

static inline uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b) {
  return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | ((b & 0xF8) >> 3);
}

// Bouncing box state (proper 2×2 scaling, no hacks needed!)
#define BOX_SIZE 64
static int box_x = 50, box_y = 50;
static int box_x_old = 50, box_y_old = 50;  // Track previous position
static int box_dx = 2, box_dy = 1;

// Background buffer (pre-calculated gradient, RGB565)
static uint16_t background[FRAMEBUF_HEIGHT * FRAMEBUF_WIDTH];

// Pre-calculate rainbow gradient background (called once at init)
static void init_background(void) {
  for (int y = 0; y < FRAMEBUF_HEIGHT; y++) {
    for (int x = 0; x < FRAMEBUF_WIDTH; x++) {
      // Rainbow gradient - horizontal across 320 pixels
      int segment = x / 53;  // 6 segments of ~53 pixels each
      int t = (x % 53) * 255 / 53;  // Position within segment (0-255)

      uint8_t r, g, b;
      switch (segment) {
        case 0:  // Red → Yellow
          r = 255; g = t; b = 0;
          break;
        case 1:  // Yellow → Green
          r = 255 - t; g = 255; b = 0;
          break;
        case 2:  // Green → Cyan
          r = 0; g = 255; b = t;
          break;
        case 3:  // Cyan → Blue
          r = 0; g = 255 - t; b = 255;
          break;
        case 4:  // Blue → Magenta
          r = t; g = 0; b = 255;
          break;
        default: // Magenta → Red
          r = 255; g = 0; b = 255 - t;
          break;
      }
      background[y * FRAMEBUF_WIDTH + x] = rgb565(r, g, b);
    }
  }
  // Copy background to framebuf initially
  memcpy(framebuf, background, sizeof(framebuf));
}

static void draw_frame(void) {
  uint16_t box_color = rgb565(0, 0, 0);  // Black box

  // Restore old box area from background
  for (int y = 0; y < BOX_SIZE; y++) {
    for (int x = 0; x < BOX_SIZE; x++) {
      int fx = box_x_old + x;
      int fy = box_y_old + y;
      framebuf[fy * FRAMEBUF_WIDTH + fx] = background[fy * FRAMEBUF_WIDTH + fx];
    }
  }

  // Draw box at new position
  for (int y = 0; y < BOX_SIZE; y++) {
    for (int x = 0; x < BOX_SIZE; x++) {
      int fx = box_x + x;
      int fy = box_y + y;
      framebuf[fy * FRAMEBUF_WIDTH + fx] = box_color;
    }
  }

  // Save current position as old
  box_x_old = box_x;
  box_y_old = box_y;
}

static void update_box(void) {
  box_x += box_dx;
  box_y += box_dy;

  // Proper boundary checking with clamping
  if (box_x < 0) {
    box_x = 0;
    box_dx = -box_dx;
  } else if (box_x + BOX_SIZE >= FRAMEBUF_WIDTH) {  // >= to catch at boundary
    box_x = FRAMEBUF_WIDTH - BOX_SIZE;
    box_dx = -box_dx;
  }

  if (box_y < 0) {
    box_y = 0;
    box_dy = -box_dy;
  } else if (box_y + BOX_SIZE >= FRAMEBUF_HEIGHT) {  // >= to catch at boundary
    box_y = FRAMEBUF_HEIGHT - BOX_SIZE;
    box_dy = -box_dy;
  }
}

// ============================================================================
// Command Lists (pre-computed at init time)
// ============================================================================

static uint32_t vblank_line_vsync_off[] = {
    HSTX_CMD_RAW_REPEAT | MODE_H_FRONT_PORCH,
    SYNC_V1_H1,
    HSTX_CMD_RAW_REPEAT | MODE_H_SYNC_WIDTH,
    SYNC_V1_H0,
    HSTX_CMD_RAW_REPEAT | (MODE_H_BACK_PORCH + MODE_H_ACTIVE_PIXELS),
    SYNC_V1_H1,
    HSTX_CMD_NOP};

static uint32_t vblank_line_vsync_on[] = {
    HSTX_CMD_RAW_REPEAT | MODE_H_FRONT_PORCH,
    SYNC_V0_H1,
    HSTX_CMD_RAW_REPEAT | MODE_H_SYNC_WIDTH,
    SYNC_V0_H0,
    HSTX_CMD_RAW_REPEAT | (MODE_H_BACK_PORCH + MODE_H_ACTIVE_PIXELS),
    SYNC_V0_H1,
    HSTX_CMD_NOP};

static uint32_t vactive_line[] = {
    HSTX_CMD_RAW_REPEAT | MODE_H_FRONT_PORCH,
    SYNC_V1_H1,
    HSTX_CMD_NOP,
    HSTX_CMD_RAW_REPEAT | MODE_H_SYNC_WIDTH,
    SYNC_V1_H0,
    HSTX_CMD_NOP,
    HSTX_CMD_RAW_REPEAT | MODE_H_BACK_PORCH,
    SYNC_V1_H1,
    HSTX_CMD_TMDS | MODE_H_ACTIVE_PIXELS};

// Data island placement: At START of hsync pulse (matching pico_lib)
#define SYNC_BEFORE_DI 0  // DI starts immediately at hsync transition
#define SYNC_AFTER_DI (MODE_H_SYNC_WIDTH - W_PREAMBLE - W_DATA_ISLAND)  // Remaining hsync after DI

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

static uint32_t vblank_di_ping[VACTIVE_DI_MAX_WORDS];
static uint32_t vblank_di_pong[VACTIVE_DI_MAX_WORDS];
static uint32_t vblank_di_len;

static void init_sine_table(void) {
  for (int i = 0; i < SINE_TABLE_SIZE; i++) {
    float angle = (float)i * 2.0f * 3.14159265f / SINE_TABLE_SIZE;
    sine_table[i] = (int16_t)(sinf(angle) * TONE_AMPLITUDE);
  }
}

static uint32_t build_line_with_di(uint32_t *buf, const uint32_t *di_words,
                                   bool vsync, bool active) {
  uint32_t *p = buf;

  uint32_t sync_h0 = vsync ? SYNC_V0_H0 : SYNC_V1_H0;
  uint32_t sync_h1 = vsync ? SYNC_V0_H1 : SYNC_V1_H1;
  uint32_t preamble = vsync ? PREAMBLE_V0_H0 : PREAMBLE_V1_H0;

  // Front porch (16 clocks, H=1)
  *p++ = HSTX_CMD_RAW_REPEAT | MODE_H_FRONT_PORCH;
  *p++ = sync_h1;
  *p++ = HSTX_CMD_NOP;

  // Data Island preamble (8 clocks) - at start of hsync
  *p++ = HSTX_CMD_RAW_REPEAT | W_PREAMBLE;
  *p++ = preamble;
  *p++ = HSTX_CMD_NOP;

  // Data Island (36 clocks)
  *p++ = HSTX_CMD_RAW | W_DATA_ISLAND;
  for (int i = 0; i < W_DATA_ISLAND; i++) {
    *p++ = di_words[i];
  }

  // Remaining hsync after DI (52 clocks, H=0)
  *p++ = HSTX_CMD_RAW_REPEAT | SYNC_AFTER_DI;
  *p++ = sync_h0;
  *p++ = HSTX_CMD_NOP;

  // Back porch and active
  if (active) {
    *p++ = HSTX_CMD_RAW_REPEAT | MODE_H_BACK_PORCH;
    *p++ = sync_h1;
    *p++ = HSTX_CMD_NOP;
    *p++ = HSTX_CMD_TMDS | MODE_H_ACTIVE_PIXELS;
  } else {
    *p++ = HSTX_CMD_RAW_REPEAT | (MODE_H_BACK_PORCH + MODE_H_ACTIVE_PIXELS);
    *p++ = sync_h1;
    *p++ = HSTX_CMD_NOP;
  }

  return (uint32_t)(p - buf);
}

static uint32_t build_vblank_with_di(uint32_t *buf, const uint32_t *di_words, bool vsync) {
  return build_line_with_di(buf, di_words, vsync, false);
}

static void init_audio_packets(void) {
  data_packet_t packet;
  hstx_data_island_t island;

  packet_set_acr(&packet, AUDIO_N_VALUE, AUDIO_CTS_VALUE);
  hstx_encode_data_island(&island, &packet, false, false);
  vblank_acr_vsync_on_len = build_vblank_with_di(vblank_acr_vsync_on, island.words, true);

  packet_set_audio_infoframe(&packet, AUDIO_SAMPLE_RATE, 2, 16);
  hstx_encode_data_island(&island, &packet, false, false);
  vblank_infoframe_vsync_on_len = build_vblank_with_di(vblank_infoframe_vsync_on, island.words, true);

  packet_set_avi_infoframe(&packet, 1);
  hstx_encode_data_island(&island, &packet, true, false);
  vblank_avi_infoframe_len = build_vblank_with_di(vblank_avi_infoframe, island.words, false);

  update_audio_ring_buffer();

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
  bool back_porch = (v_scanline >= MODE_V_FRONT_PORCH + MODE_V_SYNC_WIDTH &&
                     v_scanline < MODE_V_FRONT_PORCH + MODE_V_SYNC_WIDTH + MODE_V_BACK_PORCH);
  bool active_video = (!vsync_active && !front_porch && !back_porch);

  if (vsync_active) {
    // Increment accumulator during vsync to account for all 525 lines
    audio_sample_accum += SAMPLES_PER_LINE_FP;

    if (v_scanline == MODE_V_FRONT_PORCH) {
      ch->read_addr = (uintptr_t)vblank_acr_vsync_on;
      ch->transfer_count = vblank_acr_vsync_on_len;
      video_frame_count++;
      // Removed: audio_sample_accum = 0;
      // Allow accumulator to carry over between frames for perfect 800 samples/frame delivery
    } else {
      ch->read_addr = (uintptr_t)vblank_infoframe_vsync_on;
      ch->transfer_count = vblank_infoframe_vsync_on_len;
    }
  } else if (active_video && !vactive_cmdlist_posted) {
    audio_sample_accum += SAMPLES_PER_LINE_FP;

    // Prepare pixel-doubled line BEFORE DMA reads it (critical for glitch-free output!)
    uint32_t active_line = v_scanline - (MODE_V_TOTAL_LINES - MODE_V_ACTIVE_LINES);
    uint32_t fb_line = active_line / 2;  // Vertical: each line shown twice
    const uint16_t *src = &framebuf[fb_line * FRAMEBUF_WIDTH];
    uint32_t *dst32 = (uint32_t *)line_buffer;

    // Normal 2x pixel doubling (320→640 horizontal, 240→480 vertical via fb_line/2)
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
      ch->read_addr = (uintptr_t)vactive_line;
      ch->transfer_count = count_of(vactive_line);
    }
    vactive_cmdlist_posted = true;
  } else if (active_video && vactive_cmdlist_posted) {
    // Data phase: DMA reads the line_buffer we prepared earlier
    ch->read_addr = (uintptr_t)line_buffer;
    ch->transfer_count = (MODE_H_ACTIVE_PIXELS * sizeof(uint16_t)) / sizeof(uint32_t);  // RGB565: 640 pixels * 2 bytes / 4
    vactive_cmdlist_posted = false;
  } else {
    audio_sample_accum += SAMPLES_PER_LINE_FP;

    if (v_scanline != 0 && audio_sample_accum >= (4 << 16) &&
        audio_ring_tail != audio_ring_head) {
      audio_sample_accum -= (4 << 16);
      uint32_t *buf = dma_pong ? vblank_di_ping : vblank_di_pong;
      const uint32_t *di_words = audio_ring_buffer[audio_ring_tail].words;
      vblank_di_len = build_line_with_di(buf, di_words, false, false);
      audio_ring_tail = (audio_ring_tail + 1) % AUDIO_RING_BUFFER_SIZE;

      ch->read_addr = (uintptr_t)buf;
      ch->transfer_count = vblank_di_len;
    } else if (v_scanline == 0) {
      ch->read_addr = (uintptr_t)vblank_avi_infoframe;
      ch->transfer_count = vblank_avi_infoframe_len;
    } else {
      ch->read_addr = (uintptr_t)vblank_line_vsync_off;
      ch->transfer_count = count_of(vblank_line_vsync_off);
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
  hstx_ctrl_hw->expand_tmds = 4 << HSTX_CTRL_EXPAND_TMDS_L2_NBITS_LSB |  // Red: 5 bits
                              8 << HSTX_CTRL_EXPAND_TMDS_L2_ROT_LSB |
                              5 << HSTX_CTRL_EXPAND_TMDS_L1_NBITS_LSB |  // Green: 6 bits
                              3 << HSTX_CTRL_EXPAND_TMDS_L1_ROT_LSB |
                              4 << HSTX_CTRL_EXPAND_TMDS_L0_NBITS_LSB |  // Blue: 5 bits
                              13 << HSTX_CTRL_EXPAND_TMDS_L0_ROT_LSB;

  // RGB565: 2 shifts × 16 bits = 32 bits per word = 2 pixels
  hstx_ctrl_hw->expand_shift = 2 << HSTX_CTRL_EXPAND_SHIFT_ENC_N_SHIFTS_LSB |
                               16 << HSTX_CTRL_EXPAND_SHIFT_ENC_SHIFT_LSB |  // 16-bit pixels
                               1 << HSTX_CTRL_EXPAND_SHIFT_RAW_N_SHIFTS_LSB |
                               0 << HSTX_CTRL_EXPAND_SHIFT_RAW_SHIFT_LSB;

  hstx_ctrl_hw->csr = 0;
  hstx_ctrl_hw->csr =
      HSTX_CTRL_CSR_EXPAND_EN_BITS |
      5u << HSTX_CTRL_CSR_CLKDIV_LSB |
      5u << HSTX_CTRL_CSR_N_SHIFTS_LSB |
      2u << HSTX_CTRL_CSR_SHIFT_LSB |
      HSTX_CTRL_CSR_EN_BITS;

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
                        vblank_line_vsync_off, count_of(vblank_line_vsync_off), false);

  c = dma_channel_get_default_config(DMACH_PONG);
  channel_config_set_chain_to(&c, DMACH_PING);
  channel_config_set_dreq(&c, DREQ_HSTX);
  dma_channel_configure(DMACH_PONG, &c, &hstx_fifo_hw->fifo,
                        vblank_line_vsync_off, count_of(vblank_line_vsync_off), false);

  dma_hw->ints0 = (1u << DMACH_PING) | (1u << DMACH_PONG);
  dma_hw->inte0 = (1u << DMACH_PING) | (1u << DMACH_PONG);
  irq_set_exclusive_handler(DMA_IRQ_0, dma_irq_handler);
  irq_set_enabled(DMA_IRQ_0, true);

  bus_ctrl_hw->priority = BUSCTRL_BUS_PRIORITY_DMA_W_BITS | BUSCTRL_BUS_PRIORITY_DMA_R_BITS;

  dma_channel_start(DMACH_PING);

  // Core 1 main loop: handle audio ring buffer
  // Framebuffer updates now happen on Core 0
  while (1) {
    update_audio_ring_buffer();
    tight_loop_contents();
  }
}

// ============================================================================
// Main (Core 0)
// ============================================================================

int main(void) {
  // Set system clock to 126 MHz for exact 25.2 MHz pixel clock
  // Pixel clock = System clock / CLKDIV = 126 MHz / 5 = 25.2 MHz
  // This matches PicoDVI's 25.2 MHz pixel clock (though they use 252 MHz with PIO)
  set_sys_clock_khz(126000, true);

  stdio_init_all();

  // Initialize LED for heartbeat
  gpio_init(PICO_DEFAULT_LED_PIN);
  gpio_set_dir(PICO_DEFAULT_LED_PIN, GPIO_OUT);

  sleep_ms(1000);

  // Flush any pending output
  stdio_flush();

  printf("\n\n");
  printf("================================\n");
  printf("HSTX Lab - Multicore Edition\n");
  printf("================================\n");
  printf("Core 0: Available for capture\n");
  printf("Core 1: HSTX output (640x480)\n\n");
  stdio_flush();

  // Initialize shared resources before launching Core 1
  init_background();  // Pre-calculate rainbow gradient
  init_sine_table();
  init_audio_packets();

  vblank_di_len = build_line_with_di(
      vblank_di_ping, hstx_get_null_data_island(true, false), false, false);
  memcpy(vblank_di_pong, vblank_di_ping, sizeof(vblank_di_ping));

  // Claim DMA channels for HSTX before launching Core 1
  dma_channel_claim(DMACH_PING);
  dma_channel_claim(DMACH_PONG);

  // Launch Core 1 for HSTX output first
  printf("Launching Core 1 for HSTX...\n");
  multicore_launch_core1(core1_entry);
  sleep_ms(100);
  printf("Core 1 running.\n\n");

  // Initialize video capture
  printf("Initializing video capture...\n");
  video_capture_init(framebuf, FRAMEBUF_WIDTH, FRAMEBUF_HEIGHT, 224);
  printf("Starting continuous capture...\n");

  // Continuous capture loop - Core 0 captures, Core 1 displays
  uint32_t led_toggle_frame = 0;
  bool led_state = false;
  uint32_t capture_count = 0;

  while (1) {
    // Capture next frame
    if (video_capture_frame()) {
      capture_count++;
    }

    // LED heartbeat at 2Hz
    if (video_frame_count >= led_toggle_frame + 30) {
      led_state = !led_state;
      gpio_put(PICO_DEFAULT_LED_PIN, led_state);
      led_toggle_frame = video_frame_count;
    }
  }

  return 0;
}
