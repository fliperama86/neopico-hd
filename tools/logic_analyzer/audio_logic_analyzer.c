/**
 * Audio Logic Analyzer for YM2610 Signal Debugging
 *
 * Captures BCK, DAT, WS signals at high speed and streams to USB.
 * Designed for debugging Neo Geo MVS audio capture.
 *
 * Default pin configuration (matches audio_config.h):
 *   GPIO 21: BCK (bit clock)
 *   GPIO 23: DAT (serial data)
 *   GPIO 24: WS (word select)
 *
 * Protocol:
 *   Host sends 'C' -> Pico captures and sends binary data
 *   Host sends 'A' -> Pico does frequency analysis
 *   Host sends 'R' -> Pico sends raw GPIO state
 *   Host sends 'H' -> Pico sends help
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "pico/stdlib.h"
#include "hardware/pio.h"
#include "hardware/dma.h"
#include "hardware/clocks.h"
#include "hardware/gpio.h"

#include "capture.pio.h"

// =============================================================================
// Configuration
// =============================================================================

// Audio pins (matching audio_config.h)
#define PIN_BCK  21
#define PIN_DAT  23
#define PIN_WS   24

// Capture settings
// NOTE: PIO uses 2 cycles per sample (in + push), so effective rate = target/2
#define SAMPLE_RATE_MHZ  10      // 10 MHz effective sample rate (need >5.3 MHz for 2.66 MHz BCK)
#define CAPTURE_BUFFER_SIZE (64 * 1024)  // 64KB buffer = 64K samples
#define CAPTURE_DURATION_MS 50   // ~50ms of capture time at 8MHz = 400K samples
                                 // But we're limited by buffer, so ~8ms actual

// PIO configuration
static PIO capture_pio = pio0;
static uint capture_sm = 0;
static uint capture_offset;
static int capture_dma_chan;

// Capture buffer (stores raw GPIO states)
static uint32_t capture_buffer[CAPTURE_BUFFER_SIZE / 4];

// =============================================================================
// Utility Functions
// =============================================================================

static void init_capture_pins(void) {
    // Initialize all three pins as inputs with pulls disabled
    gpio_init(PIN_BCK);
    gpio_init(PIN_DAT);
    gpio_init(PIN_WS);
    gpio_set_dir(PIN_BCK, GPIO_IN);
    gpio_set_dir(PIN_DAT, GPIO_IN);
    gpio_set_dir(PIN_WS, GPIO_IN);
    gpio_disable_pulls(PIN_BCK);
    gpio_disable_pulls(PIN_DAT);
    gpio_disable_pulls(PIN_WS);

    printf("Pins initialized: BCK=%d, DAT=%d, WS=%d\n", PIN_BCK, PIN_DAT, PIN_WS);
}

static void init_capture_pio(void) {
    // Calculate clock divider for desired sample rate
    // PIO loop is 2 cycles (in + push), so PIO clock must be 2x sample rate
    uint32_t sys_clk = clock_get_hz(clk_sys);
    float divider = (float)sys_clk / (SAMPLE_RATE_MHZ * 1000000 * 2);

    printf("System clock: %lu Hz\n", (unsigned long)sys_clk);
    printf("Effective sample rate: %d MHz\n", SAMPLE_RATE_MHZ);
    printf("PIO clock: %d MHz, divider: %.2f\n", SAMPLE_RATE_MHZ * 2, divider);

    // Load PIO program
    capture_offset = pio_add_program(capture_pio, &capture_program);
    capture_program_init(capture_pio, capture_sm, capture_offset, 0, divider);

    // PIO program samples all 32 GPIO pins, we'll extract bits 21, 23, 24 later
    printf("PIO capture initialized\n");
}

static void init_capture_dma(void) {
    capture_dma_chan = dma_claim_unused_channel(true);

    dma_channel_config c = dma_channel_get_default_config(capture_dma_chan);
    channel_config_set_transfer_data_size(&c, DMA_SIZE_32);
    channel_config_set_read_increment(&c, false);
    channel_config_set_write_increment(&c, true);
    channel_config_set_dreq(&c, pio_get_dreq(capture_pio, capture_sm, false));

    dma_channel_configure(
        capture_dma_chan,
        &c,
        capture_buffer,                    // Write to buffer
        &capture_pio->rxf[capture_sm],     // Read from PIO FIFO
        CAPTURE_BUFFER_SIZE / 4,           // Number of 32-bit transfers
        false                              // Don't start yet
    );

    printf("DMA channel %d configured\n", capture_dma_chan);
}

// =============================================================================
// Capture and Analysis
// =============================================================================

static void do_capture(void) {
    printf("\n=== Starting Capture ===\n");
    printf("Buffer: %d samples, Rate: %d MHz\n",
           CAPTURE_BUFFER_SIZE / 4, SAMPLE_RATE_MHZ);
    printf("Duration: ~%d ms\n", (CAPTURE_BUFFER_SIZE / 4) / (SAMPLE_RATE_MHZ * 1000));

    // Clear PIO FIFO
    pio_sm_set_enabled(capture_pio, capture_sm, false);
    pio_sm_clear_fifos(capture_pio, capture_sm);
    pio_sm_restart(capture_pio, capture_sm);
    pio_sm_exec(capture_pio, capture_sm, pio_encode_jmp(capture_offset));

    // Reset DMA
    dma_channel_set_write_addr(capture_dma_chan, capture_buffer, false);
    dma_channel_set_trans_count(capture_dma_chan, CAPTURE_BUFFER_SIZE / 4, false);

    // Start capture
    pio_sm_set_enabled(capture_pio, capture_sm, true);
    dma_channel_start(capture_dma_chan);

    // Wait for completion
    dma_channel_wait_for_finish_blocking(capture_dma_chan);
    pio_sm_set_enabled(capture_pio, capture_sm, false);

    printf("Capture complete!\n");
}

static void analyze_capture(void) {
    printf("\n=== Signal Analysis ===\n");

    uint32_t num_samples = CAPTURE_BUFFER_SIZE / 4;

    // Count transitions for each signal
    uint32_t bck_transitions = 0;
    uint32_t dat_transitions = 0;
    uint32_t ws_transitions = 0;

    // Track last state
    bool last_bck = (capture_buffer[0] >> PIN_BCK) & 1;
    bool last_dat = (capture_buffer[0] >> PIN_DAT) & 1;
    bool last_ws = (capture_buffer[0] >> PIN_WS) & 1;

    // Count high samples (for duty cycle)
    uint32_t bck_high = 0;
    uint32_t dat_high = 0;
    uint32_t ws_high = 0;

    for (uint32_t i = 0; i < num_samples; i++) {
        uint32_t sample = capture_buffer[i];
        bool bck = (sample >> PIN_BCK) & 1;
        bool dat = (sample >> PIN_DAT) & 1;
        bool ws = (sample >> PIN_WS) & 1;

        if (bck != last_bck) { bck_transitions++; last_bck = bck; }
        if (dat != last_dat) { dat_transitions++; last_dat = dat; }
        if (ws != last_ws) { ws_transitions++; last_ws = ws; }

        if (bck) bck_high++;
        if (dat) dat_high++;
        if (ws) ws_high++;
    }

    // Calculate frequencies (transitions/2 = cycles, divide by time)
    float capture_time_s = (float)num_samples / (SAMPLE_RATE_MHZ * 1000000);
    float bck_freq = (bck_transitions / 2.0f) / capture_time_s;
    float dat_freq = (dat_transitions / 2.0f) / capture_time_s;
    float ws_freq = (ws_transitions / 2.0f) / capture_time_s;

    printf("Capture: %lu samples over %.3f ms\n",
           (unsigned long)num_samples, capture_time_s * 1000);
    printf("\nSignal      Transitions  Frequency    Duty Cycle\n");
    printf("----------- ------------ ------------ ----------\n");
    printf("BCK (GP%d)  %10lu  %8.1f kHz  %5.1f%%\n",
           PIN_BCK, (unsigned long)bck_transitions, bck_freq / 1000,
           100.0f * bck_high / num_samples);
    printf("DAT (GP%d)  %10lu  %8.1f kHz  %5.1f%%\n",
           PIN_DAT, (unsigned long)dat_transitions, dat_freq / 1000,
           100.0f * dat_high / num_samples);
    printf("WS  (GP%d)  %10lu  %8.1f kHz  %5.1f%%\n",
           PIN_WS, (unsigned long)ws_transitions, ws_freq / 1000,
           100.0f * ws_high / num_samples);

    // Expected values for YM2610
    printf("\n=== Expected vs Actual ===\n");
    printf("BCK expected: ~1776 kHz (55.5 kHz * 32)\n");
    printf("BCK actual:   %.1f kHz\n", bck_freq / 1000);
    printf("WS expected:  ~55.5 kHz\n");
    printf("WS actual:    %.1f kHz\n", ws_freq / 1000);

    // Warnings
    if (bck_transitions < 1000) {
        printf("\n** WARNING: BCK signal appears dead or very slow! **\n");
        printf("   Check GPIO %d wiring to YM2610 pin 5 (oS/BCK)\n", PIN_BCK);
    }
    if (ws_transitions < 100) {
        printf("\n** WARNING: WS signal appears dead or very slow! **\n");
        printf("   Check GPIO %d wiring to YM3016 pin 3 (SH1)\n", PIN_WS);
    }
}

static void stream_binary_capture(void) {
    // Stream format:
    // "LOGA" - magic
    // uint32_t sample_rate_hz
    // uint32_t num_samples
    // uint8_t pin_bck, pin_dat, pin_ws, reserved
    // uint8_t data[num_samples] - packed: bit0=BCK, bit1=DAT, bit2=WS

    printf("\n=== Binary Stream Mode ===\n");
    printf("Sending %lu samples as binary...\n", (unsigned long)(CAPTURE_BUFFER_SIZE / 4));

    // Header
    putchar('L'); putchar('O'); putchar('G'); putchar('A');

    uint32_t sample_rate_hz = SAMPLE_RATE_MHZ * 1000000;
    uint32_t num_samples = CAPTURE_BUFFER_SIZE / 4;

    // Write as bytes (little endian)
    putchar(sample_rate_hz & 0xFF);
    putchar((sample_rate_hz >> 8) & 0xFF);
    putchar((sample_rate_hz >> 16) & 0xFF);
    putchar((sample_rate_hz >> 24) & 0xFF);

    putchar(num_samples & 0xFF);
    putchar((num_samples >> 8) & 0xFF);
    putchar((num_samples >> 16) & 0xFF);
    putchar((num_samples >> 24) & 0xFF);

    putchar(PIN_BCK);
    putchar(PIN_DAT);
    putchar(PIN_WS);
    putchar(0);  // reserved

    // Pack and send data - extract bits 21, 23, 24 into bits 0, 1, 2
    for (uint32_t i = 0; i < num_samples; i++) {
        uint32_t sample = capture_buffer[i];
        uint8_t packed = 0;
        if ((sample >> PIN_BCK) & 1) packed |= 0x01;
        if ((sample >> PIN_DAT) & 1) packed |= 0x02;
        if ((sample >> PIN_WS) & 1) packed |= 0x04;
        putchar(packed);
    }

    printf("\nBinary stream complete.\n");
}

static void show_raw_state(void) {
    printf("\n=== Raw GPIO State ===\n");
    printf("Reading 100 samples at ~100Hz...\n\n");

    printf("Sample  BCK(GP%d)  DAT(GP%d)  WS(GP%d)\n", PIN_BCK, PIN_DAT, PIN_WS);
    printf("------  --------  --------  -------\n");

    for (int i = 0; i < 100; i++) {
        bool bck = gpio_get(PIN_BCK);
        bool dat = gpio_get(PIN_DAT);
        bool ws = gpio_get(PIN_WS);

        printf("%5d     %d         %d         %d\n", i, bck, dat, ws);
        sleep_ms(10);
    }
}

static void show_timing_pattern(void) {
    printf("\n=== Timing Pattern (first 256 samples) ===\n");
    printf("Sample: BCK DAT WS\n");

    for (int i = 0; i < 256 && i < (CAPTURE_BUFFER_SIZE / 4); i++) {
        uint32_t sample = capture_buffer[i];
        bool bck = (sample >> PIN_BCK) & 1;
        bool dat = (sample >> PIN_DAT) & 1;
        bool ws = (sample >> PIN_WS) & 1;

        printf("%4d:    %d   %d   %d", i, bck, dat, ws);

        // Mark edges
        if (i > 0) {
            uint32_t prev = capture_buffer[i-1];
            bool prev_bck = (prev >> PIN_BCK) & 1;
            bool prev_ws = (prev >> PIN_WS) & 1;

            if (bck && !prev_bck) printf("  <- BCK rising");
            if (!bck && prev_bck) printf("  <- BCK falling");
            if (ws && !prev_ws) printf("  <- WS rising");
            if (!ws && prev_ws) printf("  <- WS falling");
        }
        printf("\n");
    }
}

static void show_help(void) {
    printf("\n");
    printf("========================================\n");
    printf("  YM2610 Audio Logic Analyzer\n");
    printf("========================================\n");
    printf("\n");
    printf("Pin Configuration:\n");
    printf("  GPIO %d: BCK (bit clock, YM2610 pin 5)\n", PIN_BCK);
    printf("  GPIO %d: DAT (serial data, YM2610 pin 8)\n", PIN_DAT);
    printf("  GPIO %d: WS  (word select, YM3016 pin 3)\n", PIN_WS);
    printf("\n");
    printf("Commands:\n");
    printf("  C - Capture to buffer and analyze\n");
    printf("  A - Analyze captured data\n");
    printf("  T - Show timing pattern (first 256 samples)\n");
    printf("  B - Stream binary data (for Python viewer)\n");
    printf("  R - Show raw GPIO state (slow polling)\n");
    printf("  H - Show this help\n");
    printf("\n");
    printf("Expected YM2610 signals:\n");
    printf("  BCK: ~1776 kHz (55.5 kHz sample rate * 32 bits)\n");
    printf("  WS:  ~55.5 kHz (toggles each audio sample)\n");
    printf("  DAT: varies with audio content\n");
    printf("\n");
}

// =============================================================================
// Main
// =============================================================================

int main() {
    stdio_init_all();

    // Wait for USB connection
    sleep_ms(2000);

    printf("\n\n");
    show_help();

    // Initialize hardware
    init_capture_pins();
    init_capture_pio();
    init_capture_dma();

    printf("\nReady! Send command (C/A/T/B/R/H)...\n");

    // Initial capture
    do_capture();
    analyze_capture();

    while (true) {
        int c = getchar_timeout_us(100000);  // 100ms timeout
        if (c != PICO_ERROR_TIMEOUT) {
            switch (c) {
                case 'C':
                case 'c':
                    do_capture();
                    analyze_capture();
                    break;
                case 'A':
                case 'a':
                    analyze_capture();
                    break;
                case 'T':
                case 't':
                    show_timing_pattern();
                    break;
                case 'B':
                case 'b':
                    stream_binary_capture();
                    break;
                case 'R':
                case 'r':
                    show_raw_state();
                    break;
                case 'H':
                case 'h':
                case '?':
                    show_help();
                    break;
                default:
                    printf("Unknown command '%c'. Send 'H' for help.\n", c);
            }
            printf("\nReady! Send command (C/A/T/B/R/H)...\n");
        }
    }

    return 0;
}
