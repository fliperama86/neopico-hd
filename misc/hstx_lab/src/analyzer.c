// High-speed HSTX signal analyzer for RP2350
// Based on logicanalyzer project - overclocks to 400MHz for blast capture
// Captures raw bits from GPIO pins at maximum speed

#include <stdio.h>
#include <string.h>
#include "pico/stdlib.h"
#include "hardware/pio.h"
#include "hardware/dma.h"
#include "hardware/clocks.h"
#include "hardware/vreg.h"
#include "hardware/structs/bus_ctrl.h"

#include "blast_capture.pio.h"

// Capture settings - matches HSTX output pins on generator
#define CAPTURE_PIN_BASE    12      // Start capturing from GP12
#define CAPTURE_PIN_COUNT   8       // Capture 8 pins (GP12-GP19)
#define CAPTURE_BUFFER_SIZE (384 * 1024)  // 384KB capture buffer (max for RP2350)
#define LED_PIN 25

// Capture buffer - aligned for DMA
static uint8_t capture_buffer[CAPTURE_BUFFER_SIZE] __attribute__((aligned(4)));

// DMA channel
static int dma_chan;

void init_overclock(void) {
    // Disable voltage limit and set higher voltage for stable overclock
    vreg_disable_voltage_limit();
    vreg_set_voltage(VREG_VOLTAGE_1_30);  // Higher voltage for 400MHz
    sleep_ms(10);

    // TURBO MODE: Overclock to 400MHz for maximum capture speed
    set_sys_clock_khz(400000, true);
}

void init_capture(PIO pio, uint sm, uint offset) {
    // Initialize the PIO program
    blast_capture_program_init(pio, sm, offset, CAPTURE_PIN_BASE, CAPTURE_PIN_COUNT);

    // Configure DMA
    dma_chan = dma_claim_unused_channel(true);
    dma_channel_config c = dma_channel_get_default_config(dma_chan);

    channel_config_set_read_increment(&c, false);   // Always read from PIO FIFO
    channel_config_set_write_increment(&c, true);   // Increment write address
    channel_config_set_transfer_data_size(&c, DMA_SIZE_32);  // 32-bit transfers
    channel_config_set_dreq(&c, pio_get_dreq(pio, sm, false));  // Pace by PIO RX FIFO

    // Give DMA highest priority
    bus_ctrl_hw->priority = BUSCTRL_BUS_PRIORITY_DMA_W_BITS | BUSCTRL_BUS_PRIORITY_DMA_R_BITS;

    dma_channel_configure(
        dma_chan,
        &c,
        capture_buffer,         // Write to capture buffer
        &pio->rxf[sm],          // Read from PIO RX FIFO
        CAPTURE_BUFFER_SIZE / 4, // Number of 32-bit transfers
        false                   // Don't start yet
    );
}

void run_capture(PIO pio, uint sm) {
    printf("Starting capture...\n");

    // Clear buffer
    memset(capture_buffer, 0, CAPTURE_BUFFER_SIZE);

    // Reset DMA
    dma_channel_set_write_addr(dma_chan, capture_buffer, false);
    dma_channel_set_trans_count(dma_chan, CAPTURE_BUFFER_SIZE / 4, false);

    // Clear PIO FIFO
    pio_sm_clear_fifos(pio, sm);

    // Start DMA first, then PIO
    dma_channel_start(dma_chan);
    blast_capture_start(pio, sm);

    // Wait for DMA to complete
    dma_channel_wait_for_finish_blocking(dma_chan);

    // Stop capture
    blast_capture_stop(pio, sm);

    printf("Capture complete!\n");
}

// Small capture for comparison - 8KB is enough to see data islands
#define SMALL_CAPTURE_SIZE 8192

void run_small_capture(PIO pio, uint sm) {
    printf("Small capture (%d bytes)...\n", SMALL_CAPTURE_SIZE);

    memset(capture_buffer, 0, SMALL_CAPTURE_SIZE);

    dma_channel_set_write_addr(dma_chan, capture_buffer, false);
    dma_channel_set_trans_count(dma_chan, SMALL_CAPTURE_SIZE / 4, false);

    pio_sm_clear_fifos(pio, sm);
    dma_channel_start(dma_chan);
    blast_capture_start(pio, sm);

    dma_channel_wait_for_finish_blocking(dma_chan);
    blast_capture_stop(pio, sm);

    printf("Done!\n");
}

