/**
 * HDMI Audio Test - Color Bars + Sine Wave Tone
 *
 * Tests HDMI audio output using ikjordan/PicoDVI fork.
 * Displays color bars and outputs a 440Hz sine wave tone.
 *
 * Pin Configuration (same as dvi_test):
 *   DVI Data:  GPIO 16-21 (3 differential pairs)
 *   DVI Clock: GPIO 26-27
 */

#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "hardware/clocks.h"
#include "hardware/irq.h"
#include "hardware/sync.h"
#include "hardware/gpio.h"
#include "hardware/vreg.h"

#include "dvi.h"
#include "dvi_serialiser.h"
#include "audio_ring.h"

#include "neopico_config.h"

// =============================================================================
// Display Configuration
// =============================================================================

#define FRAME_WIDTH 320
#define FRAME_HEIGHT 240
#define VREG_VSEL VREG_VOLTAGE_1_20
#define DVI_TIMING dvi_timing_640x480p_60hz

struct dvi_inst dvi0;

// Scanline buffer (double-buffered)
static uint16_t scanline_buf[2][FRAME_WIDTH];

// =============================================================================
// Color Bar Generation (standard SMPTE-style)
// =============================================================================

// RGB565 colors
#define COLOR_WHITE   0xFFFF
#define COLOR_YELLOW  0xFFE0
#define COLOR_CYAN    0x07FF
#define COLOR_GREEN   0x07E0
#define COLOR_MAGENTA 0xF81F
#define COLOR_RED     0xF800
#define COLOR_BLUE    0x001F
#define COLOR_BLACK   0x0000

static const uint16_t color_bars[] = {
    COLOR_WHITE,
    COLOR_YELLOW,
    COLOR_CYAN,
    COLOR_GREEN,
    COLOR_MAGENTA,
    COLOR_RED,
    COLOR_BLUE,
    COLOR_BLACK
};

#define NUM_BARS (sizeof(color_bars) / sizeof(color_bars[0]))
#define BAR_WIDTH (FRAME_WIDTH / NUM_BARS)

static void generate_color_bar_line(uint16_t *buf, uint y) {
    for (uint x = 0; x < FRAME_WIDTH; x++) {
        uint bar_idx = x / BAR_WIDTH;
        if (bar_idx >= NUM_BARS) bar_idx = NUM_BARS - 1;
        buf[x] = color_bars[bar_idx];
    }
}

// =============================================================================
// Audio Configuration
// =============================================================================

#define AUDIO_SAMPLE_RATE 44100
#define AUDIO_BUFFER_SIZE 256  // Must be power of 2

// Audio buffer for ring buffer
static audio_sample_t audio_buffer[AUDIO_BUFFER_SIZE];

// Pre-computed sine wave table (one period)
#define SINE_TABLE_SIZE 128
static int16_t sine_table[SINE_TABLE_SIZE];

// Audio timer
static struct repeating_timer audio_timer;

// Tone frequency (A4 = 440Hz)
#define TONE_FREQ 440

// Calculate step through sine table per sample
// step = (TONE_FREQ * SINE_TABLE_SIZE) / AUDIO_SAMPLE_RATE
// For 440Hz: (440 * 128) / 44100 ≈ 1.277
// We use fixed-point: step_fp = (440 * 128 * 65536) / 44100
#define SINE_STEP_FP ((uint32_t)(((uint64_t)TONE_FREQ * SINE_TABLE_SIZE * 65536) / AUDIO_SAMPLE_RATE))

static uint32_t sine_phase = 0;  // Fixed-point phase accumulator

// Generate sine table at startup
static void init_sine_table(void) {
    for (int i = 0; i < SINE_TABLE_SIZE; i++) {
        // Generate 16-bit signed samples, amplitude ~0x3FFF to avoid clipping
        sine_table[i] = (int16_t)(sinf(2.0f * M_PI * i / SINE_TABLE_SIZE) * 0x3FFF);
    }
}

// Audio callback - called every 2ms to fill audio buffer
static bool audio_timer_callback(struct repeating_timer *t) {
    int available = get_write_size(&dvi0.audio_ring, false);
    if (available == 0) return true;

    audio_sample_t *ptr = get_write_pointer(&dvi0.audio_ring);

    for (int i = 0; i < available; i++) {
        // Get sample from sine table using fixed-point phase
        uint32_t idx = (sine_phase >> 16) % SINE_TABLE_SIZE;
        int16_t sample = sine_table[idx];

        // Stereo: same sample to both channels
        ptr->channels[0] = sample;
        ptr->channels[1] = sample;
        ptr++;

        // Advance phase
        sine_phase += SINE_STEP_FP;
    }

    increase_write_pointer(&dvi0.audio_ring, available);
    return true;
}

