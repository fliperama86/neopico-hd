/**
 * MVS Video Capture Module
 * PIO-based line-by-line capture from Neo Geo MVS
 *
 * Streaming architecture: captures lines directly to ring buffer
 */

#include "video_capture.h"

#include "pico_hdmi/video_output_rt.h"

#include "pico/stdlib.h"
#include "pico/sync.h"

#include "hardware/clocks.h"
#include "hardware/dma.h"
#include "hardware/irq.h"
#include "hardware/pio.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "audio_subsystem.h"
#include "hardware_config.h"
#include "line_ring.h"
#include "mvs_pins.h"
#include "pico.h"
#include "tusb.h"
#include "video_capture.pio.h"

// =============================================================================
// Feature Flags
// =============================================================================

// SHADOW processing (no DARK pin in this hardware layout).
// When enabled, uses 32KB LUT indexed by RGB555.
// Disabled: was causing poor image quality (RF-like); re-enable if needed.
#ifndef ENABLE_DARK_SHADOW
#define ENABLE_DARK_SHADOW 0
#endif

#if ENABLE_DARK_SHADOW
// 32KB LUT - indexed by (r | g<<5 | b<<10 | dark<<15); built from PIO R/G/B bits
static uint16_t g_pixel_lut[65536] __attribute__((aligned(4)));

static void generate_pixel_lut(void)
{
    // LUT indexed by: [15:DARK][14-10:B][9-5:G][4-0:R]
    for (uint32_t idx = 0; idx < 65536; idx++) {
        uint32_t r5 = idx & 0x1F;
        uint32_t g5 = (idx >> 5) & 0x1F;
        uint32_t b5 = (idx >> 10) & 0x1F;
        uint32_t dark = (idx >> 15) & 1;

        // Expand 5->8 bit
        uint32_t r8 = (r5 << 3) | (r5 >> 2);
        uint32_t g8 = (g5 << 3) | (g5 >> 2);
        uint32_t b8 = (b5 << 3) | (b5 >> 2);

        // Apply DARK (-4 with saturation)
        if (dark) {
            r8 = (r8 > 4) ? r8 - 4 : 0;
            g8 = (g8 > 4) ? g8 - 4 : 0;
            b8 = (b8 > 4) ? b8 - 4 : 0;
        }

        // Pack as RGB565
        g_pixel_lut[idx] = ((r8 >> 3) << 11) | ((g8 >> 2) << 5) | (b8 >> 3);
    }
}
#endif

// =============================================================================
// MVS Timing Constants
// =============================================================================

#define H_THRESHOLD 288
#define NEO_H_TOTAL 384
#define NEO_H_ACTIVE 320
#define H_SKIP_START 28
#define H_SKIP_END 36
#define V_SKIP_LINES 16
#define NEO_V_ACTIVE 224

// PIO IRQ index: sync SM raises this on every line push for event-driven vsync (no polling)
#define MVS_SYNC_IRQ_INDEX 0
// No-signal timeout: only used to detect cable unplug / loss of signal; normal path is IRQ-driven
#define MVS_NO_SIGNAL_TIMEOUT_MS 100

// =============================================================================
// State
// =============================================================================

static uint g_mvs_height = 0;

PIO g_pio_mvs = NULL;
static uint g_sm_sync = 0;
static uint g_offset_sync = 0;
static uint g_sm_pixel = 0;
static uint g_offset_pixel = 0;

static int g_dma_chan = -1;
static uint32_t g_line_buffers[2][NEO_H_TOTAL]; // Ping-pong buffers for line capture

static volatile uint32_t g_frame_count = 0;

static int g_skip_start_words = 0;
static int g_active_words = 0;
static int g_line_words = 0;

// Semaphore released by sync IRQ handler when vsync is detected (one release per frame)
static semaphore_t g_vsync_sem;

// =============================================================================
// Pixel Conversion
// =============================================================================

