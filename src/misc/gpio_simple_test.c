/**
 * Simple GPIO Test - Single Pin (R0 = GP42)
 * Tests RP2350B GPIO Bank 1 access with PIO
 */

#include <stdio.h>
#include "pico/stdlib.h"
#include "hardware/gpio.h"
#include "hardware/pio.h"
#include "hardware/clocks.h"

#define TEST_PIN 29  // PCLK (6 MHz constant signal)

// PIO program 1: Simple loop (no wait)
static const uint16_t pio_loop_program_instructions[] = {
    0x0040, //  0: jmp x--, 0  (decrement X, loop back)
};

static const struct pio_program pio_loop_program = {
    .instructions = pio_loop_program_instructions,
    .length = 1,
    .origin = -1,
};

// PIO program 2: Edge counter (with wait)
static const uint16_t pio_edge_program_instructions[] = {
    0x2020, //  0: wait 0 pin, 0  (wait for low)
    0x20a0, //  1: wait 1 pin, 0  (wait for high = rising edge)
    0x0040, //  2: jmp x--, 0     (decrement X, loop back)
};

static const struct pio_program pio_edge_program = {
    .instructions = pio_edge_program_instructions,
    .length = 3,
    .origin = -1,
};

int main() {
    stdio_init_all();
    sleep_ms(2000);

    printf("\n");
    printf("=============================================\n");
    printf("  RP2350B GPIO Test - PCLK (GP29)\n");
    printf("=============================================\n");
    printf("System clock: %lu MHz\n", clock_get_hz(clk_sys) / 1000000);
    printf("Testing pin: GP%d (PCLK - expect 6 MHz)\n\n", TEST_PIN);

    // Test 1: Direct GPIO read
    printf("TEST 1: Direct GPIO Read\n");
    gpio_init(TEST_PIN);
    gpio_set_dir(TEST_PIN, GPIO_IN);

    for (int i = 0; i < 5; i++) {
        printf("  GPIO state: %d | ", gpio_get(TEST_PIN));

        // Count toggles in 100ms
        int toggles = 0;
        bool last = gpio_get(TEST_PIN);
        for (int j = 0; j < 100; j++) {
            sleep_us(1000);
            bool current = gpio_get(TEST_PIN);
            if (current != last) toggles++;
            last = current;
        }
        printf("Toggles: %d\n", toggles);
    }

    // Test 2: PIO simple loop (baseline)
    printf("\nTEST 2: PIO Simple Loop (no wait)\n");

    PIO pio = pio0;
    uint offset = pio_add_program(pio, &pio_loop_program);
    uint sm = pio_claim_unused_sm(pio, true);

    pio_sm_config c = pio_get_default_sm_config();
    sm_config_set_wrap(&c, offset, offset);
    sm_config_set_clkdiv(&c, 1.0f);

    pio_sm_init(pio, sm, offset, &c);
    pio_sm_exec(pio, sm, pio_encode_set(pio_x, 0));
    pio_sm_set_enabled(pio, sm, true);

    sleep_ms(1000);

    // Read X register
    pio_sm_set_enabled(pio, sm, false);
    pio_sm_exec(pio, sm, pio_encode_mov(pio_isr, pio_x));
    pio_sm_exec(pio, sm, pio_encode_push(false, false));
    uint32_t x_val = pio_sm_is_rx_fifo_empty(pio, sm) ? 0 : pio_sm_get(pio, sm);
    printf("  Iterations: %lu million (baseline PIO speed)\n", (0xFFFFFFFF - x_val) / 1000000);

    pio_sm_set_enabled(pio, sm, false);
    pio_remove_program(pio, &pio_loop_program, offset);
    pio_sm_unclaim(pio, sm);

    // Test 3: PIO edge counter (with wait pin)
    printf("\nTEST 3: PIO Edge Counter (with wait pin)\n");

    offset = pio_add_program(pio, &pio_edge_program);
    sm = pio_claim_unused_sm(pio, true);

    c = pio_get_default_sm_config();
    sm_config_set_wrap(&c, offset, offset + 2);
    sm_config_set_in_pins(&c, TEST_PIN);  // Set input pin base
    sm_config_set_jmp_pin(&c, TEST_PIN);  // Set jump pin
    sm_config_set_clkdiv(&c, 1.0f);

    gpio_init(TEST_PIN);
    gpio_set_dir(TEST_PIN, GPIO_IN);
    pio_gpio_init(pio, TEST_PIN);

    pio_sm_init(pio, sm, offset, &c);
    pio_sm_exec(pio, sm, pio_encode_set(pio_x, 0));
    pio_sm_set_enabled(pio, sm, true);

    sleep_ms(1000);

    // Read X register
    pio_sm_set_enabled(pio, sm, false);
    pio_sm_exec(pio, sm, pio_encode_mov(pio_isr, pio_x));
    pio_sm_exec(pio, sm, pio_encode_push(false, false));
    x_val = pio_sm_is_rx_fifo_empty(pio, sm) ? 0 : pio_sm_get(pio, sm);
    uint32_t edges = 0xFFFFFFFF - x_val;
    printf("  Edges counted: %lu (%lu MHz)\n", edges, edges / 1000000);

    if (edges > 5000000 && edges < 7000000) {
        printf("  ✓ SUCCESS! Detected ~6 MHz PCLK signal!\n");
    } else if (edges == 0) {
        printf("  ✗ FAIL: No edges detected (wait pin not working)\n");
    } else {
        printf("  ? Unexpected frequency\n");
    }

    printf("\nTest complete!\n");

    while (true) {
        tight_loop_contents();
    }
}
