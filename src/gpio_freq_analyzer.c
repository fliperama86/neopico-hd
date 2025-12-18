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

// Pins to monitor
#define PIN_DAT 36
#define PIN_WS  37
#define PIN_BCK 38

// We'll use 3 state machines, one per pin
static PIO pio = pio0;
static uint sm_dat, sm_ws, sm_bck;
static uint program_offset;

static void init_edge_counter(uint sm, uint pin) {
    pio_sm_config c = pio_get_default_sm_config();

    sm_config_set_wrap(&c, program_offset + pio_edge_counter_wrap_target,
                          program_offset + pio_edge_counter_wrap);

    // Set input pin
    sm_config_set_in_pins(&c, pin);
    sm_config_set_jmp_pin(&c, pin);

    // Configure the pin as PIO input
    pio_gpio_init(pio, pin);
    pio_sm_set_consecutive_pindirs(pio, sm, pin, 1, false);

    // Run at full system clock for maximum resolution
    sm_config_set_clkdiv(&c, 1.0f);

    pio_sm_init(pio, sm, program_offset, &c);
}

// Last read values for delta calculation
static uint32_t last_x_value[3] = {0, 0, 0};

static uint32_t read_edge_count(uint sm) {
    // Read X register by executing "mov isr, x" then "push"
    // X decrements on each edge, wrapping around

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
    uint32_t count = last_x_value[sm] - x_value;
    last_x_value[sm] = x_value;

    return count;
}

static void start_counters(void) {
    // Initialize X to 0 on all SMs (will wrap on first decrement)
    pio_sm_exec(pio, sm_dat, pio_encode_set(pio_x, 0));
    pio_sm_exec(pio, sm_ws, pio_encode_set(pio_x, 0));
    pio_sm_exec(pio, sm_bck, pio_encode_set(pio_x, 0));

    // Initialize last values for delta calculation
    last_x_value[sm_dat] = 0;
    last_x_value[sm_ws] = 0;
    last_x_value[sm_bck] = 0;

    // Start all SMs simultaneously
    pio_enable_sm_mask_in_sync(pio, (1u << sm_dat) | (1u << sm_ws) | (1u << sm_bck));
}

int main() {
    stdio_init_all();
    sleep_ms(2000);

    printf("\n");
    printf("========================================\n");
    printf("  GPIO Frequency Analyzer (PIO-based)\n");
    printf("========================================\n");
    printf("System clock: %lu MHz\n", clock_get_hz(clk_sys) / 1000000);
    printf("\n");
    printf("Monitoring I2S pins:\n");
    printf("  GP%d: DAT (serial data)\n", PIN_DAT);
    printf("  GP%d: WS  (word select / sample rate)\n", PIN_WS);
    printf("  GP%d: BCK (bit clock)\n", PIN_BCK);
    printf("\n");

    // Need gpio_base for pins >= 32
    pio_set_gpio_base(pio, 16);  // Access GPIO 16-47

    // Load PIO program
    program_offset = pio_add_program(pio, &pio_edge_counter_program);

    // Claim state machines
    sm_dat = pio_claim_unused_sm(pio, true);
    sm_ws = pio_claim_unused_sm(pio, true);
    sm_bck = pio_claim_unused_sm(pio, true);

    printf("PIO configured: SM%d=DAT, SM%d=WS, SM%d=BCK\n\n", sm_dat, sm_ws, sm_bck);

    // Initialize counters (pins need gpio_base offset for >32)
    init_edge_counter(sm_dat, PIN_DAT);
    init_edge_counter(sm_ws, PIN_WS);
    init_edge_counter(sm_bck, PIN_BCK);

    // Start counting
    start_counters();

    printf("Counting edges... (updates every second)\n\n");

    uint64_t last_time = time_us_64();

    while (true) {
        sleep_ms(1000);

        uint64_t now = time_us_64();
        float elapsed_sec = (now - last_time) / 1000000.0f;
        last_time = now;

        // Read counts
        uint32_t count_dat = read_edge_count(sm_dat);
        uint32_t count_ws = read_edge_count(sm_ws);
        uint32_t count_bck = read_edge_count(sm_bck);

        // Calculate frequencies (rising edges per second)
        float freq_dat = count_dat / elapsed_sec;
        float freq_ws = count_ws / elapsed_sec;
        float freq_bck = count_bck / elapsed_sec;

        // Print results on same lines (update in place)
        printf("\r\033[K");  // Clear line
        printf("DAT: ");
        if (freq_dat < 100) printf("%.1f Hz    ", freq_dat);
        else if (freq_dat < 100000) printf("%.2f kHz  ", freq_dat / 1000);
        else printf("%.3f MHz  ", freq_dat / 1000000);

        printf("| WS: ");
        if (freq_ws < 100) printf("%.1f Hz    ", freq_ws);
        else if (freq_ws < 100000) printf("%.2f kHz  ", freq_ws / 1000);
        else printf("%.3f MHz  ", freq_ws / 1000000);

        printf("| BCK: ");
        if (freq_bck < 100) printf("%.1f Hz    ", freq_bck);
        else if (freq_bck < 100000) printf("%.2f kHz  ", freq_bck / 1000);
        else printf("%.3f MHz  ", freq_bck / 1000000);

        fflush(stdout);
    }

    return 0;
}