// Custom PCB color correction (try combinations if colors are wrong)
// - Invert: channel logic inverted on PCB (XOR with 0x1F)
// - Reverse: MSB/LSB swapped on PCB (reverse bit order within 5-bit channel)
// - Raw mask: XOR with 15-bit color before extracting R/G/B. Bit layout in mask:
//     bits 0-4: B (1=invert B0, 2=B1, 4=B2, 8=B3, 16=B4)
//     bits 5-9: G (0x20=G0 .. 0x200=G4)
//     bits 10-14: R (0x400=R0 .. 0x4000=R4)
//   Examples: 0x001F=invert B, 0x03E0=invert G, 0x7C00=invert R
#define MVS_INVERT_R 0
#define MVS_INVERT_G 0
#define MVS_INVERT_B 0
// PCB wires MSB (R4/G4/B4) to lower GPIO → capture has MSB in LSB of each 5-bit field; reverse all three
#define MVS_REVERSE_R 1
#define MVS_REVERSE_G 1
#define MVS_REVERSE_B 1
#define MVS_RAW_COLOR_MASK 0
// Reverse entire 15-bit color word (bus wired MSB↔LSB on PCB)
#define MVS_REVERSE_15BIT 0
// Black level: clamp 5-bit R/G/B at or below this to 0 (fixes gray blacks from MVS pedestal). 0 = off, 1–3 = clamp.
#define MVS_BLACK_LEVEL_CLAMP 2

static inline uint32_t mvs_reverse_15(uint32_t x)
{
    x &= 0x7FFF;
    return ((x & 0x0001U) << 14) | ((x & 0x0002U) << 12) | ((x & 0x0004U) << 10) | ((x & 0x0008U) << 8) |
           ((x & 0x0010U) << 6) | ((x & 0x0020U) << 4) | ((x & 0x0040U) << 2) | ((x & 0x0080U) << 0) |
           ((x & 0x0100U) >> 2) | ((x & 0x0200U) >> 4) | ((x & 0x0400U) >> 6) | ((x & 0x0800U) >> 8) |
           ((x & 0x1000U) >> 10) | ((x & 0x2000U) >> 12) | ((x & 0x4000U) >> 14);
}

static inline uint32_t mvs_correct_5bit(uint32_t x, int invert, int reverse)
{
    if (reverse) {
        x = ((x & 1U) << 4) | ((x & 2U) << 2) | (x & 4U) | ((x & 8U) >> 2) | ((x & 16U) >> 4);
    }
    if (invert) {
        x ^= 0x1F;
    }
    return x & 0x1F;
}

// 32K LUT: raw 15-bit capture -> RGB565 (corrections baked in at init; one lookup per pixel)
static uint16_t g_color_correct_lut[32768] __attribute__((aligned(4)));

static void generate_color_correct_lut(void)
{
    for (uint32_t idx = 0; idx < 32768U; idx++) {
        uint32_t r5 = mvs_correct_5bit((idx >> 10) & 0x1F, MVS_INVERT_R, MVS_REVERSE_R);
        uint32_t g5 = mvs_correct_5bit((idx >> 5) & 0x1F, MVS_INVERT_G, MVS_REVERSE_G);
        uint32_t b5 = mvs_correct_5bit(idx & 0x1F, MVS_INVERT_B, MVS_REVERSE_B);
        // Black-level clamp: MVS often has a slight pedestal so "black" is a few LSB above 0
        if (MVS_BLACK_LEVEL_CLAMP > 0 && r5 <= MVS_BLACK_LEVEL_CLAMP && g5 <= MVS_BLACK_LEVEL_CLAMP &&
            b5 <= MVS_BLACK_LEVEL_CLAMP) {
            g_color_correct_lut[idx] = 0;
        } else {
            g_color_correct_lut[idx] = (uint16_t)((r5 << 11) | (g5 << 6) | (g5 >> 4) | b5);
        }
    }
}

