/**
 * HDMI ADC Audio Test - Color Bars + MVS Audio Capture
 *
 * Captures audio from MVS via ADC and outputs through HDMI.
 * Video shows color bars for simplicity.
 *
 * Hardware:
 *   GPIO 40 (ADC0): MVS audio from OpenMVS breakout
 *   DVI pins: Same as dvi_test (GPIO 16-21, 26-27)
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
#include "hardware/adc.h"

#include "dvi.h"
#include "dvi_serialiser.h"
#include "audio_ring.h"

#include "neopico_config.h"

// ADC pin for MVS audio (RP2350B: GPIO 40-47 are ADC pins)
#define ADC_PIN_AUDIO 40
#define ADC_CHANNEL 0  // GPIO 40 = ADC0 on RP2350B


// =============================================================================
// Display Configuration
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

#define AUDIO_SAMPLE_RATE 44100
#define AUDIO_BUFFER_SIZE 256  // Must be power of 2

// Audio buffer for HDMI ring buffer
static audio_sample_t audio_buffer[AUDIO_BUFFER_SIZE];

// Timer for audio capture
static struct repeating_timer audio_timer;

// Debug: track ADC min/max values
static uint16_t adc_min = 4095;
static uint16_t adc_max = 0;
static uint32_t adc_sum = 0;
static uint32_t adc_count = 0;

// =============================================================================
// Color Bar Generation
// =============================================================================

#define COLOR_WHITE   0xFFFF
#define COLOR_YELLOW  0xFFE0
#define COLOR_CYAN    0x07FF
#define COLOR_GREEN   0x07E0
#define COLOR_MAGENTA 0xF81F
#define COLOR_RED     0xF800
#define COLOR_BLUE    0x001F
#define COLOR_BLACK   0x0000

static const uint16_t color_bars[] = {
    COLOR_WHITE, COLOR_YELLOW, COLOR_CYAN, COLOR_GREEN,
    COLOR_MAGENTA, COLOR_RED, COLOR_BLUE, COLOR_BLACK
};

#define NUM_BARS 8
#define BAR_WIDTH (FRAME_WIDTH / NUM_BARS)

static void generate_color_bar_line(uint16_t *buf, uint y) {
    for (uint x = 0; x < FRAME_WIDTH; x++) {
        uint bar_idx = x / BAR_WIDTH;
        if (bar_idx >= NUM_BARS) bar_idx = NUM_BARS - 1;
        buf[x] = color_bars[bar_idx];
    }
}

// =============================================================================
// Audio Capture - ADC sampling
// =============================================================================

// DC offset tracking and low-pass filter
static int32_t dc_offset = 1325;
static int32_t lpf_state = 0;  // Low-pass filter state

// Audio timer callback - ADC capture with filtering
static bool audio_timer_callback(struct repeating_timer *t) {
    int available = get_write_size(&dvi0.audio_ring, false);
    if (available == 0) return true;

    // Read ADC and track stats
    uint16_t raw = adc_read();
    if (raw < adc_min) adc_min = raw;
    if (raw > adc_max) adc_max = raw;
    adc_sum += raw;
    adc_count++;

    // Update DC offset slowly
    dc_offset += ((int32_t)raw - dc_offset) >> 10;

    // Convert to signed audio centered on DC offset
    int32_t input = ((int32_t)raw - dc_offset) * 8;  // Reduced gain

    // Low-pass filter to reduce high-freq noise (cutoff ~5kHz)
    // y = y + 0.5 * (x - y)
    lpf_state = lpf_state + ((input - lpf_state) >> 1);

    int32_t sample = lpf_state;
    if (sample > 32767) sample = 32767;
    if (sample < -32768) sample = -32768;

    audio_sample_t *ptr = get_write_pointer(&dvi0.audio_ring);
    ptr->channels[0] = (int16_t)sample;
    ptr->channels[1] = (int16_t)sample;
    increase_write_pointer(&dvi0.audio_ring, 1);

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
    // Set voltage first (required for stable high-speed operation)
    vreg_set_voltage(VREG_VSEL);
    sleep_ms(10);

    // Set system clock for DVI timing
    set_sys_clock_khz(DVI_TIMING.bit_clk_khz, true);

    // Initialize stdio for debug output
    stdio_init_all();

    // LED init
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
    printf("========================================\n");
    printf("  HDMI ADC Audio Test - MVS Capture\n");
    printf("========================================\n");
    printf("ADC Input: GPIO %d\n", ADC_PIN_AUDIO);
    printf("Sample Rate: %d Hz\n", AUDIO_SAMPLE_RATE);
    printf("\n");

    // Initialize ADC
    adc_init();
    adc_gpio_init(ADC_PIN_AUDIO);
    adc_select_input(ADC_CHANNEL);
    printf("ADC initialized on GPIO %d\n", ADC_PIN_AUDIO);

    // Initialize DVI
    neopico_dvi_gpio_setup();
    dvi0.timing = &DVI_TIMING;
    dvi0.ser_cfg = neopico_dvi_cfg;
    dvi_init(&dvi0, next_striped_spin_lock_num(), next_striped_spin_lock_num());
    printf("DVI initialized\n");

    // Configure audio
    dvi_get_blank_settings(&dvi0)->top = 4 * 2;
    dvi_get_blank_settings(&dvi0)->bottom = 4 * 2;
    dvi_audio_sample_buffer_set(&dvi0, audio_buffer, AUDIO_BUFFER_SIZE);

    // Set audio frequency with CTS/N values for 44.1kHz at 25.2MHz pixel clock
    // Standard HDMI values: N=6272, CTS=28000 for 44.1kHz
    dvi_set_audio_freq(&dvi0, AUDIO_SAMPLE_RATE, 28000, 6272);
    printf("HDMI audio configured: %d Hz\n", AUDIO_SAMPLE_RATE);

    // Start audio capture timer at 44.1kHz (every ~23 microseconds)
    // Negative value means the delay is between end of callback and start of next
    add_repeating_timer_us(-23, audio_timer_callback, NULL, &audio_timer);
    printf("Audio timer started at ~44kHz\n");

    // Launch DVI on core 1
    multicore_launch_core1(core1_main);
    printf("DVI output started\n");
    printf("\nMVS audio on GPIO %d -> HDMI output\n\n", ADC_PIN_AUDIO);

    static uint frame_num = 0;
    uint buf_idx = 0;

    while (true) {
        for (uint y = 0; y < FRAME_HEIGHT; ++y) {
            generate_color_bar_line(scanline_buf[buf_idx], y);
            const uint16_t *scanline = scanline_buf[buf_idx];
            queue_add_blocking_u32(&dvi0.q_colour_valid, &scanline);
            buf_idx ^= 1;
            while (queue_try_remove_u32(&dvi0.q_colour_free, &scanline));
        }

        frame_num++;
        if ((frame_num % 30) == 0) {
            gpio_xor_mask(1u << PICO_DEFAULT_LED_PIN);
        }
        if ((frame_num % 60) == 0) {
            uint32_t avg = adc_count > 0 ? adc_sum / adc_count : 0;
            printf("ADC: min=%u max=%u avg=%lu range=%u\n",
                   adc_min, adc_max, (unsigned long)avg, adc_max - adc_min);
            // Reset stats
            adc_min = 4095;
            adc_max = 0;
            adc_sum = 0;
            adc_count = 0;
        }
    }

    return 0;
}
