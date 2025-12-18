/**
 * DVI Test - Color Bars
 *
 * Simple test to verify HDMI/DVI output is working.
 * Displays color bars at 640x480 (320x240 pixel-doubled).
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

#include "dvi.h"
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
// Color Bar Generation
// =============================================================================

// RGB565 colors for color bars
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

// Generate classic SMPTE-style color bar line
static void generate_color_bar_line(uint16_t *buf, uint y) {
    for (uint x = 0; x < FRAME_WIDTH; x++) {
        uint bar_idx = (x * NUM_BARS) / FRAME_WIDTH;
        buf[x] = color_bars[bar_idx];
    }

    // Add horizontal white lines at y=60, 120, 180 to detect scrolling
    if (y == 60 || y == 120 || y == 180) {
        for (uint x = 0; x < FRAME_WIDTH; x++) {
            buf[x] = 0xFFFF;  // White
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

    printf("NeoPico-HD DVI Test - Color Bars\n");
    printf("Resolution: %dx%d (pixel-doubled to 640x480)\n", FRAME_WIDTH, FRAME_HEIGHT);
    printf("DVI pins: Clock GP25-26, Data GP27-32\n");

    // Initialize DVI
    dvi0.timing = &DVI_TIMING;
    dvi0.ser_cfg = neopico_dvi_cfg;
    dvi_init(&dvi0, next_striped_spin_lock_num(), next_striped_spin_lock_num());

    // Launch DVI on core 1
    multicore_launch_core1(core1_main);

    printf("DVI initialized, outputting color bars...\n");

    static uint frame_num = 0;

    // Main loop - feed scanlines to DVI
    uint buf_idx = 0;
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

        // Toggle LED each second
        if ((frame_num % 60) == 0) {
            gpio_xor_mask(1u << PICO_DEFAULT_LED_PIN);
        }
    }

    return 0;
}
