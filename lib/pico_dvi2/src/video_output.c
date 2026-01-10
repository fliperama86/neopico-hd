#include "pico_dvi2/video_output.h"
#include "pico_dvi2/hstx_packet.h"
#include "hardware/dma.h"
#include "hardware/gpio.h"
#include "hardware/irq.h"
#include "hardware/structs/bus_ctrl.h"
#include "hardware/structs/hstx_ctrl.h"
#include "hardware/structs/hstx_fifo.h"
#include "pico_dvi2/hstx_data_island_queue.h"
#include "pico/stdlib.h"
#include "pico_dvi2/hstx_pins.h"
#include <math.h>
#include <string.h>

// ============================================================================ 
// DVI/HSTX Constants
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
#define PREAMBLE_V0_H0                                                         \ 
  (TMDS_CTRL_00 | (TMDS_CTRL_01 << 10) | (TMDS_CTRL_01 << 20))
#define PREAMBLE_V1_H0                                                         \ 
  (TMDS_CTRL_10 | (TMDS_CTRL_01 << 10) | (TMDS_CTRL_01 << 20))
#define PREAMBLE_V0_H1                                                         \ 
  (TMDS_CTRL_01 | (TMDS_CTRL_01 << 10) | (TMDS_CTRL_01 << 20))
#define PREAMBLE_V1_H1                                                         \ 
  (TMDS_CTRL_11 | (TMDS_CTRL_01 << 10) | (TMDS_CTRL_01 << 20))

#define HSTX_CMD_RAW (0x0u << 12)
#define HSTX_CMD_RAW_REPEAT (0x1u << 12)
#define HSTX_CMD_TMDS (0x2u << 12)
#define HSTX_CMD_TMDS_REPEAT (0x3u << 12)
#define HSTX_CMD_NOP (0xfu << 12)

#define SYNC_REMAINING_BACK_PORCH (MODE_H_BACK_PORCH - W_PREAMBLE - W_DATA_ISLAND)

// ============================================================================ 
// Global State
// ============================================================================ 

volatile uint32_t video_frame_count = 0;

// Single buffer per ping/pong to hold the entire scanline command list + pixels
static uint32_t scanline_buffer[2][1024] __attribute__((aligned(4)));
static uint32_t v_scanline = 0;
static bool dma_pong = false;

static video_output_task_fn background_task = NULL;
static video_output_scanline_cb_t scanline_callback = NULL;

#define DMACH_PING 0
#define DMACH_PONG 1

// ============================================================================ 
// Command Lists
// ============================================================================ 

static uint32_t vblank_acr_vsync_on[128], vblank_acr_vsync_on_len;
static uint32_t vblank_acr_vsync_off[128], vblank_acr_vsync_off_len;
static uint32_t vblank_infoframe_vsync_on[128], vblank_infoframe_vsync_on_len;
static uint32_t vblank_avi_infoframe[128], vblank_avi_infoframe_len;
static uint32_t vblank_di_null[128], vblank_di_null_len;

// ============================================================================ 
// Internal Helpers
// ============================================================================ 

static uint32_t build_line_with_di(uint32_t *buf, const uint32_t *di_words, 
                                   bool vsync, bool active) {
  uint32_t *p = buf;
  uint32_t sync_h0 = vsync ? SYNC_V0_H0 : SYNC_V1_H0;
  uint32_t sync_h1 = vsync ? SYNC_V0_H1 : SYNC_V1_H1;
  uint32_t preamble = vsync ? PREAMBLE_V0_H1 : PREAMBLE_V1_H1;

  // 1. Front Porch (Idle)
  *p++ = HSTX_CMD_RAW_REPEAT | MODE_H_FRONT_PORCH;
  *p++ = sync_h1;

  // 2. Sync Pulse (Active) - Full Duration
  *p++ = HSTX_CMD_RAW_REPEAT | MODE_H_SYNC_WIDTH;
  *p++ = sync_h0;

  // 3. Preamble (Idle)
  *p++ = HSTX_CMD_RAW_REPEAT | W_PREAMBLE;
  *p++ = preamble;

  // 4. Data Island (Idle)
  *p++ = HSTX_CMD_RAW | W_DATA_ISLAND;
  for (int i = 0; i < W_DATA_ISLAND; i++)
    *p++ = di_words[i];

  // 5. Remaining Back Porch (Idle)
  if (active) {
    *p++ = HSTX_CMD_RAW_REPEAT | SYNC_REMAINING_BACK_PORCH;
    *p++ = sync_h1;
    *p++ = HSTX_CMD_TMDS | MODE_H_ACTIVE_PIXELS;
  } else {
    *p++ = HSTX_CMD_RAW_REPEAT |
           (SYNC_REMAINING_BACK_PORCH + MODE_H_ACTIVE_PIXELS);
    *p++ = sync_h1;
    *p++ = HSTX_CMD_NOP;
  }
  return (uint32_t)(p - buf);
}

