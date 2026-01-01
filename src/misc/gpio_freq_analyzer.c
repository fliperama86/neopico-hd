/**
 * GPIO Frequency Analyzer - PIO-based
 *
 * Uses PIO to count edges at full system clock speed.
 * Can accurately measure MHz-level signals like I2S BCK.
 */

#include <stdio.h>
#include "pico/stdlib.h"
#include "hardware/gpio.h"
#include "hardware/pio.h"
#include "hardware/clocks.h"

// PIO program to count rising edges
// Each rising edge decrements X (counting down from 0xFFFFFFFF)
// Simple 3-instruction loop for maximum speed
#define pio_edge_counter_wrap_target 0
#define pio_edge_counter_wrap 2

// ORIGINAL (doesn't work on RP2350B with GPIO 32+):
// static const uint16_t pio_edge_counter_program_instructions[] = {
//     0x2020, //  0: wait   0 pin, 0        ; Wait for pin low
//     0x20a0, //  1: wait   1 pin, 0        ; Wait for pin high (rising edge)
//     0x0040, //  2: jmp    x--, 0          ; Decrement X, loop back
// };

// TEST: Just decrement X as fast as possible (no wait)
// This tests if PIO execution works at all
static const uint16_t pio_edge_counter_program_instructions[] = {
    //     .wrap_target
    0x0040, //  0: jmp    x--, 0          ; Decrement X, loop back
    0xa022, //  1: nop                     ; Padding for timing
    0xa022, //  2: nop                     ; Padding for timing
    //     .wrap
};

static const struct pio_program pio_edge_counter_program = {
    .instructions = pio_edge_counter_program_instructions,
    .length = 3,
    .origin = -1,
};

// Video pins to monitor
#define PIN_PCLK   45   // Pixel clock
#define PIN_CSYNC  46   // Composite sync
#define PIN_R0     42   // Red LSB
#define PIN_G0     36   // Green LSB
#define PIN_B0     35   // Blue LSB
#define PIN_R4     46   // Red MSB (shared with CSYNC)

// We'll use multiple PIOs since we need 6 SMs (each PIO has only 4)
static PIO pio0_inst = pio0;
static PIO pio1_inst = pio1;
static uint sm_pclk, sm_csync, sm_r0, sm_g0, sm_b0, sm_r4;
static uint program_offset_pio0, program_offset_pio1;

static void init_edge_counter(PIO pio, uint sm, uint pin, uint offset) {
    pio_sm_config c = pio_get_default_sm_config();

    sm_config_set_wrap(&c, offset + pio_edge_counter_wrap_target,
                          offset + pio_edge_counter_wrap);

    // Set input pin
    sm_config_set_in_pins(&c, pin);
    sm_config_set_jmp_pin(&c, pin);

    // Initialize GPIO for input first (important for Bank 1 GPIOs on RP2350B)
    gpio_init(pin);
    gpio_set_dir(pin, GPIO_IN);
    gpio_pull_down(pin);  // Enable pull-down to reduce noise

    // Configure the pin for PIO use
    pio_gpio_init(pio, pin);
    pio_sm_set_consecutive_pindirs(pio, sm, pin, 1, false);

    // Run at full system clock for maximum resolution
    sm_config_set_clkdiv(&c, 1.0f);

    pio_sm_init(pio, sm, offset, &c);
}

// Last read values for delta calculation (indexed by PIO*4 + SM)
static uint32_t last_x_value[8] = {0};

static uint32_t read_edge_count(PIO pio, uint sm, const char *label, bool debug) {
    // Read X register by executing "mov isr, x" then "push"
    // X decrements on each edge, wrapping around
    uint idx = (pio == pio0_inst ? 0 : 4) + sm;

    // Stop SM briefly to read X
    pio_sm_set_enabled(pio, sm, false);

    // Execute: mov isr, x
    pio_sm_exec(pio, sm, pio_encode_mov(pio_isr, pio_x));
    // Execute: push noblock
    pio_sm_exec(pio, sm, pio_encode_push(false, false));

    // Read the value
    uint32_t x_value = 0;
    if (!pio_sm_is_rx_fifo_empty(pio, sm)) {
        x_value = pio_sm_get(pio, sm);
    } else if (debug) {
        printf("[%s: FIFO empty!] ", label);
    }

    // Clear any FIFO contents
    while (!pio_sm_is_rx_fifo_empty(pio, sm)) {
        pio_sm_get(pio, sm);
    }

    // Restart SM
    pio_sm_set_enabled(pio, sm, true);

    // Calculate delta (X counts DOWN, so last - current = edges)
    uint32_t count = last_x_value[idx] - x_value;
    last_x_value[idx] = x_value;

    if (debug && count != 0) {
        printf("[%s: X=0x%08lx, delta=%lu] ", label, x_value, count);
    }

    return count;
}