// Direct conversion: RGB555 (from PIO) -> RGB565 (for HDMI)
// PIO captures 18 bits: [17:SHADOW][16-12:R][11-7:G][6-2:B][1:PCLK][0:CSYNC]
// Hot path: one LUT lookup (no per-pixel reverse/pack).
static inline uint16_t convert_pixel(uint32_t raw)
{
    uint32_t color15 = ((raw >> 2) & 0x7FFF) ^ (MVS_RAW_COLOR_MASK & 0x7FFF);
#if MVS_REVERSE_15BIT
    color15 = mvs_reverse_15(color15);
#endif
#if ENABLE_DARK_SHADOW
    if (__builtin_expect((raw >> 17) & 1, 0)) {
        uint32_t r5 = mvs_correct_5bit((color15 >> 10) & 0x1F, MVS_INVERT_R, MVS_REVERSE_R);
        uint32_t g5 = mvs_correct_5bit((color15 >> 5) & 0x1F, MVS_INVERT_G, MVS_REVERSE_G);
        uint32_t b5 = mvs_correct_5bit(color15 & 0x1F, MVS_INVERT_B, MVS_REVERSE_B);
        r5 >>= 1;
        g5 >>= 1;
        b5 >>= 1;
        return g_pixel_lut[(1 << 15) | (b5 << 10) | (g5 << 5) | r5];
    }
#endif
    return g_color_correct_lut[color15];
}

// =============================================================================
// MVS Sync Detection
// =============================================================================

static inline void drain_sync_fifo(PIO pio, uint sm)
{
    while (!pio_sm_is_rx_fifo_empty(pio, sm)) {
        pio_sm_get(pio, sm);
    }
}

// Runs in IRQ context: one word per line from sync PIO; detect vsync (long pulse after 8 short) and release semaphore
static void sync_irq_handler(void)
{
    pio_interrupt_clear(g_pio_mvs, MVS_SYNC_IRQ_INDEX);
    if (pio_sm_is_rx_fifo_empty(g_pio_mvs, g_sm_sync))
        return;

    uint32_t h_ctr = pio_sm_get(g_pio_mvs, g_sm_sync);
    bool is_short_pulse = (h_ctr <= H_THRESHOLD);

    static uint32_t equ_count = 0;
    static bool in_vsync = false;

    if (!in_vsync) {
        if (is_short_pulse) {
            equ_count++;
        } else {
            if (equ_count >= 8)
                in_vsync = true;
            equ_count = 0;
        }
    } else {
        if (is_short_pulse) {
            equ_count++;
        } else {
            equ_count = 0;
            in_vsync = false;
            sem_release(&g_vsync_sem);
        }
    }
}

// =============================================================================
// Hardware Reset
// =============================================================================

static void video_capture_reset_hardware(void)
{
    // 1. Disable both state machines to stop any pending operations
    pio_sm_set_enabled(g_pio_mvs, g_sm_sync, false);
    pio_sm_set_enabled(g_pio_mvs, g_sm_pixel, false);

    // 2. Clear FIFOs to remove any stale data
    pio_sm_clear_fifos(g_pio_mvs, g_sm_sync);
    pio_sm_clear_fifos(g_pio_mvs, g_sm_pixel);

    // 3. Reset PCs and re-initialize state
    pio_sm_restart(g_pio_mvs, g_sm_sync);
    pio_sm_restart(g_pio_mvs, g_sm_pixel);

    // Jump sync SM to entry point and reset X register (counter)
    pio_sm_exec(g_pio_mvs, g_sm_sync, pio_encode_jmp(g_offset_sync));
    pio_sm_exec(g_pio_mvs, g_sm_sync, pio_encode_set(pio_x, 0));

    // Jump pixel SM to entry point (the PULL instruction)
    pio_sm_exec(g_pio_mvs, g_sm_pixel, pio_encode_jmp(g_offset_pixel));

    // Re-enable SMs
    pio_sm_set_enabled(g_pio_mvs, g_sm_sync, true);
    pio_sm_set_enabled(g_pio_mvs, g_sm_pixel, true);

    // 4. Re-feed the pixel count to the Pixel SM
    pio_sm_put_blocking(g_pio_mvs, g_sm_pixel, NEO_H_TOTAL - 1);

    // 5. Clear any pending trigger IRQ and sync data-ready IRQ
    pio_interrupt_clear(g_pio_mvs, 4);
    pio_interrupt_clear(g_pio_mvs, MVS_SYNC_IRQ_INDEX);
}

// =============================================================================
// Public API
// =============================================================================