// =============================================================================
// DVI Core 1 Handler
// =============================================================================

void core1_main() {
    dvi_register_irqs_this_core(&dvi0, DMA_IRQ_0);
    while (queue_is_empty(&dvi0.q_colour_valid))
        __wfe();
    dvi_start(&dvi0);
    dvi_scanbuf_main_16bpp(&dvi0);
}

// =============================================================================
// Main
// =============================================================================

int main() {
    // Set voltage for stable DVI output
    vreg_set_voltage(VREG_VSEL);
    sleep_ms(10);

    // Set system clock for DVI timing
    set_sys_clock_khz(DVI_TIMING.bit_clk_khz, true);

    // Initialize stdio for debug output
    stdio_init_all();

    // Blink LED to show we're alive
    gpio_init(PICO_DEFAULT_LED_PIN);
    gpio_set_dir(PICO_DEFAULT_LED_PIN, GPIO_OUT);
    gpio_put(PICO_DEFAULT_LED_PIN, 1);

    printf("\n");
    printf("========================================\n");
    printf("  HDMI Audio Test - Color Bars + Tone\n");
    printf("========================================\n");
    printf("Resolution: %dx%d (pixel-doubled to 640x480)\n", FRAME_WIDTH, FRAME_HEIGHT);
    printf("Audio: %d Hz sine wave @ %d Hz sample rate\n", TONE_FREQ, AUDIO_SAMPLE_RATE);
    printf("DVI pins: Data GP16-21, Clock GP26-27\n");
    printf("\n");

    // Initialize sine wave table
    init_sine_table();
    printf("Sine table initialized\n");

    // Initialize DVI
    neopico_dvi_gpio_setup();
    dvi0.timing = &DVI_TIMING;
    dvi0.ser_cfg = neopico_dvi_cfg;
    dvi_init(&dvi0, next_striped_spin_lock_num(), next_striped_spin_lock_num());
    printf("DVI initialized\n");

    // Configure audio
    // Blank lines needed for data islands (audio packets)
    dvi_get_blank_settings(&dvi0)->top = 4 * 2;
    dvi_get_blank_settings(&dvi0)->bottom = 4 * 2;

    // Set up audio buffer
    dvi_audio_sample_buffer_set(&dvi0, audio_buffer, AUDIO_BUFFER_SIZE);

    // Set audio frequency with CTS/N values for 44.1kHz
    // CTS and N values for HDMI audio clock regeneration
    // For 25.2MHz pixel clock (640x480) and 44.1kHz audio:
    // N = 6272, CTS = 28000 (standard HDMI values)
    dvi_set_audio_freq(&dvi0, AUDIO_SAMPLE_RATE, 28000, 6272);
    printf("Audio configured: %d Hz, buffer size %d\n", AUDIO_SAMPLE_RATE, AUDIO_BUFFER_SIZE);

    // Start audio timer (2ms interval = 500Hz callback rate)
    add_repeating_timer_ms(2, audio_timer_callback, NULL, &audio_timer);
    printf("Audio timer started\n");

    // Launch DVI on core 1
    multicore_launch_core1(core1_main);
    printf("DVI core launched\n");

    printf("\nOutputting color bars with %d Hz tone...\n", TONE_FREQ);
    printf("Connect HDMI to a TV/monitor with speakers!\n\n");

    static uint frame_num = 0;
    uint buf_idx = 0;

    // Main loop - feed scanlines to DVI
    while (true) {
        for (uint y = 0; y < FRAME_HEIGHT; ++y) {
            // Generate color bar line
            generate_color_bar_line(scanline_buf[buf_idx], y);

            // Get pointer to current scanline buffer
            const uint16_t *scanline = scanline_buf[buf_idx];

            // Queue this scanline for display
            queue_add_blocking_u32(&dvi0.q_colour_valid, &scanline);

            // Switch to other buffer
            buf_idx ^= 1;

            // Discard any returned buffers
            while (queue_try_remove_u32(&dvi0.q_colour_free, &scanline))
                ;
        }

        frame_num++;

        // Status every 5 seconds
        if ((frame_num % 300) == 0) {
            printf("Frame %u - Audio ring: read=%lu write=%lu\n",
                   frame_num,
                   (unsigned long)dvi0.audio_ring.read,
                   (unsigned long)dvi0.audio_ring.write);
            gpio_xor_mask(1u << PICO_DEFAULT_LED_PIN);
        }
    }

    return 0;
}
