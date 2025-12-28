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

static const uint16_t pio_edge_counter_program_instructions[] = {
    //     .wrap_target
    0x2020, //  0: wait   0 pin, 0        ; Wait for pin low
    0x20a0, //  1: wait   1 pin, 0        ; Wait for pin high (rising edge)
    0x0040, //  2: jmp    x--, 0          ; Decrement X, loop back
    //     .wrap
};

static const struct pio_program pio_edge_counter_program = {
    .instructions = pio_edge_counter_program_instructions,
    .length = 3,
    .origin = -1,
};

// Pins to monitor - both old (Bank 1) and new (Bank 0) locations
#define PIN_DAT_NEW 16   // New Bank 0 location
#define PIN_WS_NEW  17
#define PIN_BCK_NEW 18
#define PIN_DAT_OLD 36   // Old Bank 1 location
#define PIN_WS_OLD  37
#define PIN_BCK_OLD 38

// We'll use PIO1 for Bank 0 pins, PIO0 for Bank 1 pins
static PIO pio_bank0 = pio1;  // For GPIO 16-18
static PIO pio_bank1 = pio0;  // For GPIO 36-38 (needs gpio_base=16)
static uint sm_dat_new, sm_ws_new, sm_bck_new;
static uint sm_dat_old, sm_ws_old, sm_bck_old;
static uint program_offset_bank0, program_offset_bank1;

static void init_edge_counter(PIO pio, uint sm, uint pin, uint offset) {
    pio_sm_config c = pio_get_default_sm_config();

    sm_config_set_wrap(&c, offset + pio_edge_counter_wrap_target,
                          offset + pio_edge_counter_wrap);

    // Set input pin
    sm_config_set_in_pins(&c, pin);
    sm_config_set_jmp_pin(&c, pin);

    // Configure the pin as PIO input
    pio_gpio_init(pio, pin);
    pio_sm_set_consecutive_pindirs(pio, sm, pin, 1, false);

    // Run at full system clock for maximum resolution
    sm_config_set_clkdiv(&c, 1.0f);

    pio_sm_init(pio, sm, offset, &c);
}

// Last read values for delta calculation (indexed by PIO*4 + SM)
static uint32_t last_x_value[8] = {0};

static uint32_t read_edge_count(PIO pio, uint sm) {
    // Read X register by executing "mov isr, x" then "push"
    // X decrements on each edge, wrapping around
    uint idx = (pio == pio0 ? 0 : 4) + sm;

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

    return count;
}

