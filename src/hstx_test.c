/**
 * HSTX Test - Color Bars via Hardware TMDS Encoder
 *
 * Based on pico-examples/hstx/dvi_out_hstx_encoder
 * Displays color bars at 640x480 @ 60Hz using RGB332 format.
 *
 * HSTX Pin Assignment (GPIO 12-19) - Custom Spotpear wiring:
 *     GPIO 12: CLKN    GPIO 13: CLKP
 *     GPIO 14: D0N     GPIO 15: D0P  (Lane 0 - Blue)
 *     GPIO 16: D1N     GPIO 17: D1P  (Lane 1 - Green)
 *     GPIO 18: D2N     GPIO 19: D2P  (Lane 2 - Red)
 *
 * Target: RP2350A (Pico 2)
 */

// Copyright (c) 2024 Raspberry Pi (Trading) Ltd.
// SPDX-License-Identifier: BSD-3-Clause

#include "hardware/dma.h"
#include "hardware/gpio.h"
#include "hardware/irq.h"
#include "hardware/structs/bus_ctrl.h"
#include "hardware/structs/hstx_ctrl.h"
#include "hardware/structs/hstx_fifo.h"
#include "pico/stdlib.h"
#include "pico/platform.h"

// ----------------------------------------------------------------------------
// DVI constants

#define TMDS_CTRL_00 0x354u
#define TMDS_CTRL_01 0x0abu
#define TMDS_CTRL_10 0x154u
#define TMDS_CTRL_11 0x2abu

#define SYNC_V0_H0 (TMDS_CTRL_00 | (TMDS_CTRL_00 << 10) | (TMDS_CTRL_00 << 20))
#define SYNC_V0_H1 (TMDS_CTRL_01 | (TMDS_CTRL_00 << 10) | (TMDS_CTRL_00 << 20))
#define SYNC_V1_H0 (TMDS_CTRL_10 | (TMDS_CTRL_00 << 10) | (TMDS_CTRL_00 << 20))
#define SYNC_V1_H1 (TMDS_CTRL_11 | (TMDS_CTRL_00 << 10) | (TMDS_CTRL_00 << 20))

#define MODE_H_FRONT_PORCH   16
#define MODE_H_SYNC_WIDTH    96
#define MODE_H_BACK_PORCH    48
#define MODE_H_ACTIVE_PIXELS 640

#define MODE_V_FRONT_PORCH   10
#define MODE_V_SYNC_WIDTH    2
#define MODE_V_BACK_PORCH    33
#define MODE_V_ACTIVE_LINES  480

#define MODE_H_TOTAL_PIXELS ( \
    MODE_H_FRONT_PORCH + MODE_H_SYNC_WIDTH + \
    MODE_H_BACK_PORCH  + MODE_H_ACTIVE_PIXELS \
)
#define MODE_V_TOTAL_LINES  ( \
    MODE_V_FRONT_PORCH + MODE_V_SYNC_WIDTH + \
    MODE_V_BACK_PORCH  + MODE_V_ACTIVE_LINES \
)

#define HSTX_CMD_RAW         (0x0u << 12)
#define HSTX_CMD_RAW_REPEAT  (0x1u << 12)
#define HSTX_CMD_TMDS        (0x2u << 12)
#define HSTX_CMD_TMDS_REPEAT (0x3u << 12)
#define HSTX_CMD_NOP         (0xfu << 12)

// ----------------------------------------------------------------------------
// Framebuffer (RGB332: 8 bits per pixel)

static uint8_t framebuf[MODE_V_ACTIVE_LINES * MODE_H_ACTIVE_PIXELS];

// RGB332 color conversion (RRRGGGBB format)
static inline uint8_t rgb332(uint8_t r, uint8_t g, uint8_t b) {
    return ((r & 0xe0) >> 0) | ((g & 0xe0) >> 3) | ((b & 0xc0) >> 6);
}

// Bouncing box state
#define BOX_SIZE 64
static int box_x = 100, box_y = 100;
static int box_dx = 3, box_dy = 2;

// Draw a frame with bouncing box
static void draw_frame(void) {
    uint8_t bg = rgb332(0, 0, 64);      // Dark blue background
    uint8_t box = rgb332(255, 255, 0);  // Yellow box

    for (int y = 0; y < MODE_V_ACTIVE_LINES; y++) {
        for (int x = 0; x < MODE_H_ACTIVE_PIXELS; x++) {
            if (x >= box_x && x < box_x + BOX_SIZE &&
                y >= box_y && y < box_y + BOX_SIZE) {
                framebuf[y * MODE_H_ACTIVE_PIXELS + x] = box;
            } else {
                framebuf[y * MODE_H_ACTIVE_PIXELS + x] = bg;
            }
        }
    }
}

