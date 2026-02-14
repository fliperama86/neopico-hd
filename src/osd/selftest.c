#include "selftest.h"

#include "pico/time.h"

#include "hardware/dma.h"
#include "hardware/irq.h"
#include "hardware/pio.h"

#include "mvs_pins.h"
#include "osd.h"
#include "selftest_sample.pio.h"

// --- PIO+DMA config ---
#define SELFTEST_PIO pio0
#define PIO_IN_BASE 6       // pin index 6 = GPIO 22 (GPIOBASE=16)
#define PIO_CLK_DIV 12600   // 126MHz / 12600 = 10kHz
#define DMA_BATCH 64        // samples per DMA transfer
#define BATCHES_PER_SEC 156 // ~10000 / 64

static uint32_t __attribute__((aligned(256))) dma_buf[DMA_BATCH];
static uint selftest_sm;
static uint selftest_offset;
static int selftest_dma;
static volatile bool selftest_active;

// --- Accumulator (written by DMA ISR on Core 0) ---
static volatile uint32_t accum_high;
static volatile uint32_t accum_low;
static volatile uint32_t batch_count;

// --- Results (snapshot by ISR, consumed by Core 1 render) ---
static volatile uint32_t result_toggled;
static volatile bool selftest_update_ready;

// --- Bit positions in the 23-bit PIO sample word ---
// Bit N = GPIO (N + 22)
#define ST_DAT (1u << 0)     // GPIO 22
#define ST_WS (1u << 1)      // GPIO 23
#define ST_BCK (1u << 2)     // GPIO 24
#define ST_CSYNC (1u << 5)   // GPIO 27
#define ST_PCLK (1u << 6)    // GPIO 28
#define ST_B4 (1u << 7)      // GPIO 29
#define ST_B3 (1u << 8)      // GPIO 30
#define ST_B2 (1u << 9)      // GPIO 31
#define ST_B1 (1u << 10)     // GPIO 32
#define ST_B0 (1u << 11)     // GPIO 33
#define ST_G4 (1u << 12)     // GPIO 34
#define ST_G3 (1u << 13)     // GPIO 35
#define ST_G2 (1u << 14)     // GPIO 36
#define ST_G1 (1u << 15)     // GPIO 37
#define ST_G0 (1u << 16)     // GPIO 38
#define ST_R4 (1u << 17)     // GPIO 39
#define ST_R3 (1u << 18)     // GPIO 40
#define ST_R2 (1u << 19)     // GPIO 41
#define ST_R1 (1u << 20)     // GPIO 42
#define ST_R0 (1u << 21)     // GPIO 43
#define ST_SHADOW (1u << 22) // GPIO 44
#define ST_ALL 0x7FFFFFu

static const uint32_t red_bits[5] = {ST_R4, ST_R3, ST_R2, ST_R1, ST_R0};
static const uint32_t green_bits[5] = {ST_G4, ST_G3, ST_G2, ST_G1, ST_G0};
static const uint32_t blue_bits[5] = {ST_B4, ST_B3, ST_B2, ST_B1, ST_B0};

#define GLYPH_CHECK '\x01'
#define GLYPH_CROSS '\x02'

// Layout
#define COL_L 8
#define BITS_X 64
#define BITS_SPC 16

// Render state
static uint8_t render_phase;    // 0=idle, 1-5=render phases
static uint32_t render_toggled; // local copy of result_toggled
static bool dat_seen;
static uint8_t dat_countdown; // decrements per update, ~5s hold

// ============================================================================
// DMA ISR — runs on Core 0, accumulates sample data
// ============================================================================

static void __not_in_flash_func(selftest_dma_isr)(void)
{
    dma_hw->ints1 = 1u << selftest_dma; // clear IRQ flag

    if (!selftest_active)
        return;

    // Accumulate high/low bits from this batch
    uint32_t hi = accum_high;
    uint32_t lo = accum_low;
    for (uint32_t i = 0; i < DMA_BATCH; i++) {
        uint32_t s = dma_buf[i];
        hi |= s;
        lo |= ~s;
    }
    accum_high = hi;
    accum_low = lo;

    uint32_t count = batch_count + 1;
    if (count >= BATCHES_PER_SEC) {
        // 1-second snapshot
        result_toggled = hi & lo & ST_ALL;
        selftest_update_ready = true;
        accum_high = 0;
        accum_low = 0;
        count = 0;
    }
    batch_count = count;

    // Restart DMA for next batch
    dma_channel_set_write_addr(selftest_dma, dma_buf, false);
    dma_channel_set_trans_count(selftest_dma, DMA_BATCH, true);
}

// ============================================================================
// Init — called once from Core 0 before core1 launch
// ============================================================================

void selftest_init(void)
{
    PIO pio = SELFTEST_PIO;

    // GPIOBASE=16: pin index 0-31 → GPIO 16-47
    *(volatile uint32_t *)((uintptr_t)pio + 0x168) = 16;

    selftest_offset = pio_add_program(pio, &selftest_sample_program);
    selftest_sm = (uint)pio_claim_unused_sm(pio, true);

    // Claim DMA channel and set up IRQ handler (but don't start)
    selftest_dma = dma_claim_unused_channel(true);

    irq_set_exclusive_handler(DMA_IRQ_1, selftest_dma_isr);
    irq_set_priority(DMA_IRQ_1, 0x80); // lower priority than HSTX (0x00)
    dma_channel_set_irq1_enabled(selftest_dma, true);
    irq_set_enabled(DMA_IRQ_1, true);

    selftest_active = false;
}

// ============================================================================
// Start/Stop — called from Core 1 background task on OSD toggle
// ============================================================================

