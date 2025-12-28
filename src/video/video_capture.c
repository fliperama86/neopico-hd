/**
 * MVS Video Capture Module
 * PIO-based line-by-line capture from Neo Geo MVS
 */

#include "video_capture.h"
#include "hardware_config.h"
#include "video_capture.pio.h"
#include "hardware/pio.h"
#include "pico/stdlib.h"

// =============================================================================
// MVS Timing Constants (from cps2_digiav neogeo_frontend.v)
// =============================================================================

// H_THRESHOLD: distinguishes short (vsync) from long (hsync) pulses
// PIO counts PCLK edges (6 MHz external), so threshold is clock-independent
#define H_THRESHOLD 288
#define NEO_H_TOTAL 384
#define NEO_H_ACTIVE 320

// Even number for clean word-aligned processing (2 pixels per word)
#define H_SKIP_START 28      // Pixels to skip at line start (blanking)
#define H_SKIP_END   36      // Pixels to skip at line end (384 - 28 - 320 = 36)

// Vertical blanking - lines to skip after vsync before active video
#define V_SKIP_LINES 16

#define NEO_V_ACTIVE 224

// =============================================================================
// State
// =============================================================================

static uint16_t *g_framebuffer = NULL;
static uint g_frame_width = 0;
static uint g_frame_height = 0;
static uint g_mvs_height = 0;
static uint8_t g_v_offset = 0;

static PIO g_pio_mvs = NULL;
static uint g_sm_sync = 0;
static uint g_sm_pixel = 0;
static uint g_offset_pixel = 0;

static volatile uint32_t g_frame_count = 0;

// Pre-calculated skip counts (in words, 2 pixels per word)
static int g_skip_start_words = 0;
static int g_active_words = 0;
static int g_skip_end_words = 0;
static int g_line_words = 0;

// =============================================================================
// MVS Sync Detection
// =============================================================================

static inline void drain_sync_fifo(PIO pio, uint sm) {
    while (!pio_sm_is_rx_fifo_empty(pio, sm)) {
        pio_sm_get(pio, sm);
    }
}

// Blocking wait for vsync (equalization pulses pattern)
static bool wait_for_vsync(PIO pio, uint sm_sync, uint32_t timeout_ms) {
    uint32_t equ_count = 0;
    absolute_time_t timeout = make_timeout_time_ms(timeout_ms);
    bool in_vsync = false;

    while (true) {
        if (absolute_time_diff_us(get_absolute_time(), timeout) <= 0) {
            return false;
        }

        if (pio_sm_is_rx_fifo_empty(pio, sm_sync)) {
            continue;
        }

        uint32_t h_ctr = pio_sm_get(pio, sm_sync);
        bool is_short_pulse = (h_ctr <= H_THRESHOLD);

        if (!in_vsync) {
            if (is_short_pulse) {
                equ_count++;
            } else {
                if (equ_count >= 8) {  // MVS has 9 equ pulses
                    in_vsync = true;
                    equ_count = 0;
                }
                equ_count = 0;
            }
        } else {
            if (is_short_pulse) {
                equ_count++;
            } else {
                // First normal hsync after vsync - we're ready
                return true;
            }
        }
    }
}

// =============================================================================
// Pixel Conversion - MVS RGB555 to RGB565
// =============================================================================

static inline void convert_and_store_pixels(uint32_t word, uint16_t *dst) {
    // Each word contains 2 pixels (16 bits each)
    // Per pixel: bit 0 = PCLK (ignore), bits 1-5 = G4-G0 (reversed), bits 6-10 = B0-B4, bits 11-15 = R0-R4

    // Pixel 0 (low 16 bits)
    uint16_t p0 = word & 0xFFFF;
    uint8_t g0_raw = (p0 >> 1) & 0x1F;
    uint8_t b0 = (p0 >> 6) & 0x1F;
    uint8_t r0 = (p0 >> 11) & 0x1F;
    uint8_t g0 = green_reverse_lut[g0_raw];
    uint8_t g0_6 = (g0 << 1) | (g0 >> 4);  // 5-bit to 6-bit green
    dst[0] = (r0 << 11) | (g0_6 << 5) | b0;

    // Pixel 1 (high 16 bits)
    uint16_t p1 = word >> 16;
    uint8_t g1_raw = (p1 >> 1) & 0x1F;
    uint8_t b1 = (p1 >> 6) & 0x1F;
    uint8_t r1 = (p1 >> 11) & 0x1F;
    uint8_t g1 = green_reverse_lut[g1_raw];
    uint8_t g1_6 = (g1 << 1) | (g1 >> 4);
    dst[1] = (r1 << 11) | (g1_6 << 5) | b1;
}

