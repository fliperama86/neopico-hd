/**
 * SNES / SuperPico Video Capture Module
 *
 * Target-specific capture backend for SHVC/SNES PPU2 digital RGB.
 * The shared HDMI/audio/output pipeline remains NeoPico-HD's runtime path.
 */

#include "hardware/clocks.h"
#include "hardware/dma.h"
#include "hardware/pio.h"

#include "video_capture.h"
#if NEOPICO_EXP_GENLOCK_DYNAMIC
#include "hardware/timer.h"
#endif
#include "pico/stdlib.h"

#include <stdint.h>
#include <string.h>

#include "capture_profile.h"
#include "line_ring.h"
#include "pico.h"
#include "snes_pins.h"
#include "tusb.h"
#include "video_capture_snes.pio.h"

#ifndef NEOPICO_SNES_CAPTURE_WARMUP_FRAMES
#define NEOPICO_SNES_CAPTURE_WARMUP_FRAMES 60
#endif

// SNES source timing.
#define SNES_H_TOTAL 341
#define SNES_CAPTURE_PIO_TARGET_HZ 126000000U

// =============================================================================
// State
// =============================================================================

static uint g_snes_height = 0;
static PIO g_pio_snes = pio1;

static uint g_sm_pixel = 0;
static uint g_offset_pixel = 0;
static pio_sm_config g_pio_config;
static float g_capture_pio_clkdiv = 1.0F;

static int g_dma_chan = -1;
static uint32_t g_line_buffers[2][CAPTURE_ACTIVE_WIDTH];
static volatile uint32_t g_frame_count = 0;

#if NEOPICO_EXP_GENLOCK_DYNAMIC
volatile uint32_t g_mvs_vsync_timestamp = 0;
#endif

// =============================================================================
// Pixel Conversion - RGB555 to RGB565 LUT
// =============================================================================
// SuperPico wiring maps MSB (R4/G4/B4) to the lower GPIO of each 5-bit group,
// so each color channel is bit-reversed in the captured word.

static inline uint32_t snes_reverse_5bit(uint32_t x)
{
    return ((x & 1U) << 4) | ((x & 2U) << 2) | (x & 4U) | ((x & 8U) >> 2) | ((x & 16U) >> 4);
}

static inline uint16_t snes_pack_rgb565(uint32_t r5, uint32_t g5, uint32_t b5)
{
    return (uint16_t)((r5 << 11) | (g5 << 6) | (g5 >> 4) | b5);
}

static uint16_t g_pixel_lut[32768] __attribute__((aligned(4)));

static void generate_pixel_lut(void)
{
    for (uint32_t idx = 0; idx < 32768U; idx++) {
        uint32_t b5 = snes_reverse_5bit(idx & 0x1F);
        uint32_t g5 = snes_reverse_5bit((idx >> 5) & 0x1F);
        uint32_t r5 = snes_reverse_5bit((idx >> 10) & 0x1F);
        g_pixel_lut[idx] = snes_pack_rgb565(r5, g5, b5);
    }
}

static inline void fill_rgb565(uint16_t *dst, uint32_t count, uint16_t color)
{
    for (uint32_t i = 0; i < count; i++) {
        dst[i] = color;
    }
}

static inline void convert_active_pixels(uint16_t *dst, const uint32_t *src, int count)
{
    const uint16_t *lut = g_pixel_lut;
    int remaining = count;
    while (remaining >= 4) {
        dst[0] = lut[(src[0] >> 2) & 0x7FFF];
        dst[1] = lut[(src[1] >> 2) & 0x7FFF];
        dst[2] = lut[(src[2] >> 2) & 0x7FFF];
        dst[3] = lut[(src[3] >> 2) & 0x7FFF];
        dst += 4;
        src += 4;
        remaining -= 4;
    }
    while (remaining-- > 0) {
        *dst++ = lut[(*src++ >> 2) & 0x7FFF];
    }
}

// =============================================================================
// Hardware Reset
// =============================================================================

static void video_capture_reset_hardware(void)
{
    pio_sm_set_enabled(g_pio_snes, g_sm_pixel, false);
    pio_sm_clear_fifos(g_pio_snes, g_sm_pixel);
    pio_sm_init(g_pio_snes, g_sm_pixel, g_offset_pixel, &g_pio_config);

    // Reapply IN_BASE after pio_sm_init, which resets pinctrl.
    uint pin_idx = PIN_SNES_BASE - 16;
    g_pio_snes->sm[g_sm_pixel].pinctrl = (g_pio_snes->sm[g_sm_pixel].pinctrl & ~0x000f8000U) | (pin_idx << 15);

    pio_sm_set_enabled(g_pio_snes, g_sm_pixel, true);
    pio_sm_put_blocking(g_pio_snes, g_sm_pixel, CAPTURE_ACTIVE_WIDTH - 1);
}

// =============================================================================
// Public API
// =============================================================================