void video_capture_init(uint mvs_height)
{
    g_mvs_height = mvs_height;

#if ENABLE_DARK_SHADOW
    generate_pixel_lut();
#endif
    generate_color_correct_lut();

    // 18-bit capture: 1 pixel per word
    g_skip_start_words = H_SKIP_START;
    g_active_words = NEO_H_ACTIVE;
    g_line_words = NEO_H_TOTAL;

    // Initialize PIO blocks
    pio_clear_instruction_memory(pio0);
    pio_clear_instruction_memory(pio1);
    pio_set_gpio_base(pio0, 0);
    pio_set_gpio_base(pio1, 0);

    g_pio_mvs = pio1;

    // 1. Force PIO1 GPIOBASE to 16 (pin index N = GP(N+16); GP27 = index 11)
    *(volatile uint32_t *)((uintptr_t)g_pio_mvs + 0x168) = 16;

    // 2. Add programs
    pio_clear_instruction_memory(g_pio_mvs);
    g_offset_sync = pio_add_program(g_pio_mvs, &mvs_sync_4a_program);
    g_offset_pixel = pio_add_program(g_pio_mvs, &mvs_pixel_capture_program);

    // 3. Claim SMs
    g_sm_sync = (uint)pio_claim_unused_sm(g_pio_mvs, true);
    g_sm_pixel = (uint)pio_claim_unused_sm(g_pio_mvs, true);

    // 4. Setup GPIOs GP27-44 (CSYNC, PCLK, B, G, R, SHADOW)
    for (uint i = PIN_MVS_BASE; i <= PIN_MVS_SHADOW; i++) {
        pio_gpio_init(g_pio_mvs, i);
        gpio_disable_pulls(i);
        gpio_set_input_enabled(i, true);
        gpio_set_input_hysteresis_enabled(i, true);
    }

    // 5. Configure Sync SM: CSYNC = GP27 (pin index 11 with GPIOBASE=16)
    pio_sm_config c = mvs_sync_4a_program_get_default_config(g_offset_sync);
    sm_config_set_clkdiv(&c, 1.0F);
    sm_config_set_in_shift(&c, false, false, 32);
    sm_config_set_fifo_join(&c, PIO_FIFO_JOIN_RX);
    pio_sm_init(g_pio_mvs, g_sm_sync, g_offset_sync, &c);

    uint pin_idx_sync = PIN_MVS_BASE - 16; // 11: GP27 = CSYNC (wait/jmp pin)
    g_pio_mvs->sm[g_sm_sync].pinctrl = (g_pio_mvs->sm[g_sm_sync].pinctrl & ~0x000f8000) | (pin_idx_sync << 15);
    g_pio_mvs->sm[g_sm_sync].execctrl = (g_pio_mvs->sm[g_sm_sync].execctrl & ~0x1f000000) | (pin_idx_sync << 24);

    // 6. Configure Pixel SM: IN_BASE = GP27 (pin index 11), capture GP27-44
    pio_sm_config pc = mvs_pixel_capture_program_get_default_config(g_offset_pixel);
    sm_config_set_clkdiv(&pc, 1.0F);
    sm_config_set_in_shift(&pc, false, true, 18);
    pio_sm_init(g_pio_mvs, g_sm_pixel, g_offset_pixel, &pc);

    uint pin_idx_pixel = PIN_MVS_BASE - 16; // 11: first pin of 18-pin capture
    g_pio_mvs->sm[g_sm_pixel].pinctrl = (g_pio_mvs->sm[g_sm_pixel].pinctrl & ~0x000f8000) | (pin_idx_pixel << 15);

    // 7. Start
    pio_interrupt_clear(g_pio_mvs, 4);
    pio_sm_exec(g_pio_mvs, g_sm_sync, pio_encode_set(pio_x, 0));
    pio_sm_set_enabled(g_pio_mvs, g_sm_sync, true);
    pio_sm_set_enabled(g_pio_mvs, g_sm_pixel, true);

    // 7a. Initialize Pixel SM with pixel count
    pio_sm_put_blocking(g_pio_mvs, g_sm_pixel, NEO_H_TOTAL - 1);

    // 8. Configure DMA for Async Pixel Capture
    g_dma_chan = dma_claim_unused_channel(true);
    dma_channel_config dc = dma_channel_get_default_config(g_dma_chan);
    channel_config_set_read_increment(&dc, false);
    channel_config_set_write_increment(&dc, true);
    channel_config_set_dreq(&dc, pio_get_dreq(g_pio_mvs, g_sm_pixel, false));
    dma_channel_configure(g_dma_chan, &dc, g_line_buffers[0], &g_pio_mvs->rxf[g_sm_pixel], 0, false);

    // 9. Sync IRQ: event-driven vsync (no polling). Sync SM raises IRQ 0 on every line push.
    sem_init(&g_vsync_sem, 0, 2);
    pio_interrupt_clear(g_pio_mvs, MVS_SYNC_IRQ_INDEX);
    g_pio_mvs->inte0 |= (1U << MVS_SYNC_IRQ_INDEX);
    irq_set_exclusive_handler(PIO1_IRQ_0, sync_irq_handler);
    irq_set_enabled(PIO1_IRQ_0, true);
}