// ============================================================================ 
// DMA IRQ Handler
// ============================================================================ 

void __scratch_x("") dma_irq_handler() {
  uint32_t ch_num = dma_pong ? DMACH_PONG : DMACH_PING;
  dma_channel_hw_t *ch = &dma_hw->ch[ch_num];
  dma_hw->intr = 1u << ch_num;
  dma_pong = !dma_pong;

  // Next buffer to fill
  uint32_t next_ch_num = dma_pong ? DMACH_PONG : DMACH_PING;
  dma_channel_hw_t *next_ch = &dma_hw->ch[next_ch_num];
  uint32_t *buf = scanline_buffer[dma_pong];

  hstx_di_queue_tick();

  bool vsync_active = (v_scanline >= MODE_V_FRONT_PORCH &&
                       v_scanline < (MODE_V_FRONT_PORCH + MODE_V_SYNC_WIDTH));
  bool active_video = (v_scanline >= (MODE_V_TOTAL_LINES - MODE_V_ACTIVE_LINES));

  bool send_acr = (v_scanline >= (MODE_V_FRONT_PORCH + MODE_V_SYNC_WIDTH) &&
                   v_scanline < (MODE_V_TOTAL_LINES - MODE_V_ACTIVE_LINES) &&
                   (v_scanline % 4 == 0));

  uint32_t len = 0;

  if (vsync_active) {
    if (v_scanline == MODE_V_FRONT_PORCH) {
      video_frame_count++;
      len = vblank_acr_vsync_on_len;
      memcpy(buf, vblank_acr_vsync_on, len * 4);
    } else {
      len = vblank_infoframe_vsync_on_len;
      memcpy(buf, vblank_infoframe_vsync_on, len * 4);
    }
  } else if (active_video) {
    uint32_t active_line = v_scanline - (MODE_V_TOTAL_LINES - MODE_V_ACTIVE_LINES);
    
    const uint32_t *di_words = hstx_di_queue_get_audio_packet();
    if (!di_words) di_words = hstx_get_null_data_island(false, false);
    
    len = build_line_with_di(buf, di_words, false, true);

    // Call callback to fill the rest of the buffer with pixels
    if (scanline_callback) {
      scanline_callback(v_scanline, active_line, &buf[len]);
      len += (MODE_H_ACTIVE_PIXELS / 2); // Each word is 2 pixels
      buf[len++] = HSTX_CMD_NOP;
    } else {
      // If no callback, just send black/zero pixels
      uint32_t *p = &buf[len];
      for (int i = 0; i < MODE_H_ACTIVE_PIXELS / 2; i++) *p++ = 0;
      len += (MODE_H_ACTIVE_PIXELS / 2);
      buf[len++] = HSTX_CMD_NOP;
    }
  } else {
    if (send_acr) {
      len = vblank_acr_vsync_off_len;
      memcpy(buf, vblank_acr_vsync_off, len * 4);
    } else if (v_scanline == 0) {
      len = vblank_avi_infoframe_len;
      memcpy(buf, vblank_avi_infoframe, len * 4);
    } else {
      const uint32_t *di_words = hstx_di_queue_get_audio_packet();
      if (!di_words) di_words = hstx_get_null_data_island(false, false);
      len = build_line_with_di(buf, di_words, false, false);
    }
  }

  next_ch->read_addr = (uintptr_t)buf;
  next_ch->transfer_count = len;

  v_scanline = (v_scanline + 1) % MODE_V_TOTAL_LINES;
}

// ============================================================================ 
// Public Interface
// ============================================================================ 

void video_output_init(void) {
  // Claim DMA channels for HSTX (channels 0 and 1)
  dma_channel_claim(DMACH_PING);
  dma_channel_claim(DMACH_PONG);

  hstx_packet_t packet;
  hstx_data_island_t island;

  hstx_packet_set_acr(&packet, 6144, 25200);
  hstx_encode_data_island(&island, &packet, true, false);
  vblank_acr_vsync_on_len =
      build_line_with_di(vblank_acr_vsync_on, island.words, true, false);
  hstx_encode_data_island(&island, &packet, false, false);
  vblank_acr_vsync_off_len =
      build_line_with_di(vblank_acr_vsync_off, island.words, false, false);

  hstx_packet_set_audio_infoframe(&packet, 48000, 2, 16);
  hstx_encode_data_island(&island, &packet, true, false);
  vblank_infoframe_vsync_on_len =
      build_line_with_di(vblank_infoframe_vsync_on, island.words, true, false);

  hstx_packet_set_avi_infoframe(&packet, 1);
  hstx_encode_data_island(&island, &packet, false, false);
  vblank_avi_infoframe_len =
      build_line_with_di(vblank_avi_infoframe, island.words, false, false);

  vblank_di_null_len = build_line_with_di(
      vblank_di_null, hstx_get_null_data_island(false, false), false, false);
}

