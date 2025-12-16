/**
 * HDMI I2S Audio Test - Color Bars + MVS Digital Audio
 *
 * Captures digital I2S audio from MVS and outputs through HDMI.
 * Much cleaner than ADC - no analog noise!
 *
 * Hardware:
 *   GPIO 40: I2S DAT (data)
 *   GPIO 41: I2S WS (word select / LRCLK)
 *   GPIO 42: I2S BCK (bit clock)
 *   DVI pins: GPIO 16-21, 26-27
 */

#include <stdio.h>
#include <stdlib.h>
#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "hardware/clocks.h"
#include "hardware/irq.h"
#include "hardware/sync.h"
#include "hardware/gpio.h"
#include "hardware/vreg.h"
#include "hardware/pio.h"
#include "hardware/dma.h"

#include "dvi.h"
#include "dvi_serialiser.h"
#include "audio_ring.h"

#include "neopico_config.h"
#include "audio_config.h"
#include "i2s_audio.pio.h"

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

#define AUDIO_SAMPLE_RATE 48000  // Standard HDMI audio rate
#define AUDIO_BUFFER_SIZE 256

static audio_sample_t audio_buffer[AUDIO_BUFFER_SIZE];

// I2S receive state
static PIO i2s_pio = pio1;  // Use pio1 (pio0 is for DVI)
static uint i2s_sm = 0;

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
// I2S Audio Processing
// =============================================================================

// Debug: track raw sample values
static uint32_t last_sample_l = 0;
static uint32_t last_sample_r = 0;
static uint32_t sample_count = 0;
static int16_t last_out_l = 0;
static int16_t last_out_r = 0;

