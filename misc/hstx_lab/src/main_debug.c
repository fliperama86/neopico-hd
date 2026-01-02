// Debug version for Logic Analyzer capture
// Runs at HALF SPEED (126Mbps TMDS) for easier LA capture
// Adds GP20 trigger pulse at start of Data Island

#include <string.h>
#include <stdio.h>
#include "pico/stdlib.h"
#include "hardware/dma.h"
#include "hardware/gpio.h"
#include "hardware/irq.h"
#include "hardware/clocks.h"
#include "hardware/structs/bus_ctrl.h"
#include "hardware/structs/hstx_ctrl.h"
#include "hardware/structs/hstx_fifo.h"
#include "hardware/structs/clocks.h"
#include "data_packet.h"

// ----------------------------------------------------------------------------
// DVI/TMDS constants (same as main.c)

#define TMDS_CTRL_00 0x354u
#define TMDS_CTRL_01 0x0abu
#define TMDS_CTRL_10 0x154u
#define TMDS_CTRL_11 0x2abu

#define SYNC_V0_H0 (TMDS_CTRL_00 | (TMDS_CTRL_00 << 10) | (TMDS_CTRL_00 << 20))
#define SYNC_V0_H1 (TMDS_CTRL_01 | (TMDS_CTRL_00 << 10) | (TMDS_CTRL_00 << 20))
#define SYNC_V1_H0 (TMDS_CTRL_10 | (TMDS_CTRL_00 << 10) | (TMDS_CTRL_00 << 20))
#define SYNC_V1_H1 (TMDS_CTRL_11 | (TMDS_CTRL_00 << 10) | (TMDS_CTRL_00 << 20))

#define SYNC_V1_H1_WITH_PREAMBLE (TMDS_CTRL_11 | (TMDS_CTRL_01 << 10) | (TMDS_CTRL_00 << 20))
#define VIDEO_LEADING_GUARD_BAND (0x2ccu | (0x133u << 10) | (0x2ccu << 20))

#define W_PREAMBLE   8
#define W_GUARDBAND  2

#define DI_PREAMBLE_V0H0 (TMDS_CTRL_00 | (TMDS_CTRL_01 << 10) | (TMDS_CTRL_01 << 20))
#define DI_PREAMBLE_V0H1 (TMDS_CTRL_01 | (TMDS_CTRL_01 << 10) | (TMDS_CTRL_01 << 20))
#define DI_PREAMBLE_V1H0 (TMDS_CTRL_10 | (TMDS_CTRL_01 << 10) | (TMDS_CTRL_01 << 20))
#define DI_PREAMBLE_V1H1 (TMDS_CTRL_11 | (TMDS_CTRL_01 << 10) | (TMDS_CTRL_01 << 20))

// ----------------------------------------------------------------------------
// 640x480 @ 60Hz timing

#define MODE_H_FRONT_PORCH   18
#define MODE_H_SYNC_WIDTH    94
#define MODE_H_BACK_PORCH    48
#define MODE_H_ACTIVE_PIXELS 640

#define MODE_V_FRONT_PORCH   10
#define MODE_V_SYNC_WIDTH    2
#define MODE_V_BACK_PORCH    33
#define MODE_V_ACTIVE_LINES  480

#define MODE_H_TOTAL (MODE_H_FRONT_PORCH + MODE_H_SYNC_WIDTH + MODE_H_BACK_PORCH + MODE_H_ACTIVE_PIXELS)
#define MODE_V_TOTAL (MODE_V_FRONT_PORCH + MODE_V_SYNC_WIDTH + MODE_V_BACK_PORCH + MODE_V_ACTIVE_LINES)

// ----------------------------------------------------------------------------
// HSTX command defines

#define HSTX_CMD_RAW         (0x0u << 12)
#define HSTX_CMD_RAW_REPEAT  (0x1u << 12)
#define HSTX_CMD_TMDS        (0x2u << 12)
#define HSTX_CMD_TMDS_REPEAT (0x3u << 12)
#define HSTX_CMD_NOP         (0xfu << 12)

// ----------------------------------------------------------------------------
// Solid black framebuffer (predictable TMDS pattern)

static uint8_t framebuf[MODE_H_ACTIVE_PIXELS * MODE_V_ACTIVE_LINES];

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
    HSTX_CMD_RAW_REPEAT | (MODE_H_BACK_PORCH - W_PREAMBLE - W_GUARDBAND),
    SYNC_V1_H1,
    HSTX_CMD_RAW_REPEAT | W_PREAMBLE,
    SYNC_V1_H1_WITH_PREAMBLE,
    HSTX_CMD_RAW_REPEAT | W_GUARDBAND,
    VIDEO_LEADING_GUARD_BAND,
    HSTX_CMD_TMDS | MODE_H_ACTIVE_PIXELS
};

