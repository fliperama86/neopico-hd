/**
 * NeoPico-HD - MVS Capture with DVI Output
 *
 * Captures video from Neo Geo MVS and outputs via DVI/HDMI.
 *
 * Pin Configuration:
 *   MVS RGB Data:  GPIO 0-14 (15 bits)
 *   MVS Dummy:     GPIO 15
 *   MVS CSYNC:     GPIO 22
 *   MVS PCLK:      GPIO 28
 *   DVI Data:      GPIO 16-21
 *   DVI Clock:     GPIO 26-27
 *   VSYS:          Connected to Spotpear VSYS
 *
 * PIO Assignment (RP2350 has 3 PIOs):
 *   PIO0: DVI output (3 state machines for TMDS)
 *   PIO1: MVS sync detection + pixel capture
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
#define FRAME_HEIGHT 240  // DVI frame height (MVS is 224, centered)
#define MVS_HEIGHT 224
#define VREG_VSEL VREG_VOLTAGE_1_20
#define DVI_TIMING dvi_timing_640x480p_60hz

struct dvi_inst dvi0;

// =============================================================================
// MVS Timing Constants
// =============================================================================

#define H_THRESHOLD 288
#define NEO_H_TOTAL 384
#define NEO_H_ACTIVE_START 27

// =============================================================================
// Buffers
// =============================================================================

// Single buffer for now (double buffering needs too much RAM)
#define RAW_BUFFER_WORDS 50000
static uint32_t raw_buffer[RAW_BUFFER_WORDS];
static uint8_t capture_buf_idx = 0;

// Frame buffer for captured MVS frame (RGB565)
static uint16_t mvs_frame[FRAME_WIDTH * MVS_HEIGHT];

static int dma_chan;
static volatile bool frame_ready = false;
static volatile uint32_t frame_count = 0;
static uint32_t capture_offset_lines = 16;  // Skip vertical blanking

// =============================================================================
// MVS Sync Detection
// =============================================================================

static inline void drain_sync_fifo(PIO pio, uint sm) {
    while (!pio_sm_is_rx_fifo_empty(pio, sm)) {
        pio_sm_get(pio, sm);
    }
}

static bool wait_for_vsync_and_hsync(PIO pio, uint sm_sync, uint32_t timeout_ms) {
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
                if (equ_count >= 8) {
                    in_vsync = true;
                    equ_count = 0;
                    drain_sync_fifo(pio, sm_sync);
                } else {
                    equ_count = 0;
                }
            }
        } else {
            if (is_short_pulse) {
                equ_count++;
            } else {
                return true;
            }
        }
    }
}

static bool wait_for_vsync_timeout(PIO pio, uint sm_sync, uint32_t timeout_ms) {
    uint32_t equ_count = 0;
    absolute_time_t timeout = make_timeout_time_ms(timeout_ms);

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

        if (is_short_pulse) {
            equ_count++;
        } else {
            if (equ_count >= 8) {
                return true;
            }
            equ_count = 0;
        }
    }
}

// =============================================================================
// Frame Processing
// =============================================================================

static void process_frame(uint32_t *raw_buf, uint16_t *frame_buf, uint32_t words_captured) {
    uint32_t raw_bit_idx = capture_offset_lines * NEO_H_TOTAL * 16;
    uint32_t out_idx = 0;

    for (uint32_t line = 0; line < MVS_HEIGHT; line++) {
        raw_bit_idx += NEO_H_ACTIVE_START * 16;

        for (uint32_t x = 0; x < FRAME_WIDTH; x++) {
            uint32_t word_idx = raw_bit_idx / 32;
            uint32_t bit_idx = raw_bit_idx % 32;

            uint16_t pixel = 0;
            if (word_idx < words_captured) {
                uint32_t raw_val = raw_buf[word_idx] >> bit_idx;
                if (bit_idx > 16 && (word_idx + 1) < words_captured) {
                    raw_val |= raw_buf[word_idx + 1] << (32 - bit_idx);
                }

                uint8_t r5 = raw_val & 0x1F;
                uint8_t b5 = (raw_val >> 5) & 0x1F;
                uint8_t g5 = (raw_val >> 10) & 0x1F;
                uint8_t g6 = (g5 << 1) | (g5 >> 4);
                pixel = (r5 << 11) | (g6 << 5) | b5;
            }

            frame_buf[out_idx++] = pixel;
            raw_bit_idx += 16;
        }

        raw_bit_idx += (NEO_H_TOTAL - NEO_H_ACTIVE_START - FRAME_WIDTH) * 16;
    }
}

// =============================================================================
// DMA Configuration
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
        raw_buffer,  // Single buffer
        &pio->rxf[sm_pixel],
        RAW_BUFFER_WORDS,
        false);
}

// =============================================================================
// DVI scanline buffers
// =============================================================================

static uint16_t scanline_buf[2][FRAME_WIDTH];
static uint8_t v_offset = (FRAME_HEIGHT - MVS_HEIGHT) / 2;

// =============================================================================
// Core 1: DVI Output (TMDS encoding and serialization)
// =============================================================================

void core1_main() {
    dvi_register_irqs_this_core(&dvi0, DMA_IRQ_0);
    while (queue_is_empty(&dvi0.q_colour_valid))
        __wfe();
    dvi_start(&dvi0);
    dvi_scanbuf_main_16bpp(&dvi0);
    __builtin_unreachable();
}

// Generate scanline from MVS frame buffer
static void generate_scanline(uint16_t *buf, uint y) {
    if (y < v_offset || y >= v_offset + MVS_HEIGHT) {
        memset(buf, 0, FRAME_WIDTH * 2);
    } else {
        memcpy(buf, &mvs_frame[(y - v_offset) * FRAME_WIDTH], FRAME_WIDTH * 2);
    }
}

// =============================================================================
// Core 0: Scanline generation + MVS Capture
// =============================================================================

int main() {
    vreg_set_voltage(VREG_VSEL);
    sleep_ms(10);
    set_sys_clock_khz(DVI_TIMING.bit_clk_khz, true);

    stdio_init_all();

    gpio_init(PICO_DEFAULT_LED_PIN);
    gpio_set_dir(PICO_DEFAULT_LED_PIN, GPIO_OUT);
    gpio_put(PICO_DEFAULT_LED_PIN, 1);

    printf("NeoPico-HD: MVS Capture + DVI Output\n");

    // Initialize DVI
    dvi0.timing = &DVI_TIMING;
    dvi0.ser_cfg = neopico_dvi_cfg;
    dvi_init(&dvi0, next_striped_spin_lock_num(), next_striped_spin_lock_num());

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

    // Start with black frame
    memset(mvs_frame, 0, sizeof(mvs_frame));

    // Pre-generate first scanline
    generate_scanline(scanline_buf[0], 0);

    // Launch DVI on Core 1
    multicore_launch_core1(core1_main);

    printf("NeoPico-HD: Starting capture + DVI\n");

    // Start capture
    pio_sm_set_enabled(pio_mvs, sm_sync, true);
    dma_channel_set_write_addr(dma_chan, raw_buffer, false);
    dma_channel_set_trans_count(dma_chan, RAW_BUFFER_WORDS, false);
    pio_sm_set_enabled(pio_mvs, sm_pixel, true);
    dma_channel_start(dma_chan);
    pio_sm_exec(pio_mvs, sm_sync, pio_encode_irq_set(false, 4));

    uint buf_idx = 0;
    uint32_t words_captured = 0;

    // VSYNC detection state
    uint32_t short_count = 0;
    bool in_vsync = false;
    bool frame_captured = false;

    while (true) {
        static uint32_t dvi_frames = 0;
        dvi_frames++;
        gpio_put(PICO_DEFAULT_LED_PIN, (dvi_frames / 30) & 1);

        // Generate DVI frame while capturing MVS
        for (uint y = 0; y < FRAME_HEIGHT; ++y) {
            const uint16_t *scanline = scanline_buf[buf_idx];
            queue_add_blocking_u32(&dvi0.q_colour_valid, &scanline);

            buf_idx ^= 1;
            generate_scanline(scanline_buf[buf_idx], (y + 1) % FRAME_HEIGHT);

            while (queue_try_remove_u32(&dvi0.q_colour_free, &scanline))
                ;

            // Non-blocking VSYNC detection
            while (!pio_sm_is_rx_fifo_empty(pio_mvs, sm_sync)) {
                uint32_t h = pio_sm_get(pio_mvs, sm_sync);
                bool is_short = (h <= H_THRESHOLD);

                if (!in_vsync) {
                    if (is_short) short_count++;
                    else {
                        if (short_count >= 8) in_vsync = true;
                        short_count = 0;
                    }
                } else {
                    if (is_short) short_count++;
                    else {
                        // VSYNC complete!
                        in_vsync = false;
                        short_count = 0;

                        // Stop and restart capture
                        dma_channel_abort(dma_chan);
                        words_captured = RAW_BUFFER_WORDS - dma_channel_hw_addr(dma_chan)->transfer_count;
                        frame_captured = true;

                        // Restart
                        dma_channel_set_write_addr(dma_chan, raw_buffer, false);
                        dma_channel_set_trans_count(dma_chan, RAW_BUFFER_WORDS, false);
                        pio_sm_set_enabled(pio_mvs, sm_pixel, false);
                        pio_sm_clear_fifos(pio_mvs, sm_pixel);
                        pio_sm_restart(pio_mvs, sm_pixel);
                        pio_sm_exec(pio_mvs, sm_pixel, pio_encode_jmp(offset_pixel));
                        pio_sm_set_enabled(pio_mvs, sm_pixel, true);
                        dma_channel_start(dma_chan);
                        pio_sm_exec(pio_mvs, sm_sync, pio_encode_irq_set(false, 4));
                    }
                }
            }
        }

        // Process frame after DVI scanlines done
        if (frame_captured && words_captured > 10000) {
            process_frame(raw_buffer, mvs_frame, words_captured);
            frame_count++;
            frame_captured = false;
        }
    }

    return 0;
}