static void start_counters(void) {
    // Initialize X to 0 on all SMs (will wrap on first decrement)
    // PIO0 SMs
    pio_sm_exec(pio0_inst, sm_pclk, pio_encode_set(pio_x, 0));
    pio_sm_exec(pio0_inst, sm_csync, pio_encode_set(pio_x, 0));
    pio_sm_exec(pio0_inst, sm_r0, pio_encode_set(pio_x, 0));
    pio_sm_exec(pio0_inst, sm_g0, pio_encode_set(pio_x, 0));
    // PIO1 SMs
    pio_sm_exec(pio1_inst, sm_b0, pio_encode_set(pio_x, 0));
    pio_sm_exec(pio1_inst, sm_r4, pio_encode_set(pio_x, 0));

    // Start all SMs simultaneously
    pio_enable_sm_mask_in_sync(pio0_inst, (1u << sm_pclk) | (1u << sm_csync) |
                                           (1u << sm_r0) | (1u << sm_g0));
    pio_enable_sm_mask_in_sync(pio1_inst, (1u << sm_b0) | (1u << sm_r4));
}

static void print_freq(const char *label, float freq) {
    printf("%s: ", label);
    if (freq < 100) printf("%6.1f Hz  ", freq);
    else if (freq < 100000) printf("%6.2f kHz", freq / 1000);
    else printf("%6.3f MHz", freq / 1000000);
}

