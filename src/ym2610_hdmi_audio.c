/**
 * YM2610 Digital Audio -> HDMI
 *
 * Captures digital audio from Neo Geo MVS MV1C board.
 * The MV1C uses NEO-YSA2 + BU9480F DAC which outputs 16-bit linear PCM
 * (NOT YM2610 floating-point format).
 *
 * Format: Right-justified, ~55.5kHz sample rate, 16-bit signed PCM
 *
 * Hardware connections (directly via level shifter):
 *   GPIO 2: BCK (øS) - bit clock
 *   GPIO 3: DAT (OPO) - serial data
 *   GPIO 4: WS (SH1) - word select
 *
 * DVI pins: as configured in neopico_config.h
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "hardware/clocks.h"
#include "hardware/irq.h"
#include "hardware/sync.h"
#include "hardware/gpio.h"
#include "hardware/vreg.h"
#include "hardware/pio.h"

#include "dvi.h"
#include "dvi_serialiser.h"
#include "audio_ring.h"

#include "neopico_config.h"
#include "audio_config.h"
#include "ym2610_audio.pio.h"

// =============================================================================
// Display Configuration - 480p for reliable HDMI audio
// =============================================================================

#define FRAME_WIDTH 320
#define FRAME_HEIGHT 240
#define VREG_VSEL VREG_VOLTAGE_1_20
#define DVI_TIMING dvi_timing_640x480p_60hz

struct dvi_inst dvi0;
static uint16_t scanline_buf[2][FRAME_WIDTH];

// =============================================================================
// Audio Configuration
// =============================================================================

#define AUDIO_SAMPLE_RATE 48000  // Standard HDMI rate
#define AUDIO_BUFFER_SIZE 256    // Must be power of 2

static audio_sample_t audio_buffer[AUDIO_BUFFER_SIZE];

// I2S/YM2610 capture state
static PIO audio_pio = pio1;
static uint audio_sm = 0;

// Debug counters and values
static volatile uint32_t samples_captured = 0;
static volatile uint32_t samples_dropped = 0;
static volatile uint32_t fifo_overflows = 0;
static volatile uint32_t exp0_count = 0;  // Count of exp=0 samples
static volatile int16_t last_left = 0;
static volatile int16_t last_right = 0;

// Clipping and signal analysis
static volatile int16_t min_sample_l = 32767;
static volatile int16_t max_sample_l = -32768;
static volatile int16_t min_sample_r = 32767;
static volatile int16_t max_sample_r = -32768;
static volatile uint32_t clip_count = 0;        // Samples near ±32767
static volatile uint32_t large_jump_count = 0;  // Sudden large deltas (bit errors?)
static volatile int16_t prev_left = 0;
static volatile int16_t prev_right = 0;
#define CLIP_THRESHOLD 30000    // Consider clipped if |sample| > this
#define JUMP_THRESHOLD 20000    // Suspicious if delta > this in one sample

// Test tone mode: 0=real capture, 1=test SRC, 2=dump raw to USB
static int test_tone_mode = 0;  // Real capture with YM2610 decode
static uint32_t tone_phase = 0;
#define TONE_FREQ 440
#define TONE_SAMPLE_RATE 55500
// Raw debug values (for analysis)
static volatile uint16_t last_raw_pio_l = 0;  // Raw from PIO (before bit reverse)
static volatile uint16_t last_raw_pio_r = 0;
static volatile uint16_t last_reversed_l = 0; // After bit reverse
static volatile uint16_t last_reversed_r = 0;

// Sample rate conversion: 55555 Hz input -> 48000 Hz output
// Using fractional accumulator method
// PIO captures stereo samples at ~55.5kHz (one L/R pair per WS cycle)
#define SRC_INPUT_RATE  55555
#define SRC_OUTPUT_RATE 48000
static uint32_t src_accumulator = 0;

// =============================================================================
// Color Bar Generation (simple test pattern)
// =============================================================================

static const uint16_t color_bars[] = {
    0xFFFF,  // White
    0xFFE0,  // Yellow
    0x07FF,  // Cyan
    0x07E0,  // Green
    0xF81F,  // Magenta
    0xF800,  // Red
    0x001F,  // Blue
    0x0000   // Black
};

static void generate_color_bar_line(uint16_t *buf, uint y) {
    const uint bar_width = FRAME_WIDTH / 8;
    for (uint x = 0; x < FRAME_WIDTH; x++) {
        uint bar = x / bar_width;
        if (bar >= 8) bar = 7;
        buf[x] = color_bars[bar];
    }
}

// =============================================================================
// Audio Processing
// =============================================================================

// Reverse bits in a 16-bit value
// Needed because YM2610 transmits LSB first, but our PIO shift-left
// puts the first bit at the high position
static inline uint16_t reverse_bits_16(uint16_t x) {
    x = ((x & 0x5555) << 1) | ((x & 0xAAAA) >> 1);
    x = ((x & 0x3333) << 2) | ((x & 0xCCCC) >> 2);
    x = ((x & 0x0F0F) << 4) | ((x & 0xF0F0) >> 4);
    x = ((x & 0x00FF) << 8) | ((x & 0xFF00) >> 8);
    return x;
}

// Process audio sample - Neo Geo outputs raw 16-bit signed PCM (not YM2610 float)
// Based on cps2_digiav reference which uses i2s_rx_asrc MODE=1 (right-justified)
// without any floating-point decode for Neo Geo
static inline int16_t decode_ym2610_sample(uint16_t raw) {
    // Treat as signed 16-bit PCM directly
    return (int16_t)raw;
}

// DC blocking filter state (simple IIR high-pass)
// y[n] = x[n] - x[n-1] + alpha * y[n-1]
// alpha = 0.995 (~63 in fixed point /64) gives ~10Hz cutoff at 27750Hz
static int32_t dc_filter_l_prev_in = 0;
static int32_t dc_filter_l_prev_out = 0;
static int32_t dc_filter_r_prev_in = 0;
static int32_t dc_filter_r_prev_out = 0;

#define DC_FILTER_ALPHA 63  // alpha = 63/64 = 0.984375

// =============================================================================
// FIR Lowpass Filter (from cps2_digiav)
// =============================================================================
// 16-tap symmetric lowpass filter for anti-aliasing before SRC
// Coefficients in Q15 fixed-point (scaled by 32768)
// Original: 0.0088, 0.0215, 0.0078, -0.0391, -0.0625, 0.0176, 0.1992, 0.3574...
#define FIR_TAPS 16

static const int16_t fir_coeffs[FIR_TAPS] = {
    288,    // 0.0088 * 32768
    704,    // 0.0215 * 32768
    256,    // 0.0078 * 32768
    -1281,  // -0.0391 * 32768
    -2048,  // -0.0625 * 32768
    577,    // 0.0176 * 32768
    6528,   // 0.1992 * 32768
    11710,  // 0.3574 * 32768
    11710,  // 0.3574 * 32768 (symmetric)
    6528,   // 0.1992 * 32768
    577,    // 0.0176 * 32768
    -2048,  // -0.0625 * 32768
    -1281,  // -0.0391 * 32768
    256,    // 0.0078 * 32768
    704,    // 0.0215 * 32768
    288     // 0.0088 * 32768
};

// FIR history buffers
static int16_t fir_history_l[FIR_TAPS];
static int16_t fir_history_r[FIR_TAPS];
static uint8_t fir_idx = 0;

// Apply FIR filter to get one output sample
static inline int16_t fir_filter(int16_t input, int16_t *history, uint8_t idx) {
    // Store new sample in history
    history[idx] = input;

    // Compute FIR output: sum of history * coefficients
    int32_t acc = 0;
    for (int i = 0; i < FIR_TAPS; i++) {
        // Circular buffer access
        uint8_t hist_idx = (idx - i) & (FIR_TAPS - 1);
        acc += (int32_t)history[hist_idx] * fir_coeffs[i];
    }

    // Scale back from Q15 (divide by 32768)
    acc >>= 15;

    // Clamp to int16 range
    if (acc > 32767) acc = 32767;
    if (acc < -32768) acc = -32768;

    return (int16_t)acc;
}

static inline int16_t dc_block_filter(int16_t in, int32_t *prev_in, int32_t *prev_out) {
    // y[n] = x[n] - x[n-1] + alpha * y[n-1] / 64
    int32_t out = in - *prev_in + (*prev_out * DC_FILTER_ALPHA) / 64;
    *prev_in = in;
    *prev_out = out;

    // Clip to int16 range
    if (out > 32767) out = 32767;
    if (out < -32768) out = -32768;
    return (int16_t)out;
}

// Simple sine lookup (256 entries, 8-bit index)
static const int16_t sine_table[256] = {
    0, 804, 1608, 2410, 3212, 4011, 4808, 5602, 6393, 7179, 7962, 8739, 9512, 10278, 11039, 11793,
    12539, 13279, 14010, 14732, 15446, 16151, 16846, 17530, 18204, 18868, 19519, 20159, 20787, 21403, 22005, 22594,
    23170, 23731, 24279, 24811, 25329, 25832, 26319, 26790, 27245, 27683, 28105, 28510, 28898, 29268, 29621, 29956,
    30273, 30571, 30852, 31113, 31356, 31580, 31785, 31971, 32137, 32285, 32412, 32521, 32609, 32678, 32728, 32757,
    32767, 32757, 32728, 32678, 32609, 32521, 32412, 32285, 32137, 31971, 31785, 31580, 31356, 31113, 30852, 30571,
    30273, 29956, 29621, 29268, 28898, 28510, 28105, 27683, 27245, 26790, 26319, 25832, 25329, 24811, 24279, 23731,
    23170, 22594, 22005, 21403, 20787, 20159, 19519, 18868, 18204, 17530, 16846, 16151, 15446, 14732, 14010, 13279,
    12539, 11793, 11039, 10278, 9512, 8739, 7962, 7179, 6393, 5602, 4808, 4011, 3212, 2410, 1608, 804,
    0, -804, -1608, -2410, -3212, -4011, -4808, -5602, -6393, -7179, -7962, -8739, -9512, -10278, -11039, -11793,
    -12539, -13279, -14010, -14732, -15446, -16151, -16846, -17530, -18204, -18868, -19519, -20159, -20787, -21403, -22005, -22594,
    -23170, -23731, -24279, -24811, -25329, -25832, -26319, -26790, -27245, -27683, -28105, -28510, -28898, -29268, -29621, -29956,
    -30273, -30571, -30852, -31113, -31356, -31580, -31785, -31971, -32137, -32285, -32412, -32521, -32609, -32678, -32728, -32757,
    -32767, -32757, -32728, -32678, -32609, -32521, -32412, -32285, -32137, -31971, -31785, -31580, -31356, -31113, -30852, -30571,
    -30273, -29956, -29621, -29268, -28898, -28510, -28105, -27683, -27245, -26790, -26319, -25832, -25329, -24811, -24279, -23731,
    -23170, -22594, -22005, -21403, -20787, -20159, -19519, -18868, -18204, -17530, -16846, -16151, -15446, -14732, -14010, -13279,
    -12539, -11793, -11039, -10278, -9512, -8739, -7962, -7179, -6393, -5602, -4808, -4011, -3212, -2410, -1608, -804
};

static void process_audio(void) {
    if (test_tone_mode == 2) {
        // DUMP RAW: Capture raw PIO data and send to USB for Python analysis
        // Format: binary stream of 16-bit L, 16-bit R values (raw, no decode)
        static uint32_t dump_count = 0;
        static int init_state = 0;  // 0=waiting, 1=countdown, 2=dumping

        if (init_state == 0) {
            printf("\n\n=== RAW AUDIO DUMP MODE ===\n");
            printf("Starting in 3 seconds...\n");
            sleep_ms(1000);
            printf("2...\n");
            sleep_ms(1000);
            printf("1...\n");
            sleep_ms(1000);
            init_state = 1;
        }

        if (init_state == 1) {
            // Clear any stale PIO data
            while (pio_sm_get_rx_fifo_level(audio_pio, audio_sm) > 0) {
                pio_sm_get(audio_pio, audio_sm);
            }
            printf("RAW_DUMP_START\n");
            dump_count = 0;
            init_state = 2;
        }

        if (init_state == 2) {
            while (pio_sm_get_rx_fifo_level(audio_pio, audio_sm) >= 2) {
                uint32_t raw_left = pio_sm_get(audio_pio, audio_sm);
                uint32_t raw_right = pio_sm_get(audio_pio, audio_sm);

                uint16_t l = raw_left & 0xFFFF;
                uint16_t r = raw_right & 0xFFFF;

                // Output as hex for easy parsing
                printf("%04X,%04X\n", l, r);

                dump_count++;
                if (dump_count >= 2000) {
                    printf("RAW_DUMP_END\n");
                    printf("Captured %lu samples. Reset to capture more.\n", (unsigned long)dump_count);
                    init_state = 3;  // Done
                    break;
                }
            }
        }
        return;
    }

    if (test_tone_mode == 1) {
        // TEST: Read PIO, generate test tone at PIO rate, run through SRC
        static int16_t src_buf[64];
        static uint8_t src_head = 0, src_count = 0;
        static uint32_t src_phase = 0;
        static uint32_t src_pos = 0;

        #define TONE_PHASE_INC ((uint32_t)(440ULL * 256ULL * 65536ULL / 55555ULL))
        #define SRC_RATIO_TEST ((uint32_t)(55555ULL * 65536ULL / 48000ULL))

        while (pio_sm_get_rx_fifo_level(audio_pio, audio_sm) >= 2 && src_count < 60) {
            pio_sm_get(audio_pio, audio_sm);
            pio_sm_get(audio_pio, audio_sm);
            samples_captured++;

            int16_t sample = sine_table[(src_phase >> 16) & 0xFF] / 4;
            src_phase += TONE_PHASE_INC;

            uint8_t idx = (src_head + src_count) & 63;
            src_buf[idx] = sample;
            src_count++;
        }

        int available = get_write_size(&dvi0.audio_ring, false);
        while (available > 0 && src_count >= 2) {
            uint8_t idx0 = src_head;
            uint8_t idx1 = (src_head + 1) & 63;

            uint16_t frac = src_pos & 0xFFFF;
            int32_t diff = src_buf[idx1] - src_buf[idx0];
            int16_t out = src_buf[idx0] + ((diff * frac) >> 16);

            audio_sample_t *ptr = get_write_pointer(&dvi0.audio_ring);
            ptr->channels[0] = out;
            ptr->channels[1] = out;
            increase_write_pointer(&dvi0.audio_ring, 1);
            available--;

            src_pos += SRC_RATIO_TEST;
            while (src_pos >= 0x10000 && src_count > 0) {
                src_pos -= 0x10000;
                src_head = (src_head + 1) & 63;
                src_count--;
            }
        }
        return;
    }

    // === RAW PASSTHROUGH ===
    // No SRC, no filters - raw samples to HDMI (plays ~15% fast)
    // This sounded best in testing

    int available = get_write_size(&dvi0.audio_ring, false);

    while (available > 0 && pio_sm_get_rx_fifo_level(audio_pio, audio_sm) >= 2) {
        uint32_t raw_left = pio_sm_get(audio_pio, audio_sm);
        uint32_t raw_right = pio_sm_get(audio_pio, audio_sm);
        samples_captured++;

        int16_t left = (int16_t)(raw_left & 0xFFFF);
        int16_t right = (int16_t)(raw_right & 0xFFFF);

        // Track stats
        if (left < min_sample_l) min_sample_l = left;
        if (left > max_sample_l) max_sample_l = left;
        if (right < min_sample_r) min_sample_r = right;
        if (right > max_sample_r) max_sample_r = right;

        // Output directly
        audio_sample_t *ptr = get_write_pointer(&dvi0.audio_ring);
        ptr->channels[0] = left;
        ptr->channels[1] = right;
        increase_write_pointer(&dvi0.audio_ring, 1);
        available--;
    }

    // Check for FIFO overflow
    if (pio_sm_is_rx_fifo_full(audio_pio, audio_sm)) {
        fifo_overflows++;
    }
}

// =============================================================================
// DVI Core 1
// =============================================================================

void core1_main() {
    dvi_register_irqs_this_core(&dvi0, DMA_IRQ_0);
    while (queue_is_empty(&dvi0.q_colour_valid))
        __wfe();
    dvi_start(&dvi0);
    dvi_scanbuf_main_16bpp(&dvi0);
}

// =============================================================================
// GPIO Signal Analysis (for debugging)
// =============================================================================

static void analyze_gpio_signals(void) {
    printf("\n=== GPIO Signal Analysis ===\n");
    printf("Sampling GPIO %d/%d/%d for 100ms...\n", AUDIO_PIN_BCK, AUDIO_PIN_DAT, AUDIO_PIN_WS);

    uint32_t bck_transitions = 0;
    uint32_t ws_transitions = 0;
    uint32_t dat_transitions = 0;

    bool last_bck = gpio_get(AUDIO_PIN_BCK);
    bool last_ws = gpio_get(AUDIO_PIN_WS);
    bool last_dat = gpio_get(AUDIO_PIN_DAT);

    absolute_time_t start = get_absolute_time();
    absolute_time_t end = make_timeout_time_ms(100);

    while (absolute_time_diff_us(get_absolute_time(), end) > 0) {
        bool bck = gpio_get(AUDIO_PIN_BCK);
        bool ws = gpio_get(AUDIO_PIN_WS);
        bool dat = gpio_get(AUDIO_PIN_DAT);

        if (bck != last_bck) { bck_transitions++; last_bck = bck; }
        if (ws != last_ws) { ws_transitions++; last_ws = ws; }
        if (dat != last_dat) { dat_transitions++; last_dat = dat; }
    }

    uint32_t elapsed_us = absolute_time_diff_us(start, get_absolute_time());

    printf("Results (in %lu us):\n", (unsigned long)elapsed_us);
    printf("  BCK: %lu transitions -> %.1f kHz\n",
           (unsigned long)bck_transitions,
           bck_transitions * 1000.0 / elapsed_us);
    printf("  WS:  %lu transitions -> %.1f kHz\n",
           (unsigned long)ws_transitions,
           ws_transitions * 1000.0 / elapsed_us);
    printf("  DAT: %lu transitions -> %.1f kHz\n",
           (unsigned long)dat_transitions,
           dat_transitions * 1000.0 / elapsed_us);

    // Expected for YM2610:
    // - WS: ~55 kHz (sample rate) * 2 edges = ~110 kHz
    // - BCK: ~55 kHz * 32 bits * 2 edges = ~3.5 MHz
    // - DAT: varies with audio content

    if (bck_transitions < 1000) {
        printf("\nWARNING: BCK signal seems too slow or missing!\n");
        printf("Expected ~3.5 MHz (700k transitions in 100ms)\n");
    }
    if (ws_transitions < 100) {
        printf("\nWARNING: WS signal seems too slow or missing!\n");
        printf("Expected ~110 kHz (22k transitions in 100ms)\n");
    }

    printf("\n");
}

// =============================================================================
// Raw Sample Capture (for debugging)
// =============================================================================

static void capture_raw_samples(void) {
    printf("\n=== Raw Sample Capture ===\n");
    printf("Capturing 64 samples...\n\n");

    // Clear any stale data
    while (!pio_sm_is_rx_fifo_empty(audio_pio, audio_sm)) {
        pio_sm_get(audio_pio, audio_sm);
    }

    printf("idx,raw_L,raw_R,L_dec,R_dec\n");

    for (int i = 0; i < 64; i++) {
        // Wait for samples with timeout
        int timeout = 100000;
        while (pio_sm_get_rx_fifo_level(audio_pio, audio_sm) < 2 && timeout > 0) {
            tight_loop_contents();
            timeout--;
        }

        if (timeout <= 0) {
            printf("TIMEOUT waiting for sample %d\n", i);
            break;
        }

        uint32_t raw_l = pio_sm_get(audio_pio, audio_sm);
        uint32_t raw_r = pio_sm_get(audio_pio, audio_sm);

        // Bit-reverse to get correct YM2610 format (LSB first transmission)
        uint16_t rev_l = reverse_bits_16(raw_l & 0xFFFF);
        uint16_t rev_r = reverse_bits_16(raw_r & 0xFFFF);

        // Decode
        int16_t dec_l = decode_ym2610_sample(rev_l);
        int16_t dec_r = decode_ym2610_sample(rev_r);

        printf("%d,0x%04X,0x%04X,%d,%d\n", i, rev_l, rev_r, dec_l, dec_r);
    }

    printf("\n");
}

// =============================================================================
// Main
// =============================================================================

int main() {
    // Set voltage and clock
    vreg_set_voltage(VREG_VSEL);
    sleep_ms(10);
    set_sys_clock_khz(DVI_TIMING.bit_clk_khz, true);

    stdio_init_all();

    // LED for status
    gpio_init(PICO_DEFAULT_LED_PIN);
    gpio_set_dir(PICO_DEFAULT_LED_PIN, GPIO_OUT);

    // Startup blink
    for (int i = 0; i < 5; i++) {
        gpio_put(PICO_DEFAULT_LED_PIN, 1);
        sleep_ms(50);
        gpio_put(PICO_DEFAULT_LED_PIN, 0);
        sleep_ms(50);
    }
    gpio_put(PICO_DEFAULT_LED_PIN, 1);

    printf("\n");
    printf("================================================\n");
    printf("  YM2610 Digital Audio -> HDMI Test\n");
    printf("================================================\n");
    printf("Pins: BCK=%d, DAT=%d, WS=%d\n", AUDIO_PIN_BCK, AUDIO_PIN_DAT, AUDIO_PIN_WS);
    printf("Audio: %d Hz -> HDMI\n", AUDIO_SAMPLE_RATE);
    printf("Video: Color bars (320x240)\n");
    printf("\n");

    // Configure audio pins as inputs (before PIO takes over)
    gpio_init(AUDIO_PIN_BCK);
    gpio_init(AUDIO_PIN_DAT);
    gpio_init(AUDIO_PIN_WS);
    gpio_set_dir(AUDIO_PIN_BCK, GPIO_IN);
    gpio_set_dir(AUDIO_PIN_DAT, GPIO_IN);
    gpio_set_dir(AUDIO_PIN_WS, GPIO_IN);

    // Analyze signals first
    analyze_gpio_signals();

    // Initialize audio capture on PIO1
    // BCK (GPIO 2) and WS (GPIO 4) are hardcoded in PIO, DAT pin is passed
    printf("Initializing PIO audio capture...\n");
    uint offset = pio_add_program(audio_pio, &ym2610_rx_program);
    ym2610_rx_program_init(audio_pio, audio_sm, offset, AUDIO_PIN_DAT);
    printf("PIO audio capture started (BCK=%d, DAT=%d, WS=%d)\n",
           AUDIO_PIN_BCK, AUDIO_PIN_DAT, AUDIO_PIN_WS);

    // Capture some raw samples for debugging
    capture_raw_samples();

    // Initialize DVI
    printf("Initializing DVI...\n");
    // Note: neopico_dvi_gpio_setup() removed - causes issues when audio PIO is also running
    dvi0.timing = &DVI_TIMING;
    dvi0.ser_cfg = neopico_dvi_cfg;
    dvi_init(&dvi0, next_striped_spin_lock_num(), next_striped_spin_lock_num());

    // Configure HDMI audio
    dvi_get_blank_settings(&dvi0)->top = 4 * 2;
    dvi_get_blank_settings(&dvi0)->bottom = 4 * 2;
    dvi_audio_sample_buffer_set(&dvi0, audio_buffer, AUDIO_BUFFER_SIZE);

    // CTS/N values for 48kHz at 25.2MHz pixel clock
    // N=6144, CTS=25200 for 48kHz
    dvi_set_audio_freq(&dvi0, AUDIO_SAMPLE_RATE, 25200, 6144);
    printf("HDMI audio configured: %d Hz\n", AUDIO_SAMPLE_RATE);

    // Launch DVI on Core 1
    multicore_launch_core1(core1_main);
    printf("DVI output started on Core 1\n\n");

    printf("Running... (status every 2 seconds)\n\n");

    uint frame_num = 0;
    uint buf_idx = 0;
    uint32_t last_status_time = time_us_32();
    uint32_t last_sample_count = 0;

    while (true) {
        // Process audio from PIO
        process_audio();

        // Generate and output one frame of color bars
        for (uint y = 0; y < FRAME_HEIGHT; y++) {
            generate_color_bar_line(scanline_buf[buf_idx], y);
            const uint16_t *scanline = scanline_buf[buf_idx];
            queue_add_blocking_u32(&dvi0.q_colour_valid, &scanline);
            buf_idx ^= 1;
            while (queue_try_remove_u32(&dvi0.q_colour_free, &scanline));

            // Process audio between scanlines
            process_audio();
        }

        frame_num++;

        // Blink LED
        if ((frame_num % 30) == 0) {
            gpio_xor_mask(1u << PICO_DEFAULT_LED_PIN);
        }

        // Status every 2 seconds
        uint32_t now = time_us_32();
        if (now - last_status_time >= 2000000) {
            uint32_t samples_per_sec = (samples_captured - last_sample_count) / 2;

            // Extract exp/mantissa for display
            int exp_l = (last_reversed_l >> 13) & 0x7;
            int exp_r = (last_reversed_r >> 13) & 0x7;
            int man_l = (last_reversed_l >> 3) & 0x3FF;
            int man_r = (last_reversed_r >> 3) & 0x3FF;

            printf("F:%u S:%lu (%lu/s)\n",
                   frame_num,
                   (unsigned long)samples_captured,
                   (unsigned long)samples_per_sec);

            // Signal range analysis (key diagnostic)
            int range_l = max_sample_l - min_sample_l;
            int range_r = max_sample_r - min_sample_r;
            printf("  RANGE L:[%d,%d]=%d  R:[%d,%d]=%d\n",
                   min_sample_l, max_sample_l, range_l,
                   min_sample_r, max_sample_r, range_r);

            // Clipping and error stats
            if (clip_count > 0 || large_jump_count > 0) {
                printf("  CLIP:%lu (>%d) JUMPS:%lu (>%d)\n",
                       (unsigned long)clip_count, CLIP_THRESHOLD,
                       (unsigned long)large_jump_count, JUMP_THRESHOLD);
            }

            if (fifo_overflows > 0) {
                printf("  WARNING: %lu FIFO overflows!\n", (unsigned long)fifo_overflows);
            }

            last_status_time = now;
            last_sample_count = samples_captured;

            // Reset min/max for next period
            min_sample_l = 32767; max_sample_l = -32768;
            min_sample_r = 32767; max_sample_r = -32768;
        }
    }

    return 0;
}
