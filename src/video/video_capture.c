/**
 * MVS Video Capture Module
 * PIO-based line-by-line capture from Neo Geo MVS
 *
 * Streaming architecture: captures lines directly to ring buffer
 */

#include "video_capture.h"
#include "line_ring.h"
#include "hardware/dma.h"
#include "hardware/pio.h"
#include "hardware_config.h"
#include "mvs_pins.h"
#include "pico/stdlib.h"
#include "tusb.h"
#include "video_capture.pio.h"
#include <stdio.h>
#include <stdlib.h>

// =============================================================================
// Feature Flags
// =============================================================================

// Set to 1 to re-enable DARK/SHADOW processing via LUT
#define ENABLE_DARK_SHADOW 0

#if ENABLE_DARK_SHADOW
#include "hardware/interp.h"
// 256KB LUT for fast pixel conversion (131072 entries * 2 bytes)
static uint16_t g_pixel_lut[131072] __attribute__((aligned(4)));
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
static int g_consecutive_frames = 0;

// =============================================================================
// Pixel Conversion
// =============================================================================

// Direct conversion: RGB555 (from PIO) -> RGB565 (for HDMI)
// PIO captures 18 bits: [17:SHADOW][16:DARK][15-11:B][10-6:G][5-1:R][0:PCLK]
static inline uint16_t convert_pixel(uint32_t raw) {
#if ENABLE_DARK_SHADOW
    // Use LUT for DARK/SHADOW processing
    interp0->accum[0] = raw;
    return *(uint16_t *)interp0->peek[0];
#else
    // Direct bit extraction (ignores DARK/SHADOW)
    uint16_t r = (raw >> 1) & 0x1F;   // bits 1-5
    uint16_t g = (raw >> 6) & 0x1F;   // bits 6-10
    uint16_t b = (raw >> 11) & 0x1F;  // bits 11-15
    // RGB565: RRRRR GGGGGG BBBBB (green gets MSB duplicated)
    return (r << 11) | (g << 6) | (g >> 4) | b;
#endif
}

#if ENABLE_DARK_SHADOW
static void generate_intensity_lut(void) {
    for (uint32_t i = 0; i < 131072; i++) {
        uint8_t r5 = i & 0x1F;
        uint8_t g5 = (i >> 5) & 0x1F;
        uint8_t b5 = (i >> 10) & 0x1F;
        bool dark = (i >> 15) & 1;
        bool shadow = (i >> 16) & 1;

        // 1. Apply SHADOW FIRST (on 5-bit values!)
        if (shadow) {
            r5 >>= 1;
            g5 >>= 1;
            b5 >>= 1;
            dark = true; // FORCE DARK when SHADOW active!
        }

        // 2. Expand 5->8 bit
        uint8_t r8 = (r5 << 3) | (r5 >> 2);
        uint8_t g8 = (g5 << 3) | (g5 >> 2);
        uint8_t b8 = (b5 << 3) | (b5 >> 2);

        // 3. Apply DARK (-4 with saturation)
        if (dark) {
            r8 = (r8 > 4) ? r8 - 4 : 0;
            g8 = (g8 > 4) ? g8 - 4 : 0;
            b8 = (b8 > 4) ? b8 - 4 : 0;
        }

        // 4. Pack as RGB565
        g_pixel_lut[i] = ((r8 >> 3) << 11) | ((g8 >> 2) << 5) | (b8 >> 3);
    }
}
#endif

// =============================================================================
// MVS Sync Detection
// =============================================================================

static inline void drain_sync_fifo(PIO pio, uint sm) {
    while (!pio_sm_is_rx_fifo_empty(pio, sm)) {
        pio_sm_get(pio, sm);
    }
}

static bool wait_for_vsync(PIO pio, uint sm_sync, uint32_t timeout_ms) {
    uint32_t equ_count = 0;
    absolute_time_t timeout = make_timeout_time_ms(timeout_ms);
    bool in_vsync = false;
    static uint32_t timeout_count = 0;

    while (true) {
        if (absolute_time_diff_us(get_absolute_time(), timeout) <= 0) {
            timeout_count++;
            tud_task(); // Keep USB alive
            return false;
        }

        if (pio_sm_is_rx_fifo_empty(pio, sm_sync)) {
            tight_loop_contents();
            continue;
        }

        uint32_t h_ctr = pio_sm_get(pio, sm_sync);
        bool is_short_pulse = (h_ctr <= H_THRESHOLD);

        if (!in_vsync) {
            if (is_short_pulse) {
                equ_count++;
            } else {
                if (equ_count >= 8) {
                    in_vsync = true;
                    equ_count = 0;
                }
                equ_count = 0;
            }
        } else {
            if (is_short_pulse) {
                equ_count++;
            } else {
                timeout_count = 0;
                return true;
            }
        }
    }
}

// =============================================================================
// Hardware Reset
// =============================================================================

static void video_capture_reset_hardware(void) {
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

    // 5. Clear any pending trigger IRQ
    pio_interrupt_clear(g_pio_mvs, 4);
}