// =============================================================================
// Public API
// =============================================================================

void video_capture_init(uint16_t *framebuffer, uint frame_width, uint frame_height, uint mvs_height) {
    g_framebuffer = framebuffer;
    g_frame_width = frame_width;
    g_frame_height = frame_height;
    g_mvs_height = mvs_height;
    g_v_offset = (frame_height - mvs_height) / 2;

    // Pre-calculate skip counts (in words, 2 pixels per word)
    g_skip_start_words = H_SKIP_START / 2;   // 14 words
    g_active_words = frame_width / 2;        // 160 words
    g_skip_end_words = H_SKIP_END / 2;       // 18 words
    g_line_words = NEO_H_TOTAL / 2;          // 192 words total

    // Initialize MVS capture on PIO1
    g_pio_mvs = pio1;
    uint offset_sync = pio_add_program(g_pio_mvs, &mvs_sync_4a_program);
    g_sm_sync = pio_claim_unused_sm(g_pio_mvs, true);
    mvs_sync_4a_program_init(g_pio_mvs, g_sm_sync, offset_sync, PIN_CSYNC, PIN_PCLK);

    g_offset_pixel = pio_add_program(g_pio_mvs, &mvs_pixel_capture_program);
    g_sm_pixel = pio_claim_unused_sm(g_pio_mvs, true);
    mvs_pixel_capture_program_init(g_pio_mvs, g_sm_pixel, g_offset_pixel, PIN_BASE, PIN_CSYNC, PIN_PCLK);

    // Enable sync detection
    pio_sm_set_enabled(g_pio_mvs, g_sm_sync, true);
}

bool video_capture_frame(void) {
    g_frame_count++;

    // 1. Wait for vsync
    if (!wait_for_vsync(g_pio_mvs, g_sm_sync, 100)) {
        return false;  // Timeout
    }

    // 2. Drain sync FIFO
    drain_sync_fifo(g_pio_mvs, g_sm_sync);

    // 3. Start pixel capture
    pio_sm_set_enabled(g_pio_mvs, g_sm_pixel, false);
    pio_sm_clear_fifos(g_pio_mvs, g_sm_pixel);
    pio_sm_restart(g_pio_mvs, g_sm_pixel);
    pio_sm_exec(g_pio_mvs, g_sm_pixel, pio_encode_jmp(g_offset_pixel));
    pio_sm_set_enabled(g_pio_mvs, g_sm_pixel, true);

    // Trigger IRQ 4 to start pixel capture PIO
    pio_interrupt_clear(g_pio_mvs, 4);
    pio_sm_exec(g_pio_mvs, g_sm_sync, pio_encode_irq_set(false, 4));

    // 4. Skip vertical blanking lines
    for (int skip_line = 0; skip_line < V_SKIP_LINES; skip_line++) {
        for (int i = 0; i < g_line_words; i++) {
            pio_sm_get_blocking(g_pio_mvs, g_sm_pixel);
        }
    }

    // 5. Capture active lines
    for (uint line = 0; line < g_mvs_height; line++) {
        uint16_t *dst = &g_framebuffer[(g_v_offset + line) * g_frame_width];

        // Skip horizontal blanking at start
        for (int i = 0; i < g_skip_start_words; i++) {
            pio_sm_get_blocking(g_pio_mvs, g_sm_pixel);
        }

        // Read and convert active pixels
        for (uint x = 0; x < g_frame_width; x += 2) {
            uint32_t word = pio_sm_get_blocking(g_pio_mvs, g_sm_pixel);
            convert_and_store_pixels(word, &dst[x]);
        }

        // Skip horizontal blanking at end
        for (int i = 0; i < g_skip_end_words; i++) {
            pio_sm_get_blocking(g_pio_mvs, g_sm_pixel);
        }
    }

    // 6. Disable pixel capture until next frame
    pio_sm_set_enabled(g_pio_mvs, g_sm_pixel, false);

    return true;
}

uint32_t video_capture_get_frame_count(void) {
    return g_frame_count;
}