void video_capture_init(uint height)
{
    g_snes_height = height;
    generate_pixel_lut();
    g_capture_pio_clkdiv = (float)clock_get_hz(clk_sys) / (float)SNES_CAPTURE_PIO_TARGET_HZ;
    if (g_capture_pio_clkdiv < 1.0F) {
        g_capture_pio_clkdiv = 1.0F;
    }

    pio_clear_instruction_memory(g_pio_snes);

    pio_set_gpio_base(pio0, 0);
    pio_set_gpio_base(pio1, 0);
    *(volatile uint32_t *)((uintptr_t)g_pio_snes + 0x168) = 16;

    g_offset_pixel = pio_add_program(g_pio_snes, &snes_hard_sync_program);
    g_sm_pixel = (uint)pio_claim_unused_sm(g_pio_snes, true);

    for (uint pin = PIN_SNES_BASE; pin <= SNES_CAPTURE_PIN_LAST; pin++) {
        pio_gpio_init(g_pio_snes, pin);
        gpio_disable_pulls(pin);
        gpio_set_input_enabled(pin, true);
        gpio_set_input_hysteresis_enabled(pin, true);
    }

    g_pio_config = snes_hard_sync_program_get_default_config(g_offset_pixel);
    sm_config_set_clkdiv(&g_pio_config, g_capture_pio_clkdiv);
    sm_config_set_in_shift(&g_pio_config, false, true, SNES_CAPTURE_BITS);

    video_capture_reset_hardware();

    g_dma_chan = dma_claim_unused_channel(true);
    dma_channel_config dc = dma_channel_get_default_config(g_dma_chan);
    channel_config_set_read_increment(&dc, false);
    channel_config_set_write_increment(&dc, true);
    channel_config_set_dreq(&dc, pio_get_dreq(g_pio_snes, g_sm_pixel, false));
    dma_channel_configure(g_dma_chan, &dc, g_line_buffers[0], &g_pio_snes->rxf[g_sm_pixel], CAPTURE_ACTIVE_WIDTH,
                          false);
}

void video_capture_run(void)
{
    while (1) {
        // Detect VBLANK falling edge, which marks active frame start.
        while (!gpio_get(PIN_SNES_VBLANK)) {
            tight_loop_contents();
        }
        while (gpio_get(PIN_SNES_VBLANK)) {
            tight_loop_contents();
        }

        g_frame_count++;

#if NEOPICO_SNES_CAPTURE_WARMUP_FRAMES > 0
        if (g_frame_count <= NEOPICO_SNES_CAPTURE_WARMUP_FRAMES) {
            tud_task();
            continue;
        }
#endif

#if NEOPICO_EXP_GENLOCK_DYNAMIC
        g_mvs_vsync_timestamp = timer_hw->timerawl;
#endif

        // Reset pixel capture SM for frame alignment.
        pio_sm_set_enabled(g_pio_snes, g_sm_pixel, false);
        pio_sm_clear_fifos(g_pio_snes, g_sm_pixel);
        pio_sm_exec(g_pio_snes, g_sm_pixel, pio_encode_jmp(g_offset_pixel + 2));
        pio_sm_set_enabled(g_pio_snes, g_sm_pixel, true);

        dma_channel_set_trans_count(g_dma_chan, CAPTURE_ACTIVE_WIDTH, false);
        dma_channel_set_write_addr(g_dma_chan, g_line_buffers[0], true);

        line_ring_vsync();

        pio_interrupt_clear(g_pio_snes, 4);
        pio_sm_exec(g_pio_snes, g_sm_pixel, pio_encode_irq_set(false, 4));

        uint8_t buf_idx = 0;
        for (uint16_t line = 0; line < g_snes_height; line++) {
            uint16_t *dst = line_ring_write_ptr(line);

            dma_channel_wait_for_finish_blocking(g_dma_chan);

            uint32_t *buf = g_line_buffers[buf_idx];
            buf_idx ^= 1U;

            if (line + 1 < g_snes_height) {
                dma_channel_set_trans_count(g_dma_chan, CAPTURE_ACTIVE_WIDTH, false);
                dma_channel_set_write_addr(g_dma_chan, g_line_buffers[buf_idx], true);
            }

            fill_rgb565(dst, CAPTURE_ACTIVE_X_OFFSET, 0x0000);
            convert_active_pixels(dst + CAPTURE_ACTIVE_X_OFFSET, buf, CAPTURE_ACTIVE_WIDTH);
            fill_rgb565(dst + CAPTURE_ACTIVE_X_OFFSET + CAPTURE_ACTIVE_WIDTH,
                        LINE_WIDTH - CAPTURE_ACTIVE_X_OFFSET - CAPTURE_ACTIVE_WIDTH, 0x0000);

            line_ring_commit(line + 1);
        }
    }
}

uint32_t video_capture_get_frame_count(void)
{
    return g_frame_count;
}