// ----------------------------------------------------------------------------
// Data Island support

#define HSYNC_BEFORE_DI  (MODE_H_SYNC_WIDTH - W_PREAMBLE - W_DATA_ISLAND)
#define VBLANK_DI_BUF_SIZE 64

// Multiple packet buffers for testing
#define DI_SLOT_ACR   0
#define DI_SLOT_AVI   1
#define DI_SLOT_AUDIO_INFO 2
#define DI_SLOT_AUDIO_SAMPLE 3
#define NUM_DI_SLOTS 4

static uint32_t vblank_di_buf[NUM_DI_SLOTS][VBLANK_DI_BUF_SIZE];
static uint32_t vblank_di_len[NUM_DI_SLOTS];

// Which packet to send (cycles through all for testing)
static uint32_t current_di_slot = 0;

// Audio timing for 48kHz
#define AUDIO_N   6144
#define AUDIO_CTS 25200
#define AUDIO_SAMPLE_FREQ 3

static void build_vblank_with_di_slot(const data_island_stream_t *di, bool vsync, int slot) {
    uint32_t *p = vblank_di_buf[slot];

    uint32_t sync_h0 = vsync ? SYNC_V0_H0 : SYNC_V1_H0;
    uint32_t sync_h1 = vsync ? SYNC_V0_H1 : SYNC_V1_H1;
    uint32_t preamble = vsync ? DI_PREAMBLE_V0H0 : DI_PREAMBLE_V1H0;

    *p++ = HSTX_CMD_RAW_REPEAT | MODE_H_FRONT_PORCH;
    *p++ = sync_h1;
    *p++ = HSTX_CMD_RAW_REPEAT | HSYNC_BEFORE_DI;
    *p++ = sync_h0;
    *p++ = HSTX_CMD_RAW_REPEAT | W_PREAMBLE;
    *p++ = preamble;
    *p++ = HSTX_CMD_RAW | W_DATA_ISLAND;

    for (int i = 0; i < N_DATA_ISLAND_WORDS; i++) {
        *p++ = (di->data[0][i] & 0x3ff) |
               ((di->data[1][i] & 0x3ff) << 10) |
               ((di->data[2][i] & 0x3ff) << 20);
        *p++ = ((di->data[0][i] >> 10) & 0x3ff) |
               (((di->data[1][i] >> 10) & 0x3ff) << 10) |
               (((di->data[2][i] >> 10) & 0x3ff) << 20);
    }

    *p++ = HSTX_CMD_RAW_REPEAT | (MODE_H_BACK_PORCH + MODE_H_ACTIVE_PIXELS);
    *p++ = sync_h1;
    *p++ = HSTX_CMD_NOP;

    vblank_di_len[slot] = p - vblank_di_buf[slot];
}

// ----------------------------------------------------------------------------
// DMA scanline logic

#define DMACH_PING 0
#define DMACH_PONG 1

// Trigger pin for Logic Analyzer
#define TRIGGER_PIN 3

static bool dma_pong = false;
static uint v_scanline = 0;
static bool vactive_cmdlist_posted = false;
static uint32_t di_used_count = 0;  // Debug: count how many times DI buffer is used