int main() {
    stdio_init_all();
    sleep_ms(2000);

    printf("\n");
    printf("=============================================\n");
    printf("  MVS Video Pin Analyzer (PIO-based)\n");
    printf("=============================================\n");
    printf("System clock: %lu MHz\n", clock_get_hz(clk_sys) / 1000000);
    printf("\n");
    printf("Monitoring MVS video pins:\n");
    printf("  GP%d = PCLK  (Pixel Clock)\n", PIN_PCLK);
    printf("  GP%d = CSYNC (Composite Sync)\n", PIN_CSYNC);
    printf("  GP%d = R0    (Red LSB)\n", PIN_R0);
    printf("  GP%d = G0    (Green LSB)\n", PIN_G0);
    printf("  GP%d = B0    (Blue LSB)\n", PIN_B0);
    printf("\n");

    // Note: On RP2350B, GPIO 32+ (Bank 1) can be accessed directly
    // Split across PIO0 and PIO1 (need 6 SMs, each PIO has 4)

    // Add program to both PIOs
    program_offset_pio0 = pio_add_program(pio0_inst, &pio_edge_counter_program);
    program_offset_pio1 = pio_add_program(pio1_inst, &pio_edge_counter_program);

    // Claim state machines - 4 from PIO0, 2 from PIO1
    sm_pclk = pio_claim_unused_sm(pio0_inst, true);
    sm_csync = pio_claim_unused_sm(pio0_inst, true);
    sm_r0 = pio_claim_unused_sm(pio0_inst, true);
    sm_g0 = pio_claim_unused_sm(pio0_inst, true);
    sm_b0 = pio_claim_unused_sm(pio1_inst, true);
    sm_r4 = pio_claim_unused_sm(pio1_inst, true);

    printf("PIO0 SMs: PCLK=%d, CSYNC=%d, R0=%d, G0=%d\n",
           sm_pclk, sm_csync, sm_r0, sm_g0);
    printf("PIO1 SMs: B0=%d, R4=%d\n", sm_b0, sm_r4);
    printf("\n");

    // FIRST: Check GPIO states BEFORE PIO takes over
    printf("Testing GPIO reads (before PIO initialization):\n");

    // Initialize pins as inputs for testing
    gpio_init(PIN_PCLK); gpio_set_dir(PIN_PCLK, GPIO_IN);
    gpio_init(PIN_CSYNC); gpio_set_dir(PIN_CSYNC, GPIO_IN);
    gpio_init(PIN_R0); gpio_set_dir(PIN_R0, GPIO_IN);
    gpio_init(PIN_G0); gpio_set_dir(PIN_G0, GPIO_IN);
    gpio_init(PIN_B0); gpio_set_dir(PIN_B0, GPIO_IN);

    for (int i = 0; i < 5; i++) {
        printf("GPIO: PCLK=%d CSYNC=%d R0=%d G0=%d B0=%d | ",
               gpio_get(PIN_PCLK), gpio_get(PIN_CSYNC), gpio_get(PIN_R0),
               gpio_get(PIN_G0), gpio_get(PIN_B0));

        // Count toggles in 100ms
        int pclk_toggles = 0, csync_toggles = 0;
        bool last_pclk = gpio_get(PIN_PCLK);
        bool last_csync = gpio_get(PIN_CSYNC);
        for (int j = 0; j < 100; j++) {
            sleep_us(1000);
            bool pclk = gpio_get(PIN_PCLK);
            bool csync = gpio_get(PIN_CSYNC);
            if (pclk != last_pclk) pclk_toggles++;
            if (csync != last_csync) csync_toggles++;
            last_pclk = pclk;
            last_csync = csync;
        }
        printf("Toggles: PCLK=%d CSYNC=%d\n", pclk_toggles, csync_toggles);
    }

    printf("\nNow initializing PIO for edge counting...\n");

    // Initialize counters (this will re-init the GPIOs for PIO use)
    init_edge_counter(pio0_inst, sm_pclk, PIN_PCLK, program_offset_pio0);
    init_edge_counter(pio0_inst, sm_csync, PIN_CSYNC, program_offset_pio0);
    init_edge_counter(pio0_inst, sm_r0, PIN_R0, program_offset_pio0);
    init_edge_counter(pio0_inst, sm_g0, PIN_G0, program_offset_pio0);
    init_edge_counter(pio1_inst, sm_b0, PIN_B0, program_offset_pio1);
    init_edge_counter(pio1_inst, sm_r4, PIN_R4, program_offset_pio1);

    // Start counting
    start_counters();

    printf("PIO edge counting started...\n\n");

    uint64_t last_time = time_us_64();

    while (true) {
        sleep_ms(1000);

        uint64_t now = time_us_64();
        float elapsed_sec = (now - last_time) / 1000000.0f;
        last_time = now;

        // Read edge counts for video pins (enable debug for first 3 reads)
        static int read_count = 0;
        bool debug = (read_count++ < 3);

        uint32_t count_pclk = read_edge_count(pio0_inst, sm_pclk, "PCLK", debug);
        uint32_t count_csync = read_edge_count(pio0_inst, sm_csync, "CSYNC", debug);
        uint32_t count_r0 = read_edge_count(pio0_inst, sm_r0, "R0", debug);
        uint32_t count_g0 = read_edge_count(pio0_inst, sm_g0, "G0", debug);
        uint32_t count_b0 = read_edge_count(pio1_inst, sm_b0, "B0", debug);
        uint32_t count_r4 = read_edge_count(pio1_inst, sm_r4, "R4", debug);

        // Calculate frequencies
        float freq_pclk = count_pclk / elapsed_sec;
        float freq_csync = count_csync / elapsed_sec;
        float freq_r0 = count_r0 / elapsed_sec;
        float freq_g0 = count_g0 / elapsed_sec;
        float freq_b0 = count_b0 / elapsed_sec;
        float freq_r4 = count_r4 / elapsed_sec;

        // Print results
        printf("\r");  // Return to start of line
        print_freq("PCLK ", freq_pclk);
        printf(" | ");
        print_freq("CSYNC", freq_csync);
        printf(" | ");
        print_freq("R0", freq_r0);
        printf(" | ");
        print_freq("G0", freq_g0);
        printf(" | ");
        print_freq("B0", freq_b0);
        printf(" | ");
        print_freq("R4", freq_r4);
        printf("   ");  // Clear any trailing chars

        fflush(stdout);
    }

    return 0;
}