static void start_counters(void) {
    // Initialize X to 0 on all SMs (will wrap on first decrement)
    // Bank 0 (new pins on PIO1)
    pio_sm_exec(pio_bank0, sm_dat_new, pio_encode_set(pio_x, 0));
    pio_sm_exec(pio_bank0, sm_ws_new, pio_encode_set(pio_x, 0));
    pio_sm_exec(pio_bank0, sm_bck_new, pio_encode_set(pio_x, 0));
    // Bank 1 (old pins on PIO0)
    pio_sm_exec(pio_bank1, sm_dat_old, pio_encode_set(pio_x, 0));
    pio_sm_exec(pio_bank1, sm_ws_old, pio_encode_set(pio_x, 0));
    pio_sm_exec(pio_bank1, sm_bck_old, pio_encode_set(pio_x, 0));

    // Start all SMs simultaneously
    pio_enable_sm_mask_in_sync(pio_bank0, (1u << sm_dat_new) | (1u << sm_ws_new) | (1u << sm_bck_new));
    pio_enable_sm_mask_in_sync(pio_bank1, (1u << sm_dat_old) | (1u << sm_ws_old) | (1u << sm_bck_old));
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
    printf("  GPIO Frequency Analyzer (PIO-based)\n");
    printf("=============================================\n");
    printf("System clock: %lu MHz\n", clock_get_hz(clk_sys) / 1000000);
    printf("\n");
    printf("Monitoring I2S pins on BOTH banks:\n");
    printf("  NEW (Bank 0): GP%d=DAT, GP%d=WS, GP%d=BCK\n", PIN_DAT_NEW, PIN_WS_NEW, PIN_BCK_NEW);
    printf("  OLD (Bank 1): GP%d=DAT, GP%d=WS, GP%d=BCK\n", PIN_DAT_OLD, PIN_WS_OLD, PIN_BCK_OLD);
    printf("\n");

    // PIO1 for Bank 0 pins (gpio_base=0, default)
    program_offset_bank0 = pio_add_program(pio_bank0, &pio_edge_counter_program);
    sm_dat_new = pio_claim_unused_sm(pio_bank0, true);
    sm_ws_new = pio_claim_unused_sm(pio_bank0, true);
    sm_bck_new = pio_claim_unused_sm(pio_bank0, true);

    // PIO0 for Bank 1 pins (needs gpio_base=16)
    pio_set_gpio_base(pio_bank1, 16);
    program_offset_bank1 = pio_add_program(pio_bank1, &pio_edge_counter_program);
    sm_dat_old = pio_claim_unused_sm(pio_bank1, true);
    sm_ws_old = pio_claim_unused_sm(pio_bank1, true);
    sm_bck_old = pio_claim_unused_sm(pio_bank1, true);

    printf("PIO1 (Bank 0): SM%d=DAT, SM%d=WS, SM%d=BCK\n", sm_dat_new, sm_ws_new, sm_bck_new);
    printf("PIO0 (Bank 1): SM%d=DAT, SM%d=WS, SM%d=BCK\n", sm_dat_old, sm_ws_old, sm_bck_old);
    printf("\n");

    // Initialize counters
    init_edge_counter(pio_bank0, sm_dat_new, PIN_DAT_NEW, program_offset_bank0);
    init_edge_counter(pio_bank0, sm_ws_new, PIN_WS_NEW, program_offset_bank0);
    init_edge_counter(pio_bank0, sm_bck_new, PIN_BCK_NEW, program_offset_bank0);
    init_edge_counter(pio_bank1, sm_dat_old, PIN_DAT_OLD, program_offset_bank1);
    init_edge_counter(pio_bank1, sm_ws_old, PIN_WS_OLD, program_offset_bank1);
    init_edge_counter(pio_bank1, sm_bck_old, PIN_BCK_OLD, program_offset_bank1);

    // Start counting
    start_counters();

    printf("Counting edges... (updates every second)\n\n");

    uint64_t last_time = time_us_64();

    while (true) {
        sleep_ms(1000);

        uint64_t now = time_us_64();
        float elapsed_sec = (now - last_time) / 1000000.0f;
        last_time = now;

        // Read counts - new pins (Bank 0)
        uint32_t count_dat_new = read_edge_count(pio_bank0, sm_dat_new);
        uint32_t count_ws_new = read_edge_count(pio_bank0, sm_ws_new);
        uint32_t count_bck_new = read_edge_count(pio_bank0, sm_bck_new);
        // Read counts - old pins (Bank 1)
        uint32_t count_dat_old = read_edge_count(pio_bank1, sm_dat_old);
        uint32_t count_ws_old = read_edge_count(pio_bank1, sm_ws_old);
        uint32_t count_bck_old = read_edge_count(pio_bank1, sm_bck_old);

        // Calculate frequencies
        float freq_dat_new = count_dat_new / elapsed_sec;
        float freq_ws_new = count_ws_new / elapsed_sec;
        float freq_bck_new = count_bck_new / elapsed_sec;
        float freq_dat_old = count_dat_old / elapsed_sec;
        float freq_ws_old = count_ws_old / elapsed_sec;
        float freq_bck_old = count_bck_old / elapsed_sec;

        // Print results
        printf("\033[2K\r");  // Clear line
        printf("NEW(16-18): ");
        print_freq("D", freq_dat_new);
        printf(" ");
        print_freq("W", freq_ws_new);
        printf(" ");
        print_freq("B", freq_bck_new);
        printf("\n");

        printf("\033[2K");  // Clear line
        printf("OLD(36-38): ");
        print_freq("D", freq_dat_old);
        printf(" ");
        print_freq("W", freq_ws_old);
        printf(" ");
        print_freq("B", freq_bck_old);
        printf("\033[A");  // Move cursor up

        fflush(stdout);
    }

    return 0;
}