static void process_i2s_audio(void) {
    // PIO autopushes 16-bit samples continuously
    // Treat consecutive samples as L, R pairs
    while (pio_sm_get_rx_fifo_level(i2s_pio, i2s_sm) >= 2) {
        uint32_t raw_l = pio_sm_get(i2s_pio, i2s_sm);
        uint32_t raw_r = pio_sm_get(i2s_pio, i2s_sm);

        last_sample_l = raw_l;
        last_sample_r = raw_r;
        sample_count++;

        // Lower 16 bits contain the sample (shift-left puts MSB at bit 15)
        int16_t left = (int16_t)(raw_l & 0xFFFF);
        int16_t right = (int16_t)(raw_r & 0xFFFF);

        last_out_l = left;
        last_out_r = right;

        int available = get_write_size(&dvi0.audio_ring, false);
        if (available > 0) {
            audio_sample_t *ptr = get_write_pointer(&dvi0.audio_ring);
            ptr->channels[0] = left;
            ptr->channels[1] = right;
            increase_write_pointer(&dvi0.audio_ring, 1);
        }
    }
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
    vreg_set_voltage(VREG_VSEL);
    sleep_ms(10);
    set_sys_clock_khz(DVI_TIMING.bit_clk_khz, true);
    stdio_init_all();

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
    printf("  HDMI I2S Audio Test - MVS Digital\n");
    printf("========================================\n");
    printf("I2S Pins: DAT=%d, WS=%d, BCK=%d\n", AUDIO_PIN_DAT, AUDIO_PIN_WS, AUDIO_PIN_BCK);
    printf("Sample Rate: %d Hz\n", AUDIO_SAMPLE_RATE);
    printf("\n");

    // Configure I2S pins as inputs
    // Pins must be consecutive: DAT, WS, BCK
    gpio_init(AUDIO_PIN_DAT);
    gpio_init(AUDIO_PIN_WS);
    gpio_init(AUDIO_PIN_BCK);
    gpio_set_dir(AUDIO_PIN_DAT, GPIO_IN);
    gpio_set_dir(AUDIO_PIN_WS, GPIO_IN);
    gpio_set_dir(AUDIO_PIN_BCK, GPIO_IN);

    // Quick GPIO test before PIO takes over
    printf("GPIO test (should toggle with signal):\n");
    for (int i = 0; i < 5; i++) {
        printf("  %d: DAT=%d WS=%d BCK=%d\n", i,
               gpio_get(AUDIO_PIN_DAT), gpio_get(AUDIO_PIN_WS), gpio_get(AUDIO_PIN_BCK));
        sleep_ms(100);
    }

    // Initialize I2S receiver on PIO1
    uint offset = pio_add_program(i2s_pio, &i2s_rx_program);
    i2s_rx_program_init(i2s_pio, i2s_sm, offset, AUDIO_PIN_DAT, AUDIO_PIN_WS, AUDIO_PIN_BCK);
    printf("I2S PIO initialized\n");

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
    dvi_set_audio_freq(&dvi0, AUDIO_SAMPLE_RATE, 28000, 6144);
    printf("HDMI audio configured: %d Hz\n", AUDIO_SAMPLE_RATE);

    // Launch DVI on core 1
    multicore_launch_core1(core1_main);
    printf("DVI output started\n");
    printf("\nI2S digital audio -> HDMI\n\n");

    static uint frame_num = 0;
    uint buf_idx = 0;

    printf("\n=== I2S USB Capture Test ===\n");
    printf("Pins: DAT=%d, WS=%d, BCK=%d\n\n", AUDIO_PIN_DAT, AUDIO_PIN_WS, AUDIO_PIN_BCK);

    // Capture 1024 samples to a buffer, then dump to USB
    printf("Capturing 1024 samples in 3 seconds...\n");
    sleep_ms(3000);

    #define USB_CAPTURE_SIZE 1024
    static int16_t usb_samples[USB_CAPTURE_SIZE * 2];  // L,R pairs

    // Wait for FIFO to have data
    while (pio_sm_get_rx_fifo_level(i2s_pio, i2s_sm) < 2) {
        tight_loop_contents();
    }

    // Capture samples
    for (int i = 0; i < USB_CAPTURE_SIZE; i++) {
        while (pio_sm_get_rx_fifo_level(i2s_pio, i2s_sm) < 2) {
            tight_loop_contents();
        }
        uint32_t raw_l = pio_sm_get(i2s_pio, i2s_sm);
        uint32_t raw_r = pio_sm_get(i2s_pio, i2s_sm);
        usb_samples[i*2] = (int16_t)(raw_l & 0xFFFF);
        usb_samples[i*2+1] = (int16_t)(raw_r & 0xFFFF);
    }

    printf("Captured! Dumping as CSV (left,right):\n");
    printf("---START---\n");
    for (int i = 0; i < USB_CAPTURE_SIZE; i++) {
        printf("%d,%d\n", usb_samples[i*2], usb_samples[i*2+1]);
    }
    printf("---END---\n\n");
    printf("Now running normal audio output...\n\n");

    while (true) {
        // Process any pending I2S audio
        process_i2s_audio();

        for (uint y = 0; y < FRAME_HEIGHT; ++y) {
            generate_color_bar_line(scanline_buf[buf_idx], y);
            const uint16_t *scanline = scanline_buf[buf_idx];
            queue_add_blocking_u32(&dvi0.q_colour_valid, &scanline);
            buf_idx ^= 1;
            while (queue_try_remove_u32(&dvi0.q_colour_free, &scanline));

            // Process I2S between scanlines too
            process_i2s_audio();
        }

        frame_num++;

        if ((frame_num % 30) == 0) {
            gpio_xor_mask(1u << PICO_DEFAULT_LED_PIN);
        }
        if ((frame_num % 60) == 0) {
            uint fifo_level = pio_sm_get_rx_fifo_level(i2s_pio, i2s_sm);
            printf("F%u FIFO:%u cnt:%lu raw:0x%08lX/0x%08lX out:%d/%d\n",
                   frame_num, fifo_level,
                   (unsigned long)sample_count,
                   (unsigned long)last_sample_l,
                   (unsigned long)last_sample_r,
                   last_out_l, last_out_r);
        }
    }

    return 0;
}
