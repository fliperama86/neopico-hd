#include "pico_dvi2/video_output.h"
#include "hardware/dma.h"
#include "hardware/gpio.h"
#include "hardware/irq.h"
#include "hardware/structs/bus_ctrl.h"
#include "hardware/structs/hstx_ctrl.h"
#include "hardware/structs/hstx_fifo.h"
#include "pico_dvi2/hstx_data_island_queue.h"
#include "pico_dvi2/hstx_packet.h"
// #include "osd.h"
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

#define HSTX_CMD_RAW (0x0u << 12)
#define HSTX_CMD_RAW_REPEAT (0x1u << 12)
#define HSTX_CMD_TMDS (0x2u << 12)
#define HSTX_CMD_TMDS_REPEAT (0x3u << 12)
#define HSTX_CMD_NOP (0xfu << 12)

#define SYNC_AFTER_DI (MODE_H_SYNC_WIDTH - W_PREAMBLE - W_DATA_ISLAND)

// ============================================================================
// Audio/Video State
// ============================================================================

uint16_t framebuf[FRAMEBUF_HEIGHT * FRAMEBUF_WIDTH] __attribute__((aligned(4)));
volatile uint32_t video_frame_count = 0;

static uint16_t line_buffer[MODE_H_ACTIVE_PIXELS] __attribute__((aligned(4)));
static uint32_t v_scanline = 2;
static bool vactive_cmdlist_posted = false;
static bool dma_pong = false;

static video_output_task_fn background_task = NULL;

#define DMACH_PING 0
#define DMACH_PONG 1

// ============================================================================
// Command Lists
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

static uint32_t vactive_di_ping[128], vactive_di_pong[128],
    vactive_di_null[128];
static uint32_t vactive_di_len, vactive_di_null_len;

static uint32_t vblank_di_ping[128], vblank_di_pong[128], vblank_di_null[128];
static uint32_t vblank_di_len, vblank_di_null_len;

static uint32_t vblank_acr_vsync_on[64], vblank_acr_vsync_on_len;
static uint32_t vblank_acr_vsync_off[64], vblank_acr_vsync_off_len;
static uint32_t vblank_infoframe_vsync_on[64], vblank_infoframe_vsync_on_len;
static uint32_t vblank_infoframe_vsync_off[64], vblank_infoframe_vsync_off_len;
static uint32_t vblank_avi_infoframe[64], vblank_avi_infoframe_len;

// ============================================================================
// Internal Helpers
// ============================================================================