// Triggered capture - wait for vsync-like pattern then capture
// Vsync active = D0 lane shows TMDS_CTRL_00 pattern (both sync bits low)
// We detect this by looking for a stable period on clock (GP12/13)
void run_triggered_capture(PIO pio, uint sm) {
    printf("Waiting for vsync edge...\n");

    // Pin masks for clock differential pair (GP12=CK-, GP13=CK+)
    const uint32_t ck_mask = (1 << (12 - CAPTURE_PIN_BASE)) | (1 << (13 - CAPTURE_PIN_BASE));

    // Wait for clock activity to stabilize (look for consistent toggling)
    // This happens during blanking periods
    volatile uint8_t *gpio_in = (volatile uint8_t *)(0xd0000004 + CAPTURE_PIN_BASE / 8);

    // First, wait for a period where D0 (GP14/15) shows low activity
    // During vsync, the D0 lane has specific TMDS control symbols
    uint32_t stable_count = 0;
    uint8_t last_val = 0;

    // Wait up to 100ms for trigger
    uint32_t timeout = time_us_32() + 100000;

    while (time_us_32() < timeout) {
        uint8_t val = (gpio_in[0] >> (14 - CAPTURE_PIN_BASE)) & 0x03;  // D0 differential
        if (val == last_val) {
            stable_count++;
            // If D0 is stable for ~1000 samples, we're in blanking
            if (stable_count > 1000) {
                printf("Triggered! Starting capture...\n");
                break;
            }
        } else {
            stable_count = 0;
        }
        last_val = val;
    }

    if (stable_count <= 1000) {
        printf("Timeout waiting for trigger\n");
        return;
    }

    // Now do a small capture
    memset(capture_buffer, 0, SMALL_CAPTURE_SIZE);
    dma_channel_set_write_addr(dma_chan, capture_buffer, false);
    dma_channel_set_trans_count(dma_chan, SMALL_CAPTURE_SIZE / 4, false);

    pio_sm_clear_fifos(pio, sm);
    dma_channel_start(dma_chan);
    blast_capture_start(pio, sm);

    dma_channel_wait_for_finish_blocking(dma_chan);
    blast_capture_stop(pio, sm);

    printf("Triggered capture complete!\n");
}

// Dump in a format easy to diff - show differential values for each lane
void print_lanes_diff(uint32_t offset, uint32_t length) {
    printf("\n=== Lane differential values (CK, D0, D1, D2) ===\n");
    printf("Format: Each value is XOR of +/- pins for that lane\n\n");

    // Pin mapping: GP12=CK-, GP13=CK+, GP14=D0-, GP15=D0+, etc.
    // Bit positions in capture byte (relative to GP12):
    // Bit 0 = GP12 (CK-), Bit 1 = GP13 (CK+)
    // Bit 2 = GP14 (D0-), Bit 3 = GP15 (D0+)
    // Bit 4 = GP16 (D1-), Bit 5 = GP17 (D1+)
    // Bit 6 = GP18 (D2-), Bit 7 = GP19 (D2+)

    for (uint32_t i = offset; i < offset + length && i < SMALL_CAPTURE_SIZE; i++) {
        uint8_t b = capture_buffer[i];

        // Extract differential values (XOR of +/- for each pair)
        int ck = ((b >> 0) & 1) ^ ((b >> 1) & 1);
        int d0 = ((b >> 2) & 1) ^ ((b >> 3) & 1);
        int d1 = ((b >> 4) & 1) ^ ((b >> 5) & 1);
        int d2 = ((b >> 6) & 1) ^ ((b >> 7) & 1);

        printf("%d%d%d%d", ck, d0, d1, d2);

        if ((i - offset + 1) % 40 == 0) printf("\n");
    }
    printf("\n");
}

void print_capture_hex(uint32_t offset, uint32_t length) {
    printf("\n=== Captured data (offset %lu, %lu bytes) ===\n", offset, length);

    for (uint32_t i = 0; i < length; i++) {
        if (i % 32 == 0) {
            printf("\n%08lX: ", offset + i);
        }
        printf("%02X ", capture_buffer[offset + i]);
    }
    printf("\n");
}

void print_capture_binary(uint32_t offset, uint32_t length, uint8_t pin_mask) {
    printf("\n=== Captured bits for pins masked 0x%02X ===\n", pin_mask);

    for (uint32_t i = 0; i < length; i++) {
        uint8_t byte = capture_buffer[offset + i] & pin_mask;

        // Print each bit
        for (int bit = 7; bit >= 0; bit--) {
            if (pin_mask & (1 << bit)) {
                printf("%c", (byte & (1 << bit)) ? '1' : '0');
            }
        }

        if ((i + 1) % 8 == 0) printf("\n");
        else printf(" ");
    }
    printf("\n");
}