void __scratch_x("") dma_irq_handler() {
    uint ch_num = dma_pong ? DMACH_PONG : DMACH_PING;
    dma_channel_hw_t *ch = &dma_hw->ch[ch_num];
    dma_hw->intr = 1u << ch_num;
    dma_pong = !dma_pong;

    // Trigger ON first DI line (v_scanline == 12)
    // Stays LOW through all 4 DI lines (12-15) so LA captures all packets
    if (v_scanline == MODE_V_FRONT_PORCH + MODE_V_SYNC_WIDTH) {
        gpio_put(TRIGGER_PIN, 0);  // LOW = DI lines starting NOW
    } else if (v_scanline == MODE_V_FRONT_PORCH + MODE_V_SYNC_WIDTH + 4) {
        gpio_put(TRIGGER_PIN, 1);  // HIGH = all DI lines done
    }

    if (v_scanline >= MODE_V_FRONT_PORCH && v_scanline < (MODE_V_FRONT_PORCH + MODE_V_SYNC_WIDTH)) {
        ch->read_addr = (uintptr_t)vblank_line_vsync_on;
        ch->transfer_count = count_of(vblank_line_vsync_on);
    } else if (v_scanline == MODE_V_FRONT_PORCH + MODE_V_SYNC_WIDTH) {
        // Line 12: ACR packet
        ch->read_addr = (uintptr_t)vblank_di_buf[DI_SLOT_ACR];
        ch->transfer_count = vblank_di_len[DI_SLOT_ACR];
        di_used_count++;
    } else if (v_scanline == MODE_V_FRONT_PORCH + MODE_V_SYNC_WIDTH + 1) {
        // Line 13: AVI InfoFrame
        ch->read_addr = (uintptr_t)vblank_di_buf[DI_SLOT_AVI];
        ch->transfer_count = vblank_di_len[DI_SLOT_AVI];
    } else if (v_scanline == MODE_V_FRONT_PORCH + MODE_V_SYNC_WIDTH + 2) {
        // Line 14: Audio InfoFrame
        ch->read_addr = (uintptr_t)vblank_di_buf[DI_SLOT_AUDIO_INFO];
        ch->transfer_count = vblank_di_len[DI_SLOT_AUDIO_INFO];
    } else if (v_scanline == MODE_V_FRONT_PORCH + MODE_V_SYNC_WIDTH + 3) {
        // Line 15: Audio Sample packet
        ch->read_addr = (uintptr_t)vblank_di_buf[DI_SLOT_AUDIO_SAMPLE];
        ch->transfer_count = vblank_di_len[DI_SLOT_AUDIO_SAMPLE];
    } else if (v_scanline < MODE_V_FRONT_PORCH + MODE_V_SYNC_WIDTH + MODE_V_BACK_PORCH) {
        ch->read_addr = (uintptr_t)vblank_line_vsync_off;
        ch->transfer_count = count_of(vblank_line_vsync_off);
    } else if (!vactive_cmdlist_posted) {
        ch->read_addr = (uintptr_t)vactive_line;
        ch->transfer_count = count_of(vactive_line);
        vactive_cmdlist_posted = true;
    } else {
        uint line = v_scanline - (MODE_V_TOTAL - MODE_V_ACTIVE_LINES);
        ch->read_addr = (uintptr_t)&framebuf[line * MODE_H_ACTIVE_PIXELS];
        ch->transfer_count = MODE_H_ACTIVE_PIXELS / sizeof(uint32_t);
        vactive_cmdlist_posted = false;
    }

    if (!vactive_cmdlist_posted) {
        v_scanline = (v_scanline + 1) % MODE_V_TOTAL;
    }
}

// ----------------------------------------------------------------------------
// Main

#define LED_PIN 25

