/**
 * NeoPico-HD - MVS Capture with DVI Output
 *
 * Line-by-line capture for 60fps:
 * - No DMA, no raw buffer
 * - Read pixels directly from PIO FIFO
 * - Convert and write to framebuffer per-line
 *
 * Pin Configuration:
 *   MVS PCLK:      GPIO 0 (C2)
 *   MVS Green:     GPIO 1-5 (G4-G0, reversed bit order)
 *   MVS Blue:      GPIO 6-10 (B0-B4)
 *   MVS Red:       GPIO 11-15 (R0-R4)
 *   MVS CSYNC:     GPIO 22
 *   DVI Clock:     GPIO 25-26
 *   DVI Data:      GPIO 27-32
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
#include "hardware/dma.h"

#include "dvi.h"
#include "dvi_serialiser.h"
#include "audio_ring.h"
#include "mvs_capture.pio.h"
#include "neopico_config.h"

// I2S audio capture and processing
#include "audio/audio_buffer.h"
#include "audio/i2s_capture.h"
#include "audio/src.h"
#include "audio/dc_filter.h"
#include "audio/lowpass.h"

#define FRAME_WIDTH 320
#define FRAME_HEIGHT 240  // 480p: 240 * 2 = 480 DVI lines
#define MVS_HEIGHT 224
#define VREG_VSEL VREG_VOLTAGE_1_20

// Use 480p timing (252 MHz) - required for HDMI audio
// 240p @ 126 MHz doesn't have enough CPU headroom for audio data islands
#define DVI_TIMING dvi_timing_640x480p_60hz

struct dvi_inst dvi0;

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
// Buffers - Single framebuffer only (no raw buffer needed!)
// =============================================================================

static uint16_t g_framebuf[FRAME_WIDTH * FRAME_HEIGHT];

static volatile uint32_t frame_count = 0;

// =============================================================================
// Audio - Simple test tone
// =============================================================================

#define AUDIO_BUFFER_SIZE 1024  // Larger buffer to handle irregular fill timing
static audio_sample_t audio_buffer[AUDIO_BUFFER_SIZE];
static uint audio_sample_count = 0;

// Forward declaration - called during idle periods
static void fill_audio_buffer(void);

// =============================================================================
// I2S Audio Capture (PIO2 - separate from MVS on PIO1)
// =============================================================================

// I2S pins (directly on RP2350, active low accent on "audio" connector)
#define I2S_PIN_DAT 36
#define I2S_PIN_WS  37
#define I2S_PIN_BCK 38

static ap_ring_t i2s_ring;
static i2s_capture_t i2s_cap;
static src_t src;
static dc_filter_t dc_filter;
static lowpass_t lowpass;

// Sine wave table (128 samples)
static const int16_t sine_table[128] = {
    0x0000, 0x0648, 0x0C8C, 0x12C8, 0x18F9, 0x1F1A, 0x2528, 0x2B1F,
    0x30FC, 0x36BA, 0x3C57, 0x41CE, 0x471D, 0x4C40, 0x5134, 0x55F6,
    0x5A82, 0x5ED7, 0x62F2, 0x66D0, 0x6A6E, 0x6DCA, 0x70E3, 0x73B6,
    0x7642, 0x7885, 0x7A7D, 0x7C2A, 0x7D8A, 0x7E9D, 0x7F62, 0x7FD9,
    0x7FFF, 0x7FD9, 0x7F62, 0x7E9D, 0x7D8A, 0x7C2A, 0x7A7D, 0x7885,
    0x7642, 0x73B6, 0x70E3, 0x6DCA, 0x6A6E, 0x66D0, 0x62F2, 0x5ED7,
    0x5A82, 0x55F6, 0x5134, 0x4C40, 0x471D, 0x41CE, 0x3C57, 0x36BA,
    0x30FC, 0x2B1F, 0x2528, 0x1F1A, 0x18F9, 0x12C8, 0x0C8C, 0x0648,
    0x0000, 0xF9B8, 0xF374, 0xED38, 0xE707, 0xE0E6, 0xDAD8, 0xD4E1,
    0xCF04, 0xC946, 0xC3A9, 0xBE32, 0xB8E3, 0xB3C0, 0xAECC, 0xAA0A,
    0xA57E, 0xA129, 0x9D0E, 0x9930, 0x9592, 0x9236, 0x8F1D, 0x8C4A,
    0x89BE, 0x877B, 0x8583, 0x83D6, 0x8276, 0x8163, 0x809E, 0x8027,
    0x8001, 0x8027, 0x809E, 0x8163, 0x8276, 0x83D6, 0x8583, 0x877B,
    0x89BE, 0x8C4A, 0x8F1D, 0x9236, 0x9592, 0x9930, 0x9D0E, 0xA129,
    0xA57E, 0xAA0A, 0xAECC, 0xB3C0, 0xB8E3, 0xBE32, 0xC3A9, 0xC946,
    0xCF04, 0xD4E1, 0xDAD8, 0xE0E6, 0xE707, 0xED38, 0xF374, 0xF9B8,
};

// Poll I2S capture during safe periods
static void poll_i2s_capture(void) {
    i2s_capture_poll(&i2s_cap);
}

// Drain ring buffer (call during safe periods only)
static void drain_i2s_ring(void) {
    while (ap_ring_available(&i2s_ring) > 0) {
        ap_ring_read(&i2s_ring);
    }
}

// Debug: get DMA write index for I2S
static uint32_t get_i2s_dma_idx(void) {
    uint32_t write_ptr = dma_hw->ch[i2s_cap.dma_chan].write_addr;
    return (write_ptr - (uint32_t)i2s_cap.dma_buffer) / sizeof(uint32_t);
}

// Temporary buffers for SRC processing
static ap_sample_t src_in_buf[256];
static ap_sample_t src_out_buf[256];

// Called during vblank to fill HDMI audio buffer from captured I2S samples
static void fill_audio_buffer(void) {
    int space = get_write_size(&dvi0.audio_ring, false);
    if (space == 0) return;

    // Read available samples from I2S ring
    uint32_t available = ap_ring_available(&i2s_ring);
    if (available == 0) return;

    uint32_t to_read = (available < 256) ? available : 256;
    for (uint32_t i = 0; i < to_read; i++) {
        src_in_buf[i] = ap_ring_read(&i2s_ring);
    }

    // Apply filters (before SRC, at input sample rate)
    dc_filter_process_buffer(&dc_filter, src_in_buf, to_read);
    lowpass_process_buffer(&lowpass, src_in_buf, to_read);

    // Apply SRC (55.5kHz -> 48kHz)
    uint32_t in_consumed = 0;
    uint32_t out_max = (space < 256) ? space : 256;
    uint32_t out_count = src_process(&src, src_in_buf, to_read,
                                      src_out_buf, out_max, &in_consumed);

    // Write to HDMI audio ring
    audio_sample_t *audio_ptr = get_write_pointer(&dvi0.audio_ring);
    for (uint32_t i = 0; i < out_count; i++) {
        audio_ptr->channels[0] = src_out_buf[i].left;
        audio_ptr->channels[1] = src_out_buf[i].right;
        audio_ptr++;
    }
    increase_write_pointer(&dvi0.audio_ring, out_count);
}

// Vertical offset for centering MVS 224 lines in 240 line frame
static const uint8_t v_offset = (FRAME_HEIGHT - MVS_HEIGHT) / 2;

// =============================================================================
// DVI Scanline Callback - runs independently on Core 1's timing
// =============================================================================

static void core1_scanline_callback(uint line_num) {
    (void)line_num;  // We track internally for line doubling

    // Discard any scanline pointers passed back
    uint16_t *bufptr;
    while (queue_try_remove_u32(&dvi0.q_colour_free, &bufptr))
        ;

    // Track current DVI line (0-479 for 480p)
    static uint dvi_line = 4;  // First two framebuffer lines pushed = 4 DVI lines

    // Each framebuffer line shown twice (line doubling for 480p)
    uint scanline = dvi_line / 2;

    // Return pointer to current row in framebuffer
    bufptr = &g_framebuf[FRAME_WIDTH * scanline];
    queue_add_blocking_u32(&dvi0.q_colour_valid, &bufptr);

    dvi_line = (dvi_line + 1) % 480;  // 480p = 480 DVI lines
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
            // Don't poll I2S here - causes timing issues
            fill_audio_buffer();
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
    neopico_dvi_gpio_setup();  // Enable GP32 access for PIO
    dvi0.timing = &DVI_TIMING;
    dvi0.ser_cfg = neopico_dvi_cfg;
    dvi0.scanline_callback = core1_scanline_callback;
    dvi_init(&dvi0, next_striped_spin_lock_num(), next_striped_spin_lock_num());

    // HDMI Audio setup (no blank_settings, vblank-driven)
    dvi_audio_sample_buffer_set(&dvi0, audio_buffer, AUDIO_BUFFER_SIZE);
    dvi_set_audio_freq(&dvi0, 48000, 25200, 6144);
    // Pre-fill audio buffer before starting DVI
    for (int i = 0; i < 4; i++) fill_audio_buffer();
    printf("HDMI audio initialized (test tone, vblank-driven)\n");

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
    mvs_pixel_capture_program_init(pio_mvs, sm_pixel, offset_pixel, PIN_BASE, PIN_CSYNC, PIN_PCLK);

    // Enable sync detection
    pio_sm_set_enabled(pio_mvs, sm_sync, true);

    // Initialize I2S audio capture on PIO2 (separate from MVS on PIO1)
    ap_ring_init(&i2s_ring);
    i2s_capture_config_t i2s_cfg = {
        .pin_dat = I2S_PIN_DAT,
        .pin_ws = I2S_PIN_WS,
        .pin_bck = I2S_PIN_BCK,
        .pio = pio2,  // Use PIO2 (RP2350 has 3 PIOs!)
        .sm = 0,
    };
    i2s_capture_init(&i2s_cap, &i2s_cfg, &i2s_ring);
    i2s_capture_start(&i2s_cap);

    // Initialize SRC (55.5kHz -> 48kHz)
    src_init(&src, SRC_INPUT_RATE_DEFAULT, SRC_OUTPUT_RATE_DEFAULT);
    src_set_mode(&src, SRC_MODE_LINEAR);  // Linear interpolation for better quality

    // Initialize audio filters
    dc_filter_init(&dc_filter);
    dc_filter_set_enabled(&dc_filter, false);  // Disabled for comparison
    lowpass_init(&lowpass);
    lowpass_set_enabled(&lowpass, false);  // Disabled for comparison

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

        // DEBUG: Set to 1 for stats (causes occasional red line glitch)
        #if 0
        uint32_t now = time_us_32();
        if (now - last_time >= 5000000) {
            printf("FPS: %lu  I2S: %lu Hz\n",
                   fps_frame_count / 5,
                   i2s_capture_get_sample_rate(&i2s_cap));
            fps_frame_count = 0;
            last_time = now;
        }
        #endif

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
            fill_audio_buffer();  // Fill HDMI audio buffer only
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

        // 7. Poll I2S and fill HDMI audio (safe - no capture running)
        poll_i2s_capture();
        fill_audio_buffer();  // Consumes from i2s_ring, outputs to HDMI
    }

    return 0;
}
