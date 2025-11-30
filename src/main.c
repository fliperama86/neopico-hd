/**
 * NeoPico-HD - MVS Capture with DVI Output
 *
 * Line-by-line capture for 60fps:
 * - No DMA, no raw buffer
 * - Read pixels directly from PIO FIFO
 * - Convert and write to framebuffer per-line
 *
 * Pin Configuration:
 *   MVS RGB Data:  GPIO 0-14 (15 bits)
 *   MVS Dummy:     GPIO 15
 *   MVS CSYNC:     GPIO 22
 *   MVS PCLK:      GPIO 28
 *   DVI Data:      GPIO 16-21
 *   DVI Clock:     GPIO 26-27
 */

#include <stdio.h>
#include <string.h>
#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "hardware/pio.h"
#include "hardware/clocks.h"
#include "hardware/irq.h"
#include "hardware/vreg.h"
#include "hardware/sync.h"

#include "dvi.h"
#include "dvi_serialiser.h"
#include "mvs_capture.pio.h"

// =============================================================================
// Pin Configuration
// =============================================================================

#define PIN_R0 0       // RGB data: GPIO 0-14 (15 bits)
#define PIN_GND 15     // Dummy bit for 16-bit alignment
#define PIN_CSYNC 22   // Moved for DVI
#define PIN_PCLK 28    // Moved for DVI

// =============================================================================
// DVI Configuration
// =============================================================================

static const struct dvi_serialiser_cfg neopico_dvi_cfg = {
    .pio = pio0,
    .sm_tmds = {0, 1, 2},
    .pins_tmds = {16, 18, 20},
    .pins_clk = 26,
    .invert_diffpairs = true
};

#define FRAME_WIDTH 320
#define FRAME_HEIGHT 240  // Native 240p output
#define MVS_HEIGHT 224
#define VREG_VSEL VREG_VOLTAGE_1_20

// Custom 240p timing (640x240 @ 60Hz)
// Half the pixel clock of 480p for true 60Hz refresh
// Total: 262 lines, 12.6 MHz pixel clock
static const struct dvi_timing dvi_timing_640x240p_60hz = {
    .h_sync_polarity   = false,
    .h_front_porch     = 16,
    .h_sync_width      = 96,
    .h_back_porch      = 48,
    .h_active_pixels   = 640,

    .v_sync_polarity   = false,
    .v_front_porch     = 3,
    .v_sync_width      = 3,
    .v_back_porch      = 16,
    .v_active_lines    = 240,

    .bit_clk_khz       = 126000  // Half of 252000 for 60Hz
};

#define DVI_TIMING dvi_timing_640x240p_60hz

struct dvi_inst dvi0;

// =============================================================================
// MVS Timing Constants (from cps2_digiav neogeo_frontend.v)
// =============================================================================

#define H_THRESHOLD 288      // NEO_H_TOTAL/2 + NEO_H_TOTAL/4
#define NEO_H_TOTAL 384
#define NEO_H_ACTIVE 320

// Even number for clean word-aligned processing (2 pixels per word)
#define H_SKIP_START 28      // Pixels to skip at line start (blanking)
#define H_SKIP_END   36      // Pixels to skip at line end (384 - 28 - 320 = 36)

// Vertical blanking - lines to skip after vsync before active video
#define V_SKIP_LINES 16

#define NEO_V_ACTIVE 224

// =============================================================================
// Buffers - Single framebuffer only (no raw buffer needed!)
// =============================================================================

static uint16_t g_framebuf[FRAME_WIDTH * FRAME_HEIGHT];

static volatile uint32_t frame_count = 0;

// Vertical offset for centering MVS 224 lines in 240 line frame
static const uint8_t v_offset = (FRAME_HEIGHT - MVS_HEIGHT) / 2;

// =============================================================================
// DVI Scanline Callback - runs independently on Core 1's timing
// =============================================================================

static void core1_scanline_callback(void) {
    // Discard any scanline pointers passed back
    uint16_t *bufptr;
    while (queue_try_remove_u32(&dvi0.q_colour_free, &bufptr))
        ;

    // Track current scanline
    static uint scanline = 2;  // First two are pushed before DVI start

    // Return pointer to current row in framebuffer
    bufptr = &g_framebuf[FRAME_WIDTH * scanline];
    queue_add_blocking_u32(&dvi0.q_colour_valid, &bufptr);

    scanline = (scanline + 1) % FRAME_HEIGHT;
}

// =============================================================================
// Core 1: DVI Output
// =============================================================================

void core1_main() {
    dvi_register_irqs_this_core(&dvi0, DMA_IRQ_0);
    dvi_start(&dvi0);
    dvi_scanbuf_main_16bpp(&dvi0);
    __builtin_unreachable();
}

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
            tight_loop_contents();
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
// Inline pixel conversion - MVS RGB555 to RGB565
// =============================================================================

static inline void convert_and_store_pixels(uint32_t word, uint16_t *dst) {
    // Each word contains 2 pixels (16 bits each)
    // MVS format per pixel: R5 (bits 0-4), B5 (bits 5-9), G5 (bits 10-14), dummy (bit 15)

    // Pixel 0 (low 16 bits)
    uint16_t p0 = word & 0xFFFF;
    uint8_t r0 = p0 & 0x1F;
    uint8_t b0 = (p0 >> 5) & 0x1F;
    uint8_t g0 = (p0 >> 10) & 0x1F;
    uint8_t g0_6 = (g0 << 1) | (g0 >> 4);  // 5-bit to 6-bit green
    dst[0] = (r0 << 11) | (g0_6 << 5) | b0;

    // Pixel 1 (high 16 bits)
    uint16_t p1 = word >> 16;
    uint8_t r1 = p1 & 0x1F;
    uint8_t b1 = (p1 >> 5) & 0x1F;
    uint8_t g1 = (p1 >> 10) & 0x1F;
    uint8_t g1_6 = (g1 << 1) | (g1 >> 4);
    dst[1] = (r1 << 11) | (g1_6 << 5) | b1;
}

// =============================================================================
// Main - Core 0: Line-by-line MVS Capture
// =============================================================================

int main() {
    vreg_set_voltage(VREG_VSEL);
    sleep_ms(10);
    set_sys_clock_khz(DVI_TIMING.bit_clk_khz, true);

    stdio_init_all();

    gpio_init(PICO_DEFAULT_LED_PIN);
    gpio_set_dir(PICO_DEFAULT_LED_PIN, GPIO_OUT);
    gpio_put(PICO_DEFAULT_LED_PIN, 1);

    printf("NeoPico-HD: MVS Capture + DVI Output (Line-by-line 60fps)\n");

    // Initialize framebuffer to black
    memset(g_framebuf, 0, sizeof(g_framebuf));

    // Initialize DVI with scanline callback
    dvi0.timing = &DVI_TIMING;
    dvi0.ser_cfg = neopico_dvi_cfg;
    dvi0.scanline_callback = core1_scanline_callback;
    dvi_init(&dvi0, next_striped_spin_lock_num(), next_striped_spin_lock_num());

    // Push first two scanlines to start DVI
    uint16_t *bufptr = g_framebuf;
    queue_add_blocking_u32(&dvi0.q_colour_valid, &bufptr);
    bufptr += FRAME_WIDTH;
    queue_add_blocking_u32(&dvi0.q_colour_valid, &bufptr);

    // Launch DVI on Core 1
    printf("Starting Core 1 (DVI)\n");
    multicore_launch_core1(core1_main);

    // Initialize MVS capture on PIO1
    PIO pio_mvs = pio1;
    uint offset_sync = pio_add_program(pio_mvs, &mvs_sync_4a_program);
    uint sm_sync = pio_claim_unused_sm(pio_mvs, true);
    mvs_sync_4a_program_init(pio_mvs, sm_sync, offset_sync, PIN_CSYNC, PIN_PCLK);

    uint offset_pixel = pio_add_program(pio_mvs, &mvs_pixel_capture_program);
    uint sm_pixel = pio_claim_unused_sm(pio_mvs, true);
    mvs_pixel_capture_program_init(pio_mvs, sm_pixel, offset_pixel, PIN_R0, PIN_GND, PIN_CSYNC, PIN_PCLK);

    // Enable sync detection
    pio_sm_set_enabled(pio_mvs, sm_sync, true);

    printf("Starting line-by-line capture loop\n");

    // Frame rate measurement
    uint32_t last_time = time_us_32();
    uint32_t fps_frame_count = 0;

    // Pre-calculate skip counts (in words, 2 pixels per word)
    const int skip_start_words = H_SKIP_START / 2;   // 14 words
    const int active_words = FRAME_WIDTH / 2;         // 160 words
    const int skip_end_words = H_SKIP_END / 2;        // 18 words
    const int line_words = NEO_H_TOTAL / 2;           // 192 words total

    // Main capture loop
    while (true) {
        frame_count++;
        fps_frame_count++;
        gpio_put(PICO_DEFAULT_LED_PIN, (frame_count / 30) & 1);

        // Print FPS every second
        uint32_t now = time_us_32();
        if (now - last_time >= 1000000) {
            printf("FPS: %lu\n", fps_frame_count);
            fps_frame_count = 0;
            last_time = now;
        }

        // 1. Wait for vsync
        if (!wait_for_vsync(pio_mvs, sm_sync, 100)) {
            continue;  // Timeout, try again
        }

        // 2. Drain sync FIFO
        drain_sync_fifo(pio_mvs, sm_sync);

        // 3. Start pixel capture
        pio_sm_set_enabled(pio_mvs, sm_pixel, false);
        pio_sm_clear_fifos(pio_mvs, sm_pixel);
        pio_sm_restart(pio_mvs, sm_pixel);
        pio_sm_exec(pio_mvs, sm_pixel, pio_encode_jmp(offset_pixel));
        pio_sm_set_enabled(pio_mvs, sm_pixel, true);

        // Trigger IRQ 4 to start pixel capture PIO
        pio_interrupt_clear(pio_mvs, 4);
        pio_sm_exec(pio_mvs, sm_sync, pio_encode_irq_set(false, 4));

        // 4. Skip vertical blanking lines
        for (int skip_line = 0; skip_line < V_SKIP_LINES; skip_line++) {
            for (int i = 0; i < line_words; i++) {
                pio_sm_get_blocking(pio_mvs, sm_pixel);
            }
        }

        // 5. Capture active lines
        for (int line = 0; line < MVS_HEIGHT; line++) {
            uint16_t *dst = &g_framebuf[(v_offset + line) * FRAME_WIDTH];

            // Skip horizontal blanking at start
            for (int i = 0; i < skip_start_words; i++) {
                pio_sm_get_blocking(pio_mvs, sm_pixel);
            }

            // Read and convert active pixels
            for (int x = 0; x < FRAME_WIDTH; x += 2) {
                uint32_t word = pio_sm_get_blocking(pio_mvs, sm_pixel);
                convert_and_store_pixels(word, &dst[x]);
            }

            // Skip horizontal blanking at end
            for (int i = 0; i < skip_end_words; i++) {
                pio_sm_get_blocking(pio_mvs, sm_pixel);
            }
        }

        // 6. Disable pixel capture until next frame
        pio_sm_set_enabled(pio_mvs, sm_pixel, false);
    }

    return 0;
}