static uint32_t build_line_with_di(uint32_t *buf, const uint32_t *di_words,
                                   bool vsync, bool active) {
  uint32_t *p = buf;
  uint32_t sync_h0 = vsync ? SYNC_V0_H0 : SYNC_V1_H0;
  uint32_t sync_h1 = vsync ? SYNC_V0_H1 : SYNC_V1_H1;
  uint32_t preamble = vsync ? PREAMBLE_V0_H0 : PREAMBLE_V1_H0;

  *p++ = HSTX_CMD_RAW_REPEAT | MODE_H_FRONT_PORCH;
  *p++ = sync_h1;
  *p++ = HSTX_CMD_NOP;

  *p++ = HSTX_CMD_RAW_REPEAT | W_PREAMBLE;
  *p++ = preamble;
  *p++ = HSTX_CMD_NOP;

  *p++ = HSTX_CMD_RAW | W_DATA_ISLAND;
  for (int i = 0; i < W_DATA_ISLAND; i++)
    *p++ = di_words[i];
  *p++ = HSTX_CMD_NOP;

  *p++ = HSTX_CMD_RAW_REPEAT | SYNC_AFTER_DI;
  *p++ = sync_h0;
  *p++ = HSTX_CMD_NOP;

  if (active) {
    *p++ = HSTX_CMD_RAW_REPEAT | MODE_H_BACK_PORCH;
    *p++ = sync_h1;
    *p++ = HSTX_CMD_TMDS | MODE_H_ACTIVE_PIXELS;
  } else {
    *p++ = HSTX_CMD_RAW_REPEAT | (MODE_H_BACK_PORCH + MODE_H_ACTIVE_PIXELS);
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

  // Advance audio/data-island scheduler exactly once per scanline
  if (!vactive_cmdlist_posted) {
    hstx_di_queue_tick();
  }

  bool vsync_active = (v_scanline >= MODE_V_FRONT_PORCH &&
                       v_scanline < (MODE_V_FRONT_PORCH + MODE_V_SYNC_WIDTH));
  bool front_porch = (v_scanline < MODE_V_FRONT_PORCH);
  bool back_porch =
      (v_scanline >= MODE_V_FRONT_PORCH + MODE_V_SYNC_WIDTH &&
       v_scanline < MODE_V_FRONT_PORCH + MODE_V_SYNC_WIDTH + MODE_V_BACK_PORCH);
  bool active_video = (!vsync_active && !front_porch && !back_porch);

  bool send_acr = (v_scanline >= (MODE_V_FRONT_PORCH + MODE_V_SYNC_WIDTH) &&
                   v_scanline < (MODE_V_TOTAL_LINES - MODE_V_ACTIVE_LINES) &&
                   (v_scanline % 4 == 0));

  if (vsync_active) {
    if (v_scanline == MODE_V_FRONT_PORCH) {
      ch->read_addr = (uintptr_t)vblank_acr_vsync_on;
      ch->transfer_count = vblank_acr_vsync_on_len;
      video_frame_count++;
    } else {
      ch->read_addr = (uintptr_t)vblank_infoframe_vsync_on;
      ch->transfer_count = vblank_infoframe_vsync_on_len;
    }
  } else if (active_video && !vactive_cmdlist_posted) {
    uint32_t active_line =
        v_scanline - (MODE_V_TOTAL_LINES - MODE_V_ACTIVE_LINES);
    uint32_t fb_line = active_line / 2;
    const uint16_t *src = &framebuf[fb_line * FRAMEBUF_WIDTH];
    uint32_t *dst32 = (uint32_t *)line_buffer;

    for (uint32_t i = 0; i < FRAMEBUF_WIDTH; i++) {
      uint32_t p = src[i];
      dst32[i] = p | (p << 16);
    }

    uint32_t *buf = dma_pong ? vactive_di_ping : vactive_di_pong;
    const uint32_t *di_words = hstx_di_queue_get_audio_packet();
    if (di_words) {
      vactive_di_len = build_line_with_di(buf, di_words, false, true);
      ch->read_addr = (uintptr_t)buf;
      ch->transfer_count = vactive_di_len;
    } else {
      ch->read_addr = (uintptr_t)vactive_di_null;
      ch->transfer_count = vactive_di_null_len;
    }
    vactive_cmdlist_posted = true;
  } else if (active_video && vactive_cmdlist_posted) {
    ch->read_addr = (uintptr_t)line_buffer;
    ch->transfer_count =
        (MODE_H_ACTIVE_PIXELS * sizeof(uint16_t)) / sizeof(uint32_t);
    vactive_cmdlist_posted = false;
  } else {
    if (send_acr) {
      ch->read_addr = (uintptr_t)vblank_acr_vsync_off;
      ch->transfer_count = vblank_acr_vsync_off_len;
    } else if (v_scanline == 0) {
      ch->read_addr = (uintptr_t)vblank_avi_infoframe;
      ch->transfer_count = vblank_avi_infoframe_len;
    } else {
      const uint32_t *di_words = hstx_di_queue_get_audio_packet();
      if (di_words) {
        uint32_t *buf = dma_pong ? vblank_di_ping : vblank_di_pong;
        vblank_di_len = build_line_with_di(buf, di_words, false, false);
        ch->read_addr = (uintptr_t)buf;
        ch->transfer_count = vblank_di_len;
      } else {
        ch->read_addr = (uintptr_t)vblank_di_null;
        ch->transfer_count = vblank_di_null_len;
      }
    }
  }
  if (!vactive_cmdlist_posted)
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
  hstx_encode_data_island(&island, &packet, true, true);
  vblank_acr_vsync_on_len =
      build_line_with_di(vblank_acr_vsync_on, island.words, true, false);
  hstx_encode_data_island(&island, &packet, false, true);
  vblank_acr_vsync_off_len =
      build_line_with_di(vblank_acr_vsync_off, island.words, false, false);

  hstx_packet_set_audio_infoframe(&packet, 48000, 2, 16);
  hstx_encode_data_island(&island, &packet, true, true);
  vblank_infoframe_vsync_on_len =
      build_line_with_di(vblank_infoframe_vsync_on, island.words, true, false);
  hstx_encode_data_island(&island, &packet, false, true);
  vblank_infoframe_vsync_off_len = build_line_with_di(
      vblank_infoframe_vsync_off, island.words, false, false);

  hstx_packet_set_avi_infoframe(&packet, 1);
  hstx_encode_data_island(&island, &packet, false, true);
  vblank_avi_infoframe_len =
      build_line_with_di(vblank_avi_infoframe, island.words, false, false);

  vblank_di_null_len = build_line_with_di(
      vblank_di_null, hstx_get_null_data_island(false, true), false, false);
  vactive_di_null_len = build_line_with_di(
      vactive_di_null, hstx_get_null_data_island(false, true), false, true);

  vblank_di_len = build_line_with_di(
      vblank_di_ping, hstx_get_null_data_island(false, true), false, false);
  memcpy(vblank_di_pong, vblank_di_ping, sizeof(vblank_di_ping));
}

void video_output_set_background_task(video_output_task_fn task) {
  background_task = task;
}

void video_output_core1_run(void) {
  // HSTX Hardware Setup
  hstx_ctrl_hw->expand_tmds = 4 << HSTX_CTRL_EXPAND_TMDS_L2_NBITS_LSB |
                              8 << HSTX_CTRL_EXPAND_TMDS_L2_ROT_LSB |
                              5 << HSTX_CTRL_EXPAND_TMDS_L1_NBITS_LSB |
                              3 << HSTX_CTRL_EXPAND_TMDS_L1_ROT_LSB |
                              4 << HSTX_CTRL_EXPAND_TMDS_L0_NBITS_LSB |
                              13 << HSTX_CTRL_EXPAND_TMDS_L0_ROT_LSB;

  hstx_ctrl_hw->expand_shift = 2 << HSTX_CTRL_EXPAND_SHIFT_ENC_N_SHIFTS_LSB |
                               16 << HSTX_CTRL_EXPAND_SHIFT_ENC_SHIFT_LSB |
                               1 << HSTX_CTRL_EXPAND_SHIFT_RAW_N_SHIFTS_LSB |
                               0 << HSTX_CTRL_EXPAND_SHIFT_RAW_SHIFT_LSB;

  hstx_ctrl_hw->csr = 0;
  hstx_ctrl_hw->csr = HSTX_CTRL_CSR_EXPAND_EN_BITS |
                      5u << HSTX_CTRL_CSR_CLKDIV_LSB |
                      5u << HSTX_CTRL_CSR_N_SHIFTS_LSB |
                      2u << HSTX_CTRL_CSR_SHIFT_LSB | HSTX_CTRL_CSR_EN_BITS;

  hstx_ctrl_hw->bit[0] = HSTX_CTRL_BIT0_CLK_BITS | HSTX_CTRL_BIT0_INV_BITS;
  hstx_ctrl_hw->bit[1] = HSTX_CTRL_BIT0_CLK_BITS;
  for (uint lane = 0; lane < 3; ++lane) {
    int bit = 2 + lane * 2;
    uint32_t lane_data_sel_bits = (lane * 10) << HSTX_CTRL_BIT0_SEL_P_LSB |
                                  (lane * 10 + 1) << HSTX_CTRL_BIT0_SEL_N_LSB;
    hstx_ctrl_hw->bit[bit] = lane_data_sel_bits | HSTX_CTRL_BIT0_INV_BITS;
    hstx_ctrl_hw->bit[bit + 1] = lane_data_sel_bits;
  }

  // Set GPIO 12-19 to HSTX function
  for (int i = PIN_HSTX_CLK; i <= PIN_HSTX_D2 + 1; ++i)
    gpio_set_function(i, GPIO_FUNC_HSTX);

  // DMA Setup
  dma_channel_config c = dma_channel_get_default_config(DMACH_PING);
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
