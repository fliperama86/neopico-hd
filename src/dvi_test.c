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
#include "dvi_serialiser.h"

// =============================================================================
// Custom Pin Configuration for NeoPico-HD
// =============================================================================
// DVI Data:  GPIO 16-21 (3 differential pairs)
// DVI Clock: GPIO 26-27
//
// Wiring:
//   D0N/D0P -> GP16/GP17
//   D1N/D1P -> GP18/GP19
//   D2N/D2P -> GP20/GP21
//   CLKN/CLKP -> GP26/GP27

// Pico 2 custom wiring to Spotpear
// Spotpear GP8  (CLKN) <- Pico GP26
// Spotpear GP9  (CLKP) <- Pico GP27
// Spotpear GP10 (D0N)  <- Pico GP16
// Spotpear GP11 (D0P)  <- Pico GP17
// Spotpear GP12 (D1N)  <- Pico GP18
// Spotpear GP13 (D1P)  <- Pico GP19
// Spotpear GP14 (D2N)  <- Pico GP20
// Spotpear GP15 (D2P)  <- Pico GP21
static const struct dvi_serialiser_cfg neopico_dvi_cfg = {
    .pio = pio0,
    .sm_tmds = {0, 1, 2},
    .pins_tmds = {16, 18, 20},   // D0=GP16-17, D1=GP18-19, D2=GP20-21
    .pins_clk = 26,              // Clock=GP26-27
    .invert_diffpairs = true
};

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

static void generate_color_bar_line(uint16_t *buf, uint y) {
    (void)y;  // Same pattern for all lines
    for (uint x = 0; x < FRAME_WIDTH; x++) {
        uint bar_idx = x / BAR_WIDTH;
        if (bar_idx >= NUM_BARS) bar_idx = NUM_BARS - 1;
        buf[x] = color_bars[bar_idx];
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
    printf("DVI pins: Data GP16-21, Clock GP26-27\n");

    // Initialize DVI
    dvi0.timing = &DVI_TIMING;
    dvi0.ser_cfg = neopico_dvi_cfg;
    dvi_init(&dvi0, next_striped_spin_lock_num(), next_striped_spin_lock_num());

    // Launch DVI on core 1
    multicore_launch_core1(core1_main);

    printf("DVI initialized, outputting color bars...\n");

    // Pre-generate first scanline
    generate_color_bar_line(scanline_buf[0], 0);

    // Main loop - feed scanlines to DVI
    uint buf_idx = 0;
    while (true) {
        for (uint y = 0; y < FRAME_HEIGHT; ++y) {
            // Get pointer to current scanline buffer
            const uint16_t *scanline = scanline_buf[buf_idx];

            // Queue this scanline for display
            queue_add_blocking_u32(&dvi0.q_colour_valid, &scanline);

            // Switch to other buffer and generate next line
            buf_idx ^= 1;
            generate_color_bar_line(scanline_buf[buf_idx], (y + 1) % FRAME_HEIGHT);

            // Discard any returned buffers
            while (queue_try_remove_u32(&dvi0.q_colour_free, &scanline))
                ;
        }

        // Toggle LED each frame
        static uint frame_count = 0;
        if (++frame_count >= 60) {
            frame_count = 0;
            gpio_xor_mask(1u << PICO_DEFAULT_LED_PIN);
        }
    }

    return 0;
}