// Update bouncing box position
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

// ----------------------------------------------------------------------------
// HSTX command lists

static uint32_t vblank_line_vsync_off[] = {
    HSTX_CMD_RAW_REPEAT | MODE_H_FRONT_PORCH,
    SYNC_V1_H1,
    HSTX_CMD_RAW_REPEAT | MODE_H_SYNC_WIDTH,
    SYNC_V1_H0,
    HSTX_CMD_RAW_REPEAT | (MODE_H_BACK_PORCH + MODE_H_ACTIVE_PIXELS),
    SYNC_V1_H1,
    HSTX_CMD_NOP
};

static uint32_t vblank_line_vsync_on[] = {
    HSTX_CMD_RAW_REPEAT | MODE_H_FRONT_PORCH,
    SYNC_V0_H1,
    HSTX_CMD_RAW_REPEAT | MODE_H_SYNC_WIDTH,
    SYNC_V0_H0,
    HSTX_CMD_RAW_REPEAT | (MODE_H_BACK_PORCH + MODE_H_ACTIVE_PIXELS),
    SYNC_V0_H1,
    HSTX_CMD_NOP
};

static uint32_t vactive_line[] = {
    HSTX_CMD_RAW_REPEAT | MODE_H_FRONT_PORCH,
    SYNC_V1_H1,
    HSTX_CMD_NOP,
    HSTX_CMD_RAW_REPEAT | MODE_H_SYNC_WIDTH,
    SYNC_V1_H0,
    HSTX_CMD_NOP,
    HSTX_CMD_RAW_REPEAT | MODE_H_BACK_PORCH,
    SYNC_V1_H1,
    HSTX_CMD_TMDS | MODE_H_ACTIVE_PIXELS
};

// ----------------------------------------------------------------------------
// DMA logic

#define DMACH_PING 0
#define DMACH_PONG 1

static bool dma_pong = false;
static uint v_scanline = 2;
static bool vactive_cmdlist_posted = false;

void __scratch_x("") dma_irq_handler() {
    uint ch_num = dma_pong ? DMACH_PONG : DMACH_PING;
    dma_channel_hw_t *ch = &dma_hw->ch[ch_num];
    dma_hw->intr = 1u << ch_num;
    dma_pong = !dma_pong;

    if (v_scanline >= MODE_V_FRONT_PORCH && v_scanline < (MODE_V_FRONT_PORCH + MODE_V_SYNC_WIDTH)) {
        ch->read_addr = (uintptr_t)vblank_line_vsync_on;
        ch->transfer_count = count_of(vblank_line_vsync_on);
    } else if (v_scanline < MODE_V_FRONT_PORCH + MODE_V_SYNC_WIDTH + MODE_V_BACK_PORCH) {
        ch->read_addr = (uintptr_t)vblank_line_vsync_off;
        ch->transfer_count = count_of(vblank_line_vsync_off);
    } else if (!vactive_cmdlist_posted) {
        ch->read_addr = (uintptr_t)vactive_line;
        ch->transfer_count = count_of(vactive_line);
        vactive_cmdlist_posted = true;
    } else {
        ch->read_addr = (uintptr_t)&framebuf[(v_scanline - (MODE_V_TOTAL_LINES - MODE_V_ACTIVE_LINES)) * MODE_H_ACTIVE_PIXELS];
        ch->transfer_count = MODE_H_ACTIVE_PIXELS / sizeof(uint32_t);
        vactive_cmdlist_posted = false;
    }

    if (!vactive_cmdlist_posted) {
        v_scanline = (v_scanline + 1) % MODE_V_TOTAL_LINES;
    }
}

// ----------------------------------------------------------------------------
// Main