void analyze_transitions(uint8_t pin) {
    printf("\n=== Transition analysis for pin %d ===\n", pin);

    uint8_t mask = 1 << pin;
    uint8_t last_state = capture_buffer[0] & mask;
    uint32_t transitions = 0;
    uint32_t min_period = 0xFFFFFFFF;
    uint32_t max_period = 0;
    uint32_t last_transition = 0;
    uint32_t total_periods = 0;
    uint32_t period_sum = 0;

    for (uint32_t i = 1; i < CAPTURE_BUFFER_SIZE; i++) {
        uint8_t state = capture_buffer[i] & mask;
        if (state != last_state) {
            transitions++;

            if (last_transition > 0) {
                uint32_t period = i - last_transition;
                if (period < min_period) min_period = period;
                if (period > max_period) max_period = period;
                period_sum += period;
                total_periods++;
            }

            last_transition = i;
            last_state = state;
        }
    }

    printf("Total transitions: %lu\n", transitions);

    if (total_periods > 0) {
        float avg_period = (float)period_sum / total_periods;
        float sys_clk = (float)clock_get_hz(clk_sys);
        float freq = sys_clk / (avg_period * 2);  // *2 because period is half-cycle

        printf("Min half-period: %lu samples (%.1f ns)\n", min_period, min_period * 1000000000.0f / sys_clk);
        printf("Max half-period: %lu samples (%.1f ns)\n", max_period, max_period * 1000000000.0f / sys_clk);
        printf("Avg half-period: %.1f samples\n", avg_period);
        printf("Estimated frequency: %.2f MHz\n", freq / 1000000.0f);
    }
}

int main() {
    // Initialize overclock BEFORE stdio
    init_overclock();

    stdio_init_all();

    gpio_init(LED_PIN);
    gpio_set_dir(LED_PIN, GPIO_OUT);

    // Wait for USB connection
    sleep_ms(2000);

    uint32_t sys_clk = clock_get_hz(clk_sys);
    printf("\n\n========================================\n");
    printf("HSTX Signal Analyzer\n");
    printf("System clock: %lu MHz\n", sys_clk / 1000000);
    printf("Sample rate: %lu Msps\n", sys_clk / 1000000);
    printf("Buffer size: %d bytes\n", CAPTURE_BUFFER_SIZE);
    printf("Capture pins: GP%d-GP%d\n", CAPTURE_PIN_BASE, CAPTURE_PIN_BASE + CAPTURE_PIN_COUNT - 1);
    printf("========================================\n\n");

    printf("Connect HSTX output pin to GP0-GP7 for capture.\n");
    printf("WARNING: HSTX outputs at 3.3V LVCMOS levels.\n");
    printf("         Do NOT connect differential pairs directly!\n\n");

    PIO pio = pio0;
    uint sm = pio_claim_unused_sm(pio, true);
    uint offset = pio_add_program(pio, &blast_capture_program);

    init_capture(pio, sm, offset);

    printf("Commands:\n");
    printf("  c - Full capture (384KB)\n");
    printf("  s - Small capture (8KB)\n");
    printf("  t - Triggered capture (wait for blanking)\n");
    printf("  l - Show lane differentials (first 400 samples)\n");
    printf("  h - Show hex dump (first 256 bytes)\n");
    printf("  d - Dump full raw buffer (binary)\n");
    printf("  a - Analyze transitions (pin 0)\n");
    printf("  0-7 - Analyze specific pin\n");
    printf("\n");

    bool led_state = false;
    uint32_t last_blink = 0;

    while (true) {
        // Blink LED
        uint32_t now = time_us_32();
        if (now - last_blink > 500000) {
            led_state = !led_state;
            gpio_put(LED_PIN, led_state);
            last_blink = now;
        }

        int c = getchar_timeout_us(0);
        if (c != PICO_ERROR_TIMEOUT) {
            switch (c) {
                case 'c':
                case 'C':
                    run_capture(pio, sm);
                    break;

                case 's':
                case 'S':
                    run_small_capture(pio, sm);
                    break;

                case 't':
                case 'T':
                    run_triggered_capture(pio, sm);
                    break;

                case 'l':
                case 'L':
                    print_lanes_diff(0, 400);
                    break;

                case 'd':
                case 'D':
                    // Dump full raw buffer as binary
                    printf("DUMP_START:%d\n", CAPTURE_BUFFER_SIZE);
                    for (uint32_t i = 0; i < CAPTURE_BUFFER_SIZE; i++) {
                        putchar(capture_buffer[i]);
                    }
                    printf("\nDUMP_END\n");
                    break;

                case 'h':
                case 'H':
                    print_capture_hex(0, 256);
                    break;

                case 'a':
                case 'A':
                    analyze_transitions(0);
                    break;

                case '0': case '1': case '2': case '3':
                case '4': case '5': case '6': case '7':
                    analyze_transitions(c - '0');
                    break;

                case '\r':
                case '\n':
                    break;

                default:
                    printf("Unknown command: %c\n", c);
                    break;
            }
        }
    }

    return 0;
}