// Genlock: 60 Hz = 126 MHz sysclk, ~59.18 Hz = 124.1 MHz (nominal MVS)
#define SYS_CLK_60HZ_KHZ 126000
#define SYS_CLK_GENLOCK_KHZ 124100

void video_capture_run(void)
{
    // One-time: wait for first vsync (IRQ-driven) then drain FIFO for clean phase
    if (sem_acquire_timeout_ms(&g_vsync_sem, 500)) {
        drain_sync_fifo(g_pio_mvs, g_sm_sync);
    }

    while (1) {
        g_frame_count++;

        if (!sem_acquire_timeout_ms(&g_vsync_sem, MVS_NO_SIGNAL_TIMEOUT_MS)) {
            video_capture_reset_hardware();
            tud_task();
            continue;
        }

        // Signal VSYNC to Core 1
        line_ring_vsync();

        drain_sync_fifo(g_pio_mvs, g_sm_sync);

        // Reset Pixel Capture SM for frame alignment
        pio_sm_set_enabled(g_pio_mvs, g_sm_pixel, false);
        pio_sm_clear_fifos(g_pio_mvs, g_sm_pixel);
        pio_sm_exec(g_pio_mvs, g_sm_pixel, pio_encode_jmp(g_offset_pixel + 2));
        pio_sm_set_enabled(g_pio_mvs, g_sm_pixel, true);

        // Prepare first DMA
        dma_channel_set_trans_count(g_dma_chan, g_line_words, false);
        dma_channel_set_write_addr(g_dma_chan, g_line_buffers[0], true);

        // Trigger capture
        pio_interrupt_clear(g_pio_mvs, 4);
        pio_sm_exec(g_pio_mvs, g_sm_sync, pio_encode_irq_set(false, 4));

        // Skip V border lines
        for (int skip = 0; skip < V_SKIP_LINES; skip++) {
            dma_channel_wait_for_finish_blocking(g_dma_chan);
            dma_channel_set_trans_count(g_dma_chan, g_line_words, false);
            dma_channel_set_write_addr(g_dma_chan, g_line_buffers[0], true);
        }

        // Capture active lines into ring buffer
        uint8_t buf_idx = 0;
        for (uint16_t line = 0; line + 1 < g_mvs_height; line++) {
            uint16_t *dst = line_ring_write_ptr(line);

            dma_channel_wait_for_finish_blocking(g_dma_chan);

            // Start next DMA (ping-pong)
            uint32_t *buf = g_line_buffers[buf_idx];
            buf_idx ^= 1U;
            dma_channel_set_trans_count(g_dma_chan, g_line_words, false);
            dma_channel_set_write_addr(g_dma_chan, g_line_buffers[buf_idx], true);

            // Convert pixels directly to ring buffer
            uint32_t *src = buf + g_skip_start_words;
            for (int i = 0; i < g_active_words; i++) {
                dst[i] = convert_pixel(*src++);
            }

            // Signal line ready
            line_ring_commit(line + 1);
        }

        // Last active line (no next DMA to schedule)
        if (g_mvs_height > 0) {
            uint16_t last_line = (uint16_t)(g_mvs_height - 1);
            uint16_t *dst = line_ring_write_ptr(last_line);

            dma_channel_wait_for_finish_blocking(g_dma_chan);

            uint32_t *src = g_line_buffers[buf_idx] + g_skip_start_words;
            for (int i = 0; i < g_active_words; i++) {
                dst[i] = convert_pixel(*src++);
            }

            line_ring_commit(g_mvs_height);
        }
    }
}

uint32_t video_capture_get_frame_count(void)
{
    return g_frame_count;
}