void video_output_set_scanline_callback(video_output_scanline_cb_t cb) {
  scanline_callback = cb;
}

void video_output_set_background_task(video_output_task_fn task) {
  background_task = task;
}

void video_output_core1_run(void) {
  // HSTX Hardware Setup: Correct RGB565 component extraction
  hstx_ctrl_hw->expand_tmds = 4 << HSTX_CTRL_EXPAND_TMDS_L2_NBITS_LSB | // Red 5 bits
                              11 << HSTX_CTRL_EXPAND_TMDS_L2_ROT_LSB | // Rotate 11
                              5 << HSTX_CTRL_EXPAND_TMDS_L1_NBITS_LSB | // Green 6 bits
                              5 << HSTX_CTRL_EXPAND_TMDS_L1_ROT_LSB |  // Rotate 5
                              4 << HSTX_CTRL_EXPAND_TMDS_L0_NBITS_LSB | // Blue 5 bits
                              0 << HSTX_CTRL_EXPAND_TMDS_L0_ROT_LSB;   // Rotate 0

  hstx_ctrl_hw->expand_shift = 2 << HSTX_CTRL_EXPAND_SHIFT_ENC_N_SHIFTS_LSB |
                               16 << HSTX_CTRL_EXPAND_SHIFT_ENC_SHIFT_LSB |
                               1 << HSTX_CTRL_EXPAND_SHIFT_RAW_N_SHIFTS_LSB |
                               0 << HSTX_CTRL_EXPAND_SHIFT_RAW_SHIFT_LSB;

  hstx_ctrl_hw->csr = 0;
  hstx_ctrl_hw->csr = HSTX_CTRL_CSR_EXPAND_EN_BITS |
                      5u << HSTX_CTRL_CSR_CLKDIV_LSB | // 126MHz / 5 = 25.2MHz pixel clock
                      5u << HSTX_CTRL_CSR_N_SHIFTS_LSB |
                      2u << HSTX_CTRL_CSR_SHIFT_LSB | HSTX_CTRL_CSR_EN_BITS;

  // Set Polarity: Even=Normal, Odd=Inverted (Standard DVI)
  hstx_ctrl_hw->bit[0] = HSTX_CTRL_BIT0_CLK_BITS;
  hstx_ctrl_hw->bit[1] = HSTX_CTRL_BIT0_CLK_BITS | HSTX_CTRL_BIT0_INV_BITS;
  for (uint lane = 0; lane < 3; ++lane) {
    int bit = 2 + lane * 2;
    uint32_t lane_data_sel_bits = (lane * 10) << HSTX_CTRL_BIT0_SEL_P_LSB |
                                  (lane * 10 + 1) << HSTX_CTRL_BIT0_SEL_N_LSB;
    hstx_ctrl_hw->bit[bit] = lane_data_sel_bits;
    hstx_ctrl_hw->bit[bit + 1] = lane_data_sel_bits | HSTX_CTRL_BIT0_INV_BITS;
  }

  // Set GPIO 12-19 to HSTX function with Fast Slew and 12mA Drive
  for (int i = PIN_HSTX_CLK; i <= PIN_HSTX_D2 + 1; ++i) {
    gpio_set_function(i, 14); // GPIO_FUNC_HSTX
    gpio_set_slew_rate(i, GPIO_SLEW_RATE_FAST);
    gpio_set_drive_strength(i, GPIO_DRIVE_STRENGTH_12MA);
  }

  // DMA Setup
  // We start with a blanking line
  dma_channel_config c = dma_channel_get_default_config(DMACH_PING);
  channel_config_set_chain_to(&c, DMACH_PONG);
  channel_config_set_dreq(&c, DREQ_HSTX);
  dma_channel_configure(DMACH_PING, &c, &hstx_fifo_hw->fifo,
                        vblank_di_null, vblank_di_null_len,
                        false);

  c = dma_channel_get_default_config(DMACH_PONG);
  channel_config_set_chain_to(&c, DMACH_PING);
  channel_config_set_dreq(&c, DREQ_HSTX);
  dma_channel_configure(DMACH_PONG, &c, &hstx_fifo_hw->fifo,
                        vblank_di_null, vblank_di_null_len,
                        false);

  dma_hw->ints0 = (1u << DMACH_PING) | (1u << DMACH_PONG);
  dma_hw->inte0 = (1u << DMACH_PING) | (1u << DMACH_PONG);
  irq_set_exclusive_handler(DMA_IRQ_0, dma_irq_handler);
  irq_set_priority(DMA_IRQ_0, 0);
  irq_set_enabled(DMA_IRQ_0, true);

  bus_ctrl_hw->priority =
      BUSCTRL_BUS_PRIORITY_DMA_W_BITS | BUSCTRL_BUS_PRIORITY_DMA_R_BITS;
  dma_channel_start(DMACH_PING);

  while (1) {
    if (background_task) {
      background_task();
    }
    tight_loop_contents();
  }
}