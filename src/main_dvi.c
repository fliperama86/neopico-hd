/**
 * NeoPico-HD - MVS Capture with DVI Output
 *
 * Simplified approach inspired by PicoDVI-N64:
 * - Single framebuffer (accepts tearing)
 * - Scanline callback for DVI output (runs independently)
 * - Capture loop writes directly to framebuffer
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
#include "hardware/dma.h"
#include "hardware/irq.h"
#include "hardware/vreg.h"
#include "hardware/sync.h"

#include "dvi.h"
#include "dvi_serialiser.h"
#include "mvs_sync.pio.h"

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
#define FRAME_HEIGHT 240  // DVI expects 240 lines
#define MVS_HEIGHT 224
#define VREG_VSEL VREG_VOLTAGE_1_20
#define DVI_TIMING dvi_timing_640x480p_60hz

struct dvi_inst dvi0;

// =============================================================================
// MVS Timing Constants (from cps2_digiav neogeo_frontend.v)
// =============================================================================

#define H_THRESHOLD 288      // NEO_H_TOTAL/2 + NEO_H_TOTAL/4
#define NEO_H_TOTAL 384
#define NEO_H_SYNCLEN 29
#define NEO_H_BACKPORCH 28
#define NEO_H_ACTIVE 320
#define NEO_H_ACTIVE_START 27  // Tuned for correct horizontal position

#define NEO_V_TOTAL 264
#define NEO_V_SYNCLEN 3
#define NEO_V_BACKPORCH 21
#define NEO_V_ACTIVE 224

// =============================================================================
// Buffers - Single framebuffer (accepts tearing)
// =============================================================================

// Single raw capture buffer - simpler, 30fps but stable
#define RAW_BUFFER_WORDS 48000
static uint32_t raw_buffer[RAW_BUFFER_WORDS];

// Single frame buffer - DVI reads while capture writes (tearing OK)
static uint16_t g_framebuf[FRAME_WIDTH * FRAME_HEIGHT];

static int dma_chan;
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
// Frame Processing - extract pixels from raw capture buffer
// =============================================================================

static inline uint16_t extract_pixel(uint32_t *raw_buf, uint32_t raw_bit_idx, uint32_t words_captured) {
    uint32_t word_idx = raw_bit_idx / 32;
    uint32_t bit_idx = raw_bit_idx % 32;

    if (word_idx >= words_captured) return 0x0000;  // Black for missing data

    uint32_t raw_val = raw_buf[word_idx] >> bit_idx;
    if (bit_idx > 16 && (word_idx + 1) < words_captured) {
        raw_val |= raw_buf[word_idx + 1] << (32 - bit_idx);
    }

    // MVS format: bits 0-4=R, 5-9=B, 10-14=G
    uint8_t r5 = raw_val & 0x1F;
    uint8_t b5 = (raw_val >> 5) & 0x1F;
    uint8_t g5 = (raw_val >> 10) & 0x1F;
    uint8_t g6 = (g5 << 1) | (g5 >> 4);  // Expand 5-bit to 6-bit
    return (r5 << 11) | (g6 << 5) | b5;  // RGB565
}

// Process entire frame from raw buffer to framebuffer
static void process_frame(uint32_t *raw_buf, uint32_t words_captured) {
    // Skip some lines at top (vertical blanking offset)
    // With 46000 words = ~239 lines, offset 16 gives 223 active lines
    const uint32_t v_blanking_offset = 16;

    // Calculate max safe lines based on buffer size
    uint32_t max_lines = (words_captured * 32) / (NEO_H_TOTAL * 16);
    uint32_t safe_lines = (max_lines > v_blanking_offset) ? (max_lines - v_blanking_offset) : 0;
    if (safe_lines > MVS_HEIGHT) safe_lines = MVS_HEIGHT;

    for (uint32_t line = 0; line < safe_lines; line++) {
        uint32_t raw_bit_idx = (v_blanking_offset + line) * NEO_H_TOTAL * 16;
        raw_bit_idx += NEO_H_ACTIVE_START * 16;  // Skip h blanking

        // Write to framebuffer with vertical centering
        uint16_t *dst = &g_framebuf[(v_offset + line) * FRAME_WIDTH];

        for (uint32_t x = 0; x < FRAME_WIDTH; x++) {
            dst[x] = extract_pixel(raw_buf, raw_bit_idx, words_captured);
            raw_bit_idx += 16;
        }
    }
}

// =============================================================================
// DMA Setup
// =============================================================================

static void setup_dma(PIO pio, uint sm_pixel) {
    dma_channel_config cfg = dma_channel_get_default_config(dma_chan);
    channel_config_set_read_increment(&cfg, false);
    channel_config_set_write_increment(&cfg, true);
    channel_config_set_dreq(&cfg, pio_get_dreq(pio, sm_pixel, false));
    channel_config_set_transfer_data_size(&cfg, DMA_SIZE_32);

    dma_channel_configure(
        dma_chan,
        &cfg,
        raw_buffer,
        &pio->rxf[sm_pixel],
        RAW_BUFFER_WORDS,
        false);
}

// =============================================================================
// Main - Core 0: MVS Capture (runs independently of DVI)
// =============================================================================

int main() {
    vreg_set_voltage(VREG_VSEL);
    sleep_ms(10);
    set_sys_clock_khz(DVI_TIMING.bit_clk_khz, true);

    stdio_init_all();

    gpio_init(PICO_DEFAULT_LED_PIN);
    gpio_set_dir(PICO_DEFAULT_LED_PIN, GPIO_OUT);
    gpio_put(PICO_DEFAULT_LED_PIN, 1);

    printf("NeoPico-HD: MVS Capture + DVI Output (N64-style)\n");

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

    dma_chan = dma_claim_unused_channel(true);
    setup_dma(pio_mvs, sm_pixel);

    // Enable sync detection
    pio_sm_set_enabled(pio_mvs, sm_sync, true);

    printf("Starting capture loop\n");

    // Frame rate measurement
    uint32_t last_time = time_us_32();
    uint32_t fps_frame_count = 0;

    // Main capture loop - completely independent of DVI timing
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

        // 2. Drain sync FIFO to clear any buffered pulses
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

        // 4. Start DMA transfer
        dma_channel_set_write_addr(dma_chan, raw_buffer, false);
        dma_channel_set_trans_count(dma_chan, RAW_BUFFER_WORDS, false);
        dma_channel_start(dma_chan);

        // 5. Wait for DMA to complete
        dma_channel_wait_for_finish_blocking(dma_chan);
        pio_sm_set_enabled(pio_mvs, sm_pixel, false);

        // 6. Process frame (this takes ~16ms, so we run at ~30fps)
        process_frame(raw_buffer, RAW_BUFFER_WORDS);
    }

    return 0;
}
