/**
 * HSTX Audio Test - Video + HDMI Audio via Data Islands
 *
 * Based on pico-examples/hstx/dvi_out_hstx_encoder with audio additions.
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

#include "hardware/dma.h"
#include "hardware/gpio.h"
#include "hardware/irq.h"
#include "hardware/structs/bus_ctrl.h"
#include "hardware/structs/hstx_ctrl.h"
#include "hardware/structs/hstx_fifo.h"
#include "hstx_audio/data_packet.h"
#include "pico/stdlib.h"
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

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

// 480p timing
#define MODE_H_FRONT_PORCH 16
#define MODE_H_SYNC_WIDTH 96
#define MODE_H_BACK_PORCH 48
#define MODE_H_ACTIVE_PIXELS 640

#define MODE_V_FRONT_PORCH 10
#define MODE_V_SYNC_WIDTH 2
#define MODE_V_BACK_PORCH 33
#define MODE_V_ACTIVE_LINES 480

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
#define AUDIO_CTS_VALUE 30000 // CTS for 30MHz pixel clock

// Audio generation
#define TONE_FREQUENCY 440  // A4 note (440 Hz)
#define TONE_AMPLITUDE 8000 // ~25% of int16 max

// Audio state
static uint32_t video_frame_count = 0;
static int audio_frame_counter = 0; // IEC60958 frame counter (0-191)
static uint32_t audio_phase =
    0x40000000; // Start at 1/4 cycle (90 degrees) so first sample isn't zero
static uint32_t audio_sample_accum =
    0; // Fixed-point accumulator for samples per line

// Pre-computed sine table for fast audio generation
#define SINE_TABLE_SIZE 256
static int16_t sine_table[SINE_TABLE_SIZE];

// Fast inline sine lookup for background task
static inline int16_t fast_sine_sample(void) {
  // Use top 8 bits of phase for table lookup
  int16_t s = sine_table[(audio_phase >> 24) & 0xFF];
  // Advance phase
  audio_phase +=
      (uint32_t)(((uint64_t)TONE_FREQUENCY << 32) / AUDIO_SAMPLE_RATE);
  return s;
}

// Audio ring buffer for pre-encoded data islands
// Each island is 36 words (144 bytes). 64 entries = 9KB.
#define AUDIO_RING_BUFFER_SIZE 64
static hstx_data_island_t audio_ring_buffer[AUDIO_RING_BUFFER_SIZE];
static volatile uint32_t audio_ring_head = 0; // Written by Core 0
static volatile uint32_t audio_ring_tail = 0; // Read by IRQ

// Debug: Use a distinctive CTS pattern to trace encoding issues
// Set to 1 to use test CTS values for debugging
// Test values:
//   0x06257 = normal (25175) - bytes: 57, 62, 00 - FAILS (57→00, 62→OK, 00→07)
//   0x62626 = all 0x62       - bytes: 26, 62, 06 - TEST: does 0x62 pattern work
//   everywhere? 0x55555 = alternating    - bytes: 55, 55, 05 - TEST: different
//   pattern 0x12345 = sequential     - bytes: 45, 23, 01 - TEST: unique bytes
#define DEBUG_USE_TEST_CTS 0
#define DEBUG_TEST_CTS 0x12345 // Try sequential pattern first

// Audio samples per frame (48kHz / 60fps = 800 samples)
// We send audio on every line (525 lines) using fractional accumulation
// samples_per_line = 800/525 ≈ 1.52
#define SAMPLES_PER_FRAME (AUDIO_SAMPLE_RATE / 60) // 800
#define LINES_PER_FRAME (MODE_V_TOTAL_LINES)       // 525

// Fixed-point accumulator for fractional samples per line (16.16 format)
#define SAMPLES_PER_LINE_FP                                                    \
  ((SAMPLES_PER_FRAME << 16) / LINES_PER_FRAME) // ~99614 = 1.52 in 16.16

// Set to 1 to enable audio during active video lines
// Set to 0 to use vblank-only audio
#define ENABLE_ACTIVE_LINE_AUDIO 1

// Background task to fill the audio ring buffer
// Should be called from the main loop
static void update_audio_ring_buffer(void) {
  while (((audio_ring_head + 1) % AUDIO_RING_BUFFER_SIZE) != audio_ring_tail) {
    // Generate audio samples for one packet (always 4 samples)
    audio_sample_t samples[4];
    for (int i = 0; i < 4; i++) {
      int16_t s = fast_sine_sample();
      samples[i].left = s;
      samples[i].right = s;
    }

    data_packet_t packet;
    audio_frame_counter =
        packet_set_audio_samples(&packet, samples, 4, audio_frame_counter);

    // Encode and store in ring buffer
    // Note: active lines and most vblank lines use vsync=1 (inactive), hsync=0
    // (pulse)
    hstx_encode_data_island(&audio_ring_buffer[audio_ring_head], &packet, true,
                            false);

    audio_ring_head = (audio_ring_head + 1) % AUDIO_RING_BUFFER_SIZE;
  }
}

// ============================================================================
// Framebuffer
// ============================================================================

static uint8_t framebuf[MODE_V_ACTIVE_LINES * MODE_H_ACTIVE_PIXELS];

static inline uint8_t rgb332(uint8_t r, uint8_t g, uint8_t b) {
  return ((r & 0xe0) >> 0) | ((g & 0xe0) >> 3) | ((b & 0xc0) >> 6);
}

// Bouncing box state
#define BOX_SIZE 64
static int box_x = 100, box_y = 100;
static int box_dx = 3, box_dy = 2;

static void draw_frame(void) {
  uint8_t bg = rgb332(0, 0, 64);
  uint8_t box = rgb332(255, 255, 0);

  for (int y = 0; y < MODE_V_ACTIVE_LINES; y++) {
    for (int x = 0; x < MODE_H_ACTIVE_PIXELS; x++) {
      if (x >= box_x && x < box_x + BOX_SIZE && y >= box_y &&
          y < box_y + BOX_SIZE) {
        framebuf[y * MODE_H_ACTIVE_PIXELS + x] = box;
      } else {
        framebuf[y * MODE_H_ACTIVE_PIXELS + x] = bg;
      }
    }
  }
}

static void update_box(void) {
  box_x += box_dx;
  box_y += box_dy;

  if (box_x <= 0 || box_x + BOX_SIZE >= MODE_H_ACTIVE_PIXELS) {
    box_dx = -box_dx;
    box_x += box_dx;
  }
  if (box_y <= 0 || box_y + BOX_SIZE >= MODE_V_ACTIVE_LINES) {
    box_dy = -box_dy;
    box_y += box_dy;
  }
}

// ============================================================================
// Command Lists (pre-computed at init time)
// ============================================================================

// Standard vblank line (no data island)
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

// Active video line
static uint32_t vactive_line[] = {HSTX_CMD_RAW_REPEAT | MODE_H_FRONT_PORCH,
                                  SYNC_V1_H1,
                                  HSTX_CMD_NOP,
                                  HSTX_CMD_RAW_REPEAT | MODE_H_SYNC_WIDTH,
                                  SYNC_V1_H0,
                                  HSTX_CMD_NOP,
                                  HSTX_CMD_RAW_REPEAT | MODE_H_BACK_PORCH,
                                  SYNC_V1_H1,
                                  HSTX_CMD_TMDS | MODE_H_ACTIVE_PIXELS};

// Data island placement: DURING hsync pulse (like hsdaoh)
// Structure: front_porch(16) + sync_before_di(52) + preamble(8) + di(36) +
// back_porch+active(688) Sync_before_di = 96 - 8 - 36 = 52
#define SYNC_BEFORE_DI                                                         \
  (MODE_H_SYNC_WIDTH - W_PREAMBLE - W_DATA_ISLAND) // 96 - 8 - 36 = 52

// Pre-computed vblank lines WITH data islands
// Size: front(2) + sync_before(2) + preamble(2) + di_cmd(1) + di_data(36) +
// back+active(2) + nop(1) = 46
#define VBLANK_DI_MAX_WORDS 64
static uint32_t vblank_acr_vsync_on[VBLANK_DI_MAX_WORDS];
static uint32_t vblank_acr_vsync_on_len;
static uint32_t vblank_infoframe_vsync_on[VBLANK_DI_MAX_WORDS];
static uint32_t vblank_infoframe_vsync_on_len;
static uint32_t vblank_avi_infoframe[VBLANK_DI_MAX_WORDS];
static uint32_t vblank_avi_infoframe_len;

// Active line with data island (ping/pong buffers for DMA)
// Size: front(2) + sync_before(2) + preamble(2) + di_cmd(1) + di_data(36) +
// back(2) + active_cmd(1) = 46
#define VACTIVE_DI_MAX_WORDS 128
static uint32_t vactive_di_ping[VACTIVE_DI_MAX_WORDS];
static uint32_t vactive_di_pong[VACTIVE_DI_MAX_WORDS];
static uint32_t vactive_di_len;

// Pre-computed null lines WITH data islands (for blanking)
static uint32_t vblank_di_ping[VACTIVE_DI_MAX_WORDS];
static uint32_t vblank_di_pong[VACTIVE_DI_MAX_WORDS];
static uint32_t vblank_di_len;

// Initialize sine table
static void init_sine_table(void) {
  for (int i = 0; i < SINE_TABLE_SIZE; i++) {
    float angle = (float)i * 2.0f * 3.14159265f / SINE_TABLE_SIZE;
    sine_table[i] = (int16_t)(sinf(angle) * TONE_AMPLITUDE);
  }
}

static uint32_t build_line_with_di(uint32_t *buf, const uint32_t *di_words,
                                   bool vsync, bool active) {
  uint32_t *p = buf;

  // Sync symbols for this line
  // During vsync pulse: vsync=0 (active), During non-vsync: vsync=1 (inactive)
  // Note: V0 = vsync active, V1 = vsync inactive (confusing naming)
  uint32_t sync_h0 = vsync ? SYNC_V0_H0 : SYNC_V1_H0; // During hsync pulse
  uint32_t sync_h1 = vsync ? SYNC_V0_H1 : SYNC_V1_H1; // Outside hsync pulse
  uint32_t preamble =
      vsync ? PREAMBLE_V0_H0 : PREAMBLE_V1_H0; // During hsync, before DI

  // Front porch (16 clocks) - hsync=1
  *p++ = HSTX_CMD_RAW_REPEAT | MODE_H_FRONT_PORCH;
  *p++ = sync_h1;
  *p++ = HSTX_CMD_NOP;

  // Hsync pulse part 1: before data island (52 clocks) - hsync=0
  *p++ = HSTX_CMD_RAW_REPEAT | SYNC_BEFORE_DI;
  *p++ = sync_h0;
  *p++ = HSTX_CMD_NOP;

  // Data Island preamble (8 clocks) - hsync=0, with DI preamble pattern on
  // lanes 1&2
  *p++ = HSTX_CMD_RAW_REPEAT | W_PREAMBLE;
  *p++ = preamble;
  *p++ = HSTX_CMD_NOP;

  // Data Island (36 clocks) - hsync=0 encoded in TERC4
  *p++ = HSTX_CMD_RAW | W_DATA_ISLAND;
  for (int i = 0; i < W_DATA_ISLAND; i++) {
    *p++ = di_words[i];
  }

  if (active) {
    // Active line: back porch only, then TMDS for active pixels
    *p++ = HSTX_CMD_RAW_REPEAT | MODE_H_BACK_PORCH;
    *p++ = sync_h1;
    *p++ = HSTX_CMD_NOP;
    *p++ = HSTX_CMD_TMDS | MODE_H_ACTIVE_PIXELS;
    // No NOP - pixel data follows immediately
  } else {
    // Vblank line: back porch + active area all blanking
    *p++ = HSTX_CMD_RAW_REPEAT | (MODE_H_BACK_PORCH + MODE_H_ACTIVE_PIXELS);
    *p++ = sync_h1;
    *p++ = HSTX_CMD_NOP;
  }

  return (uint32_t)(p - buf);
}

// Wrapper for backward compatibility
static uint32_t build_vblank_with_di(uint32_t *buf, const uint32_t *di_words,
                                     bool vsync) {
  return build_line_with_di(buf, di_words, vsync, false);
}

// Initialize audio packets and pre-compute command lists
static void init_audio_packets(void) {
  data_packet_t packet;
  hstx_data_island_t island;

  uint32_t cts_value = AUDIO_CTS_VALUE;
  packet_set_acr(&packet, AUDIO_N_VALUE, cts_value);
  hstx_encode_data_island(&island, &packet, false, false); // vsync=0, hsync=0
  vblank_acr_vsync_on_len =
      build_vblank_with_di(vblank_acr_vsync_on, island.words, true);

  // Audio InfoFrame
  packet_set_audio_infoframe(&packet, AUDIO_SAMPLE_RATE, 2, 16);
  hstx_encode_data_island(&island, &packet, false, false);
  vblank_infoframe_vsync_on_len =
      build_vblank_with_di(vblank_infoframe_vsync_on, island.words, true);

  // AVI InfoFrame
  packet_set_avi_infoframe(&packet, 1);
  hstx_encode_data_island(&island, &packet, true, false);
  vblank_avi_infoframe_len =
      build_vblank_with_di(vblank_avi_infoframe, island.words, false);

  // Fill ring buffer with initial audio
  update_audio_ring_buffer();

  printf("Audio initialized:\n");
  printf("  48kHz, N=%d, CTS=%lu\n", AUDIO_N_VALUE, cts_value);
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

  if (vsync_active) {
    // Vsync period - send ACR and Audio InfoFrame
    if (v_scanline == MODE_V_FRONT_PORCH) {
      ch->read_addr = (uintptr_t)vblank_acr_vsync_on;
      ch->transfer_count = vblank_acr_vsync_on_len;
      video_frame_count++;
      audio_sample_accum = 0; // Reset accumulator at frame start
    } else {
      ch->read_addr = (uintptr_t)vblank_infoframe_vsync_on;
      ch->transfer_count = vblank_infoframe_vsync_on_len;
    }
  } else if (active_video && !vactive_cmdlist_posted) {
    // Start of active line - check if we should send audio
    audio_sample_accum += SAMPLES_PER_LINE_FP;

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
    // Middle of active line - send pixels
    ch->read_addr = (uintptr_t)&framebuf[(v_scanline - (MODE_V_TOTAL_LINES -
                                                        MODE_V_ACTIVE_LINES)) *
                                         MODE_H_ACTIVE_PIXELS];
    ch->transfer_count = MODE_H_ACTIVE_PIXELS / sizeof(uint32_t);
    vactive_cmdlist_posted = false;
  } else {
    // Blanking (front or back porch) - can also send audio here!
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
// Main
// ============================================================================

int main(void) {
  stdio_init_all();

  // Wait for USB serial to enumerate
  sleep_ms(2000);

  printf("\n\n");
  printf("================================\n");
  printf("HSTX Audio Test\n");
  printf("================================\n");
  printf("640x480 @ 60Hz with HDMI audio\n\n");

  // Initialize framebuffer
  draw_frame();

  // Initialize sine table for fast audio generation
  init_sine_table();

  // Initialize audio packets (pre-compute command lists)
  init_audio_packets();

  // Pre-compute null lines WITH data islands (for blanking)
  vblank_di_len = build_line_with_di(
      vblank_di_ping, hstx_get_null_data_island(true, false), false, false);
  memcpy(vblank_di_pong, vblank_di_ping, sizeof(vblank_di_ping));

  // Configure HSTX TMDS encoder for RGB332
  hstx_ctrl_hw->expand_tmds = 2 << HSTX_CTRL_EXPAND_TMDS_L2_NBITS_LSB |
                              0 << HSTX_CTRL_EXPAND_TMDS_L2_ROT_LSB |
                              2 << HSTX_CTRL_EXPAND_TMDS_L1_NBITS_LSB |
                              29 << HSTX_CTRL_EXPAND_TMDS_L1_ROT_LSB |
                              1 << HSTX_CTRL_EXPAND_TMDS_L0_NBITS_LSB |
                              26 << HSTX_CTRL_EXPAND_TMDS_L0_ROT_LSB;

  hstx_ctrl_hw->expand_shift = 4 << HSTX_CTRL_EXPAND_SHIFT_ENC_N_SHIFTS_LSB |
                               8 << HSTX_CTRL_EXPAND_SHIFT_ENC_SHIFT_LSB |
                               1 << HSTX_CTRL_EXPAND_SHIFT_RAW_N_SHIFTS_LSB |
                               0 << HSTX_CTRL_EXPAND_SHIFT_RAW_SHIFT_LSB;

  hstx_ctrl_hw->csr = 0;
  hstx_ctrl_hw->csr =
      HSTX_CTRL_CSR_EXPAND_EN_BITS |
      5u << HSTX_CTRL_CSR_CLKDIV_LSB | // 150MHz / 5 = 30MHz (RP2350 default is
                                       // 150MHz, not 125MHz!)
      5u << HSTX_CTRL_CSR_N_SHIFTS_LSB | 2u << HSTX_CTRL_CSR_SHIFT_LSB |
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

  printf("Starting HSTX output...\n");
  dma_channel_start(DMACH_PING);

  // Animation loop
  uint32_t last_frame = 0;
  uint32_t last_vframe = 0;
  while (1) {
    // Fill audio ring buffer in the background
    update_audio_ring_buffer();

    // Update animation
    update_box();
    draw_frame();

    // Print status every second
    if (video_frame_count >= last_frame + 60) {
      printf("Frame %u, audio_ring_tail=%u\n", (unsigned int)video_frame_count,
             (unsigned int)audio_ring_tail);
      last_frame = video_frame_count;
    }

    // Wait for next video frame (rough sync)
    while (video_frame_count == last_vframe) {
      update_audio_ring_buffer(); // Keep filling buffer while waiting
      tight_loop_contents();
    }
    last_vframe = video_frame_count;
  }

  return 0;
}