int main(void) {
    // **DEBUG MODE: USB-compatible speed for debugging**
    // 50MHz sys_clock → 10MHz pixel clock → 100Mbps TMDS
    // At 200Msps LA = 2 samples/bit (marginal but USB works!)
    set_sys_clock_khz(50000, true);

    stdio_init_all();

    gpio_init(LED_PIN);
    gpio_set_dir(LED_PIN, GPIO_OUT);

    // Trigger output for Logic Analyzer - active LOW
    gpio_init(TRIGGER_PIN);
    gpio_set_dir(TRIGGER_PIN, GPIO_OUT);
    gpio_put(TRIGGER_PIN, 1);  // Idle HIGH, falls when DI starts

    sleep_ms(1000);

    uint32_t sys_clk = clock_get_hz(clk_sys);
    uint32_t hstx_clk = clock_get_hz(clk_hstx);
    printf("\n\nDEBUG MODE - Half Speed\n");
    printf("DVI: sys=%luMHz, hstx=%luMHz, pixel=%luMHz\n",
           sys_clk / 1000000, hstx_clk / 1000000, hstx_clk / 5000000);
    printf("TMDS bit rate: %lu Mbps (ideal for 400Msps LA)\n", hstx_clk / 5000000 * 10);
    printf("Trigger on GP%d FALLING edge (active LOW)\n", TRIGGER_PIN);

    // Solid black framebuffer for predictable video pattern
    memset(framebuf, 0, sizeof(framebuf));
    printf("Framebuffer: solid black\n");

    // Build all Data Island packets (like main.c)
    {
        data_packet_t pkt;
        data_island_stream_t di;

        // ACR packet (Audio Clock Regeneration)
        packet_create_acr(&pkt, AUDIO_N, AUDIO_CTS);
        encode_data_island(&di, &pkt, true, false);
        build_vblank_with_di_slot(&di, false, DI_SLOT_ACR);
        printf("ACR: N=%d, CTS=%d, buf=%lu words\n", AUDIO_N, AUDIO_CTS, vblank_di_len[DI_SLOT_ACR]);

        // AVI InfoFrame (required for HDMI mode)
        packet_create_avi_infoframe(&pkt, 0, 1);  // RGB, VIC=1 (640x480p60)
        encode_data_island(&di, &pkt, true, false);
        build_vblank_with_di_slot(&di, false, DI_SLOT_AVI);
        printf("AVI InfoFrame: type=0x%02X, buf=%lu words\n", pkt.header[0], vblank_di_len[DI_SLOT_AVI]);

        // Audio InfoFrame
        packet_create_audio_infoframe(&pkt, 2, AUDIO_SAMPLE_FREQ, 1);  // 2ch, 48kHz, 16-bit
        encode_data_island(&di, &pkt, true, false);
        build_vblank_with_di_slot(&di, false, DI_SLOT_AUDIO_INFO);
        printf("Audio InfoFrame: type=0x%02X, buf=%lu words\n", pkt.header[0], vblank_di_len[DI_SLOT_AUDIO_INFO]);

        // Audio Sample packet (one test packet with silence)
        int16_t silence[8] = {0};
        packet_create_audio_sample(&pkt, silence, 4, true);  // 4 samples, frame start
        encode_data_island(&di, &pkt, true, false);
        build_vblank_with_di_slot(&di, false, DI_SLOT_AUDIO_SAMPLE);
        printf("Audio Sample: type=0x%02X, buf=%lu words\n", pkt.header[0], vblank_di_len[DI_SLOT_AUDIO_SAMPLE]);
    }

    // Configure HSTX TMDS encoder for RGB332
    hstx_ctrl_hw->expand_tmds =
        2  << HSTX_CTRL_EXPAND_TMDS_L2_NBITS_LSB |
        0  << HSTX_CTRL_EXPAND_TMDS_L2_ROT_LSB   |
        2  << HSTX_CTRL_EXPAND_TMDS_L1_NBITS_LSB |
        29 << HSTX_CTRL_EXPAND_TMDS_L1_ROT_LSB   |
        1  << HSTX_CTRL_EXPAND_TMDS_L0_NBITS_LSB |
        26 << HSTX_CTRL_EXPAND_TMDS_L0_ROT_LSB;

    hstx_ctrl_hw->expand_shift =
        4 << HSTX_CTRL_EXPAND_SHIFT_ENC_N_SHIFTS_LSB |
        8 << HSTX_CTRL_EXPAND_SHIFT_ENC_SHIFT_LSB |
        1 << HSTX_CTRL_EXPAND_SHIFT_RAW_N_SHIFTS_LSB |
        0 << HSTX_CTRL_EXPAND_SHIFT_RAW_SHIFT_LSB;

    hstx_ctrl_hw->csr = 0;
    hstx_ctrl_hw->csr =
        HSTX_CTRL_CSR_EXPAND_EN_BITS |
        5u << HSTX_CTRL_CSR_CLKDIV_LSB |
        5u << HSTX_CTRL_CSR_N_SHIFTS_LSB |
        2u << HSTX_CTRL_CSR_SHIFT_LSB |
        HSTX_CTRL_CSR_EN_BITS;

    // Pin config (same as main.c)
    hstx_ctrl_hw->bit[0] = HSTX_CTRL_BIT0_CLK_BITS | HSTX_CTRL_BIT0_INV_BITS;
    hstx_ctrl_hw->bit[1] = HSTX_CTRL_BIT0_CLK_BITS;
    for (uint lane = 0; lane < 3; ++lane) {
        int bit = 2 + lane * 2;
        uint32_t lane_data_sel_bits =
            (lane * 10    ) << HSTX_CTRL_BIT0_SEL_P_LSB |
            (lane * 10 + 1) << HSTX_CTRL_BIT0_SEL_N_LSB;
        hstx_ctrl_hw->bit[bit    ] = lane_data_sel_bits | HSTX_CTRL_BIT0_INV_BITS;
        hstx_ctrl_hw->bit[bit + 1] = lane_data_sel_bits;
    }

    for (int i = 12; i <= 19; ++i) {
        gpio_set_function(i, 0);
    }

    // Setup ping-pong DMA
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

    printf("Starting HSTX...\n");
    dma_channel_start(DMACH_PING);

    uint32_t last_di_count = 0;
    while (1) {
        // Slow blink to indicate debug mode
        gpio_put(LED_PIN, 1);
        sleep_ms(500);
        gpio_put(LED_PIN, 0);
        sleep_ms(500);

        // Print DI usage stats every second
        if (di_used_count != last_di_count) {
            printf("DI used %lu times (should be ~30/sec at 30fps)\n", di_used_count);
            last_di_count = di_used_count;
        }
    }
}
