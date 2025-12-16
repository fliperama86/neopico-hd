/**
 * YM2610 Digital Audio -> HDMI Test
 *
 * Captures digital audio from YM2610's serial output (before YM3016 DAC)
 * and sends it to HDMI. Video shows color bars for simplicity.
 *
 * Hardware connections (consecutive GPIOs):
 *   GPIO 22: BCK (øS) - bit clock from YM2610
 *   GPIO 23: DAT (OPO) - serial data from YM2610
 *   GPIO 24: WS (SH1) - word select / left channel latch
 *
 * DVI pins: GPIO 16-21 (data), 26-27 (clock)
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

#define AUDIO_SAMPLE_RATE 48000  // Standard HDMI rate, closer to YM2610's ~55kHz
#define AUDIO_BUFFER_SIZE 256    // Must be power of 2

static audio_sample_t audio_buffer[AUDIO_BUFFER_SIZE];

// I2S/YM2610 capture state
static PIO audio_pio = pio1;
static uint audio_sm = 0;

// Debug counters
static volatile uint32_t samples_captured = 0;
static volatile uint32_t samples_dropped = 0;
static volatile uint32_t fifo_overflows = 0;
static volatile int16_t last_left = 0;
static volatile int16_t last_right = 0;

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

// Decode YM2610 floating point format
// Format: bits[15:13]=exponent, bits[12:3]=mantissa, bits[2:0]=padding
static inline int16_t decode_ym2610_sample(uint16_t raw) {
    int exp = (raw >> 13) & 0x7;           // bits[15:13]
    int mantissa = (raw >> 3) & 0x3FF;     // bits[12:3]

    // Convert to signed (10-bit mantissa centered at 512)
    int32_t value = mantissa - 512;

    // Apply exponent shift
    if (exp > 0) {
        value <<= (exp - 1);
    }

    // Scale to better fill 16-bit range
    value <<= 4;

    // Clamp
    if (value > 32767) value = 32767;
    if (value < -32768) value = -32768;

    return (int16_t)value;
}

static void process_audio(void) {
    // Read all available samples from PIO FIFO
    while (pio_sm_get_rx_fifo_level(audio_pio, audio_sm) >= 2) {
        // PIO pushes 16-bit left, then 16-bit right
        // With shift-left and manual push after 16 bits, data is in LOWER 16 bits
        uint32_t raw_left = pio_sm_get(audio_pio, audio_sm);
        uint32_t raw_right = pio_sm_get(audio_pio, audio_sm);

        // Bit reversal needed - YM2610 sends LSB first
        uint16_t sample_left = reverse_bits_16(raw_left & 0xFFFF);
        uint16_t sample_right = reverse_bits_16(raw_right & 0xFFFF);

        // Extract and decode YM2610 floating point samples
        int16_t left = decode_ym2610_sample(sample_left);
        int16_t right = decode_ym2610_sample(sample_right);

        last_left = left;
        last_right = right;
        samples_captured++;

        // Write to HDMI audio ring buffer
        int available = get_write_size(&dvi0.audio_ring, false);
        if (available > 0) {
            audio_sample_t *ptr = get_write_pointer(&dvi0.audio_ring);
            ptr->channels[0] = left;
            ptr->channels[1] = right;
            increase_write_pointer(&dvi0.audio_ring, 1);
        } else {
            samples_dropped++;
        }
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

    // Initialize YM2610 audio capture on PIO1
    // BCK (GPIO 21) and WS (GPIO 24) are hardcoded in PIO, DAT pin is passed
    printf("Initializing PIO audio capture...\n");
    uint offset = pio_add_program(audio_pio, &ym2610_rx_program);
    ym2610_rx_program_init(audio_pio, audio_sm, offset, AUDIO_PIN_DAT);
    printf("PIO audio capture started (BCK=%d, DAT=%d, WS=%d)\n",
           AUDIO_PIN_BCK, AUDIO_PIN_DAT, AUDIO_PIN_WS);

    // Capture some raw samples for debugging
    capture_raw_samples();

    // Initialize DVI
    printf("Initializing DVI...\n");
    neopico_dvi_gpio_setup();
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
            uint fifo_level = pio_sm_get_rx_fifo_level(audio_pio, audio_sm);

            printf("F:%u S:%lu (%lu/s) FIFO:%u L:0x%04X(%d) R:0x%04X(%d)\n",
                   frame_num,
                   (unsigned long)samples_captured,
                   (unsigned long)samples_per_sec,
                   fifo_level,
                   (uint16_t)last_left, last_left,
                   (uint16_t)last_right, last_right);

            if (fifo_overflows > 0) {
                printf("  WARNING: %lu FIFO overflows!\n", (unsigned long)fifo_overflows);
            }
            if (samples_dropped > 0) {
                printf("  WARNING: %lu samples dropped!\n", (unsigned long)samples_dropped);
            }

            last_status_time = now;
            last_sample_count = samples_captured;
        }
    }

    return 0;
}
