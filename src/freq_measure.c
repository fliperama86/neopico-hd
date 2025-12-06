/**
 * Audio Pin Frequency Measurement (PIO Version)
 *
 * Measures the frequency of YM2610 audio signals using PIO edge counters.
 * Uses ANSI escape codes for in-place display updates.
 */

#include <stdio.h>
#include "pico/stdlib.h"
#include "hardware/pio.h"
#include "hardware/clocks.h"
#include "freq_measure.pio.h"

// Audio pin definitions (all must be GPIO 0-29 for PIO0)
#define PIN_BCK  23
#define PIN_DAT  3
#define PIN_WS   9

// Measurement period in milliseconds
#define MEASURE_PERIOD_MS  1000

// PIO and state machine assignments (all on PIO0)
static PIO pio;
static uint sm_bck;
static uint sm_dat;
static uint sm_ws;
static uint pio_offset;

// Double-buffered results (written by measurement, read by display)
static volatile uint32_t result_bck = 0;
static volatile uint32_t result_dat = 0;
static volatile uint32_t result_ws = 0;
static volatile uint32_t result_iteration = 0;
static volatile bool result_ready = false;

// Format frequency to string (avoids printf in tight loop)
static void format_freq(char *buf, size_t len, uint32_t edges, uint32_t period_ms) {
    if (edges == 0) {
        snprintf(buf, len, "No signal");
        return;
    }

    float freq = (float)edges / (period_ms / 1000.0f);

    if (freq >= 1000000) {
        snprintf(buf, len, "%7.3f MHz", freq / 1000000.0f);
    } else if (freq >= 1000) {
        snprintf(buf, len, "%7.3f kHz", freq / 1000.0f);
    } else {
        snprintf(buf, len, "%7.2f Hz ", freq);
    }
}

int main() {
    stdio_init_all();

    // Initialize LED
    gpio_init(PICO_DEFAULT_LED_PIN);
    gpio_set_dir(PICO_DEFAULT_LED_PIN, GPIO_OUT);

    // Wait for USB connection with LED blink
    for (int i = 0; i < 30; i++) {
        gpio_put(PICO_DEFAULT_LED_PIN, i & 1);
        sleep_ms(100);
    }
    gpio_put(PICO_DEFAULT_LED_PIN, 1);

    // Setup PIO0 for all pins
    pio = pio0;
    pio_offset = pio_add_program(pio, &edge_counter_program);

    sm_bck = pio_claim_unused_sm(pio, true);
    sm_dat = pio_claim_unused_sm(pio, true);
    sm_ws = pio_claim_unused_sm(pio, true);

    // BCK is fast (2.7 MHz) - use 10 MHz PIO clock
    // DAT varies - use 10 MHz
    // WS is slow (55 kHz) - use 500 kHz to filter BCK crosstalk
    edge_counter_program_init_freq(pio, sm_bck, pio_offset, PIN_BCK, 10000000.0f);
    edge_counter_program_init_freq(pio, sm_dat, pio_offset, PIN_DAT, 10000000.0f);
    edge_counter_program_init_freq(pio, sm_ws, pio_offset, PIN_WS, 500000.0f);

    uint32_t sys_clk = clock_get_hz(clk_sys);

    // Print header (stays fixed)
    printf("\033[2J\033[H");  // Clear screen, home cursor
    printf("========================================\n");
    printf("  YM2610 Audio Pin Frequency Meter\n");
    printf("========================================\n");
    printf("System: %.1f MHz | Period: %d ms\n", sys_clk / 1000000.0f, MEASURE_PERIOD_MS);
    printf("----------------------------------------\n");
    printf("Expected: BCK ~2.67 MHz, WS ~55.5 kHz\n");
    printf("          BCK/WS ratio ~48 bits/sample\n");
    printf("----------------------------------------\n");
    printf("\n");  // Line 9: BCK
    printf("\n");  // Line 10: DAT
    printf("\n");  // Line 11: WS
    printf("\n");  // Line 12: blank
    printf("\n");  // Line 13: Analysis header
    printf("\n");  // Line 14: BCK/WS ratio
    printf("\n");  // Line 15: Sample rate
    printf("\n");  // Line 16: blank
    printf("\n");  // Line 17: Pin states
    printf("----------------------------------------\n");

    stdio_flush();

    char buf_bck[32], buf_dat[32], buf_ws[32];
    uint32_t iteration = 0;

    while (true) {
        // Reset counters
        edge_counter_reset(pio, sm_bck);
        edge_counter_reset(pio, sm_dat);
        edge_counter_reset(pio, sm_ws);

        // LED on during measurement
        gpio_put(PICO_DEFAULT_LED_PIN, 1);

        // Wait for measurement period
        sleep_ms(MEASURE_PERIOD_MS);

        // Read counts (PIO is independent, this is quick)
        uint32_t bck = edge_counter_get_count(pio, sm_bck);
        uint32_t dat = edge_counter_get_count(pio, sm_dat);
        uint32_t ws = edge_counter_get_count(pio, sm_ws);

        gpio_put(PICO_DEFAULT_LED_PIN, 0);

        iteration++;

        // Format strings
        format_freq(buf_bck, sizeof(buf_bck), bck, MEASURE_PERIOD_MS);
        format_freq(buf_dat, sizeof(buf_dat), dat, MEASURE_PERIOD_MS);
        format_freq(buf_ws, sizeof(buf_ws), ws, MEASURE_PERIOD_MS);

        // Calculate analysis
        float bck_freq = (float)bck / (MEASURE_PERIOD_MS / 1000.0f);
        float ws_freq = (float)ws / (MEASURE_PERIOD_MS / 1000.0f);
        float ratio = (ws > 0) ? bck_freq / ws_freq : 0;

        // Update display in place using ANSI escape codes
        printf("\033[9;1H");  // Move to line 9
        printf("  BCK (GPIO%d): %s  (%lu)      \n", PIN_BCK, buf_bck, bck);
        printf("  DAT (GPIO%d): %s  (%lu)      \n", PIN_DAT, buf_dat, dat);
        printf("  WS  (GPIO%d): %s  (%lu)      \n", PIN_WS, buf_ws, ws);
        printf("\n");
        printf("  Analysis:                           \n");
        if (ws > 0) {
            printf("    BCK/WS: %.1f bits/sample          \n", ratio);
            printf("    Sample: %.2f kHz                  \n", ws_freq / 1000.0f);
        } else {
            printf("    BCK/WS: --                        \n");
            printf("    Sample: --                        \n");
        }
        printf("\n");
        printf("  Pins: BCK=%d DAT=%d WS=%d  [#%lu]    \n",
               gpio_get(PIN_BCK), gpio_get(PIN_DAT), gpio_get(PIN_WS), iteration);

        stdio_flush();
    }

    return 0;
}