int main(void) {
    stdio_init_all();

    // Initial frame
    draw_frame();

    // Configure HSTX's TMDS encoder for RGB332
    hstx_ctrl_hw->expand_tmds =
        2  << HSTX_CTRL_EXPAND_TMDS_L2_NBITS_LSB |
        0  << HSTX_CTRL_EXPAND_TMDS_L2_ROT_LSB   |
        2  << HSTX_CTRL_EXPAND_TMDS_L1_NBITS_LSB |
        29 << HSTX_CTRL_EXPAND_TMDS_L1_ROT_LSB   |
        1  << HSTX_CTRL_EXPAND_TMDS_L0_NBITS_LSB |
        26 << HSTX_CTRL_EXPAND_TMDS_L0_ROT_LSB;

    // Pixels (TMDS) come in 4 8-bit chunks. Control symbols (RAW) are an
    // entire 32-bit word.
    hstx_ctrl_hw->expand_shift =
        4 << HSTX_CTRL_EXPAND_SHIFT_ENC_N_SHIFTS_LSB |
        8 << HSTX_CTRL_EXPAND_SHIFT_ENC_SHIFT_LSB |
        1 << HSTX_CTRL_EXPAND_SHIFT_RAW_N_SHIFTS_LSB |
        0 << HSTX_CTRL_EXPAND_SHIFT_RAW_SHIFT_LSB;

    // Serial output config: clock period of 5 cycles, pop from command
    // expander every 5 cycles, shift the output shiftreg by 2 every cycle.
    hstx_ctrl_hw->csr = 0;
    hstx_ctrl_hw->csr =
        HSTX_CTRL_CSR_EXPAND_EN_BITS |
        5u << HSTX_CTRL_CSR_CLKDIV_LSB |
        5u << HSTX_CTRL_CSR_N_SHIFTS_LSB |
        2u << HSTX_CTRL_CSR_SHIFT_LSB |
        HSTX_CTRL_CSR_EN_BITS;

    // Note we are leaving the HSTX clock at the SDK default of 125 MHz; since
    // we shift out two bits per HSTX clock cycle, this gives us an output of
    // 250 Mbps, which is very close to the bit clock for 480p 60Hz (252 MHz).

    // HSTX outputs 0 through 7 appear on GPIO 12 through 19.
    // Spotpear pinout (active active low active):
    //
    //   GP12 CK-  GP13 CK+
    //   GP14 D0-  GP15 D0+  (Lane 0 - Blue)
    //   GP16 D1-  GP17 D1+  (Lane 1 - Green)
    //   GP18 D2-  GP19 D2+  (Lane 2 - Red)

    // Assign clock pair to bits 0-1 (GPIO 12-13)
    hstx_ctrl_hw->bit[0] = HSTX_CTRL_BIT0_CLK_BITS | HSTX_CTRL_BIT0_INV_BITS;  // GP12 = CK-
    hstx_ctrl_hw->bit[1] = HSTX_CTRL_BIT0_CLK_BITS;                             // GP13 = CK+

    // Data lanes: Spotpear uses sequential layout
    // Lane 0 -> bits 2-3 (GP14-15), Lane 1 -> bits 4-5 (GP16-17), Lane 2 -> bits 6-7 (GP18-19)
    for (uint lane = 0; lane < 3; ++lane) {
        int bit = 2 + lane * 2;
        uint32_t lane_data_sel_bits =
            (lane * 10    ) << HSTX_CTRL_BIT0_SEL_P_LSB |
            (lane * 10 + 1) << HSTX_CTRL_BIT0_SEL_N_LSB;
        hstx_ctrl_hw->bit[bit    ] = lane_data_sel_bits | HSTX_CTRL_BIT0_INV_BITS;  // D-
        hstx_ctrl_hw->bit[bit + 1] = lane_data_sel_bits;                             // D+
    }

    for (int i = 12; i <= 19; ++i) {
        gpio_set_function(i, 0); // HSTX
    }

    // Both channels are set up identically, to transfer a whole scanline and
    // then chain to the opposite channel.
    dma_channel_config c;
    c = dma_channel_get_default_config(DMACH_PING);
    channel_config_set_chain_to(&c, DMACH_PONG);
    channel_config_set_dreq(&c, DREQ_HSTX);
    dma_channel_configure(
        DMACH_PING,
        &c,
        &hstx_fifo_hw->fifo,
        vblank_line_vsync_off,
        count_of(vblank_line_vsync_off),
        false
    );
    c = dma_channel_get_default_config(DMACH_PONG);
    channel_config_set_chain_to(&c, DMACH_PING);
    channel_config_set_dreq(&c, DREQ_HSTX);
    dma_channel_configure(
        DMACH_PONG,
        &c,
        &hstx_fifo_hw->fifo,
        vblank_line_vsync_off,
        count_of(vblank_line_vsync_off),
        false
    );

    dma_hw->ints0 = (1u << DMACH_PING) | (1u << DMACH_PONG);
    dma_hw->inte0 = (1u << DMACH_PING) | (1u << DMACH_PONG);
    irq_set_exclusive_handler(DMA_IRQ_0, dma_irq_handler);
    irq_set_enabled(DMA_IRQ_0, true);

    bus_ctrl_hw->priority = BUSCTRL_BUS_PRIORITY_DMA_W_BITS | BUSCTRL_BUS_PRIORITY_DMA_R_BITS;

    dma_channel_start(DMACH_PING);

    // Animation loop
    while (1) {
        sleep_ms(16);  // ~60 FPS
        update_box();
        draw_frame();
    }
}