// =============================================================================
// Public API
// =============================================================================

void video_capture_init(uint mvs_height) {
    g_mvs_height = mvs_height;

#if ENABLE_DARK_SHADOW
    // Initialize LUT for DARK/SHADOW processing
    generate_intensity_lut();

    interp_config cfg = interp_default_config();
    interp_config_set_shift(&cfg, 0);
    interp_config_set_mask(&cfg, 1, 17);
    interp_set_config(interp0, 0, &cfg);
    interp0->base[0] = (uintptr_t)g_pixel_lut;
#endif

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

    // 1. Force GPIOBASE to 16
    *(volatile uint32_t *)((uintptr_t)g_pio_mvs + 0x168) = 16;

    // 2. Add programs
    pio_clear_instruction_memory(g_pio_mvs);
    g_offset_sync = pio_add_program(g_pio_mvs, &mvs_sync_4a_program);
    g_offset_pixel = pio_add_program(g_pio_mvs, &mvs_pixel_capture_program);

    // 3. Claim SMs
    g_sm_sync = (uint)pio_claim_unused_sm(g_pio_mvs, true);
    g_sm_pixel = (uint)pio_claim_unused_sm(g_pio_mvs, true);

    // 4. Setup GPIOs (GP25-43: PCLK, RGB, DARK, SHADOW, CSYNC)
    for (uint i = PIN_MVS_BASE; i <= PIN_MVS_CSYNC; i++) {
        pio_gpio_init(g_pio_mvs, i);
        gpio_disable_pulls(i);
        gpio_set_input_enabled(i, true);
        gpio_set_input_hysteresis_enabled(i, true);
    }

    // 5. Configure Sync SM (GP45 as CSYNC)
    pio_sm_config c = mvs_sync_4a_program_get_default_config(g_offset_sync);
    sm_config_set_clkdiv(&c, 1.0f);
    sm_config_set_in_shift(&c, false, false, 32);
    sm_config_set_fifo_join(&c, PIO_FIFO_JOIN_RX);
    pio_sm_init(g_pio_mvs, g_sm_sync, g_offset_sync, &c);

    // MANUAL REGISTER OVERRIDE for Sync SM
    uint pin_idx_sync = 29;
    g_pio_mvs->sm[g_sm_sync].pinctrl =
        (g_pio_mvs->sm[g_sm_sync].pinctrl & ~0x000f8000) | (pin_idx_sync << 15);
    g_pio_mvs->sm[g_sm_sync].execctrl =
        (g_pio_mvs->sm[g_sm_sync].execctrl & ~0x1f000000) | (pin_idx_sync << 24);

    // 6. Configure Pixel SM (GP27 as IN_BASE)
    pio_sm_config pc = mvs_pixel_capture_program_get_default_config(g_offset_pixel);
    sm_config_set_clkdiv(&pc, 1.0f);
    sm_config_set_in_shift(&pc, false, true, 18);
    pio_sm_init(g_pio_mvs, g_sm_pixel, g_offset_pixel, &pc);

    // MANUAL REGISTER OVERRIDE for Pixel SM
    uint pin_idx_pixel = 11;
    g_pio_mvs->sm[g_sm_pixel].pinctrl =
        (g_pio_mvs->sm[g_sm_pixel].pinctrl & ~0x000f8000) | (pin_idx_pixel << 15);

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
    dma_channel_configure(g_dma_chan, &dc, g_line_buffers[0],
                          &g_pio_mvs->rxf[g_sm_pixel], 0, false);
}

void video_capture_run(void) {
    while (1) {
        g_frame_count++;

        // Wait for VSYNC
        if (!wait_for_vsync(g_pio_mvs, g_sm_sync, 100)) {
            video_capture_reset_hardware();
            g_consecutive_frames = 0;
            continue;
        }

        g_consecutive_frames++;
        if (g_consecutive_frames < 10) {
            // Wait for signal to stabilize
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
        for (uint16_t line = 0; line < g_mvs_height; line++) {
            uint16_t *dst = line_ring_write_ptr(line);

            dma_channel_wait_for_finish_blocking(g_dma_chan);

            // Start next DMA (ping-pong)
            uint32_t *buf = g_line_buffers[line % 2];
            if (line + 1 < g_mvs_height) {
                dma_channel_set_trans_count(g_dma_chan, g_line_words, false);
                dma_channel_set_write_addr(g_dma_chan, g_line_buffers[(line + 1) % 2], true);
            }

            // Convert pixels directly to ring buffer
            for (int i = 0; i < g_active_words; i++) {
                dst[i] = convert_pixel(buf[g_skip_start_words + i]);
            }

            // Signal line ready
            line_ring_commit(line + 1);
        }

        // Disable SM until next frame
        pio_sm_set_enabled(g_pio_mvs, g_sm_pixel, false);
    }
}

uint32_t video_capture_get_frame_count(void) {
    return g_frame_count;
}