void selftest_start(void)
{
    PIO pio = SELFTEST_PIO;

    // Reset accumulators
    accum_high = 0;
    accum_low = 0;
    batch_count = 0;
    result_toggled = 0;
    selftest_update_ready = false;
    render_phase = 0;
    dat_seen = false;
    dat_countdown = 0;

    // Configure and enable SM
    pio_sm_config c = selftest_sample_program_get_default_config(selftest_offset);
    sm_config_set_in_pins(&c, PIO_IN_BASE);
    sm_config_set_in_shift(&c, false, true, 23); // left shift, autopush, 23 bits
    sm_config_set_clkdiv_int_frac(&c, PIO_CLK_DIV, 0);

    pio_sm_init(pio, selftest_sm, selftest_offset, &c);

    // Drain RX FIFO
    while (!pio_sm_is_rx_fifo_empty(pio, selftest_sm))
        (void)pio_sm_get(pio, selftest_sm);

    selftest_active = true;

    // Enable SM
    pio_sm_set_enabled(pio, selftest_sm, true);

    // Configure and start DMA
    dma_channel_config dc = dma_channel_get_default_config(selftest_dma);
    channel_config_set_transfer_data_size(&dc, DMA_SIZE_32);
    channel_config_set_read_increment(&dc, false);
    channel_config_set_write_increment(&dc, true);
    channel_config_set_dreq(&dc, pio_get_dreq(pio, selftest_sm, false));

    dma_channel_configure(selftest_dma, &dc, dma_buf, &pio->rxf[selftest_sm], DMA_BATCH, true);
}

void selftest_stop(void)
{
    selftest_active = false;

    // Abort DMA
    dma_channel_abort(selftest_dma);

    // Disable SM
    pio_sm_set_enabled(SELFTEST_PIO, selftest_sm, false);
}

// ============================================================================
// OSD rendering — phase-based, max ~2.5us per call
// ============================================================================

static void render_icon(int x, int y, bool ok)
{
    uint16_t color = ok ? OSD_COLOR_GREEN : OSD_COLOR_RED;
    osd_putchar_color(x, y, ok ? GLYPH_CHECK : GLYPH_CROSS, color);
}

void selftest_reset(void)
{
    render_phase = 0;
    dat_seen = false;
    dat_countdown = 0;

    // Static layout — labels stay gray, only icons update dynamically
    osd_clear();
    osd_puts(16, 16, "NeoPico-HD Self Test");

    osd_puts(COL_L, 32, "Video");
    osd_puts_color(COL_L, 40, "CSYNC", OSD_COLOR_GRAY);
    osd_puts_color(80, 40, "PCLK", OSD_COLOR_GRAY);
    osd_puts_color(152, 40, "Shadow", OSD_COLOR_GRAY);

    osd_puts_color(BITS_X, 48, "4", OSD_COLOR_GRAY);
    osd_puts_color(BITS_X + BITS_SPC, 48, "3", OSD_COLOR_GRAY);
    osd_puts_color(BITS_X + (BITS_SPC * 2), 48, "2", OSD_COLOR_GRAY);
    osd_puts_color(BITS_X + (BITS_SPC * 3), 48, "1", OSD_COLOR_GRAY);
    osd_puts_color(BITS_X + (BITS_SPC * 4), 48, "0", OSD_COLOR_GRAY);

    osd_puts_color(COL_L, 56, "Red", OSD_COLOR_GRAY);
    osd_puts_color(COL_L, 64, "Green", OSD_COLOR_GRAY);
    osd_puts_color(COL_L, 72, "Blue", OSD_COLOR_GRAY);

    osd_puts(COL_L, 88, "Audio");
    osd_puts_color(COL_L, 96, "BCK", OSD_COLOR_GRAY);
    osd_puts_color(80, 96, "WS", OSD_COLOR_GRAY);
    osd_puts_color(144, 96, "DAT", OSD_COLOR_GRAY);
}

void selftest_render(void)
{
    // Phase 0: idle — wait for ISR to signal 1s update
    if (render_phase == 0) {
        if (!selftest_update_ready)
            return;
        selftest_update_ready = false;
        render_toggled = result_toggled;

        // DAT 5s hold
        if (render_toggled & ST_DAT) {
            dat_seen = true;
            dat_countdown = 5; // 5 update cycles = 5 seconds
        } else if (dat_countdown > 0) {
            dat_countdown--;
        }

        render_phase = 1;
        return;
    }

    switch (render_phase) {
        case 1:
            // Video single-bit: 3 icons
            render_icon(56, 40, render_toggled & ST_CSYNC);
            render_icon(120, 40, render_toggled & ST_PCLK);
            render_icon(208, 40, render_toggled & ST_SHADOW);
            render_phase = 2;
            return;
        case 2:
            // Red: 5 icons
            for (int i = 0; i < 5; i++)
                render_icon(BITS_X + (i * BITS_SPC), 56, render_toggled & red_bits[i]);
            render_phase = 3;
            return;
        case 3:
            // Green: 5 icons
            for (int i = 0; i < 5; i++)
                render_icon(BITS_X + (i * BITS_SPC), 64, render_toggled & green_bits[i]);
            render_phase = 4;
            return;
        case 4:
            // Blue: 5 icons
            for (int i = 0; i < 5; i++)
                render_icon(BITS_X + (i * BITS_SPC), 72, render_toggled & blue_bits[i]);
            render_phase = 5;
            return;
        case 5: {
            // Audio: 3 icons
            bool dat = dat_seen && (dat_countdown > 0);
            render_icon(40, 96, render_toggled & ST_BCK);
            render_icon(112, 96, render_toggled & ST_WS);
            render_icon(176, 96, dat);
            render_phase = 0;
            return;
        }
    }
}
