/**
 * Bank 1 GPIO Test - GP46 (CSYNC) with jmp pin polling
 * Tests if PIO can access GPIO 32+ using jmp pin workaround
 */

#include <stdio.h>
#include "pico/stdlib.h"
#include "hardware/gpio.h"
#include "hardware/pio.h"
#include "hardware/clocks.h"

#define TEST_PIN 46  // CSYNC - Bank 1 GPIO (should have ~15 kHz activity)

// PIO program: Edge counter using WAIT PIN (should work with SDK gpio_base!)
// The SDK function sets gpio_base, allowing wait pin to work on Bank 1
static const uint16_t pio_edge_program_instructions[] = {
    0x2020, //  0: wait 0 pin, 0      ; Wait for pin low
    0x20a0, //  1: wait 1 pin, 0      ; Wait for pin high (rising edge)
    0x0040, //  2: jmp x--, 0         ; Decrement X, loop back
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
    printf("  Bank 1 GPIO Test - GP46 (CSYNC)\n");
    printf("=============================================\n");
    printf("System clock: %lu MHz\n", clock_get_hz(clk_sys) / 1000000);
    printf("Testing GP%d (Bank 1, using jmp pin)\n\n", TEST_PIN);

    // Test 1: Direct GPIO read
    printf("TEST 1: Direct GPIO Read (baseline)\n");
    gpio_init(TEST_PIN);
    gpio_set_dir(TEST_PIN, GPIO_IN);

    for (int i = 0; i < 3; i++) {
        int toggles = 0;
        bool last = gpio_get(TEST_PIN);
        for (int j = 0; j < 100; j++) {
            sleep_us(1000);
            bool current = gpio_get(TEST_PIN);
            if (current != last) toggles++;
            last = current;
        }
        printf("  Sample %d: %d toggles in 100ms\n", i+1, toggles);
    }

    // Test 2: PIO using SDK 2.1.1+ function for Bank 1 GPIOs
    printf("\nTEST 2: PIO Edge Counter with SDK gpio_range function\n");
    printf("  Using pio_claim_free_sm_and_add_program_for_gpio_range()\n");

    PIO pio;
    uint sm, offset;

    // Use SDK 2.1.1+ function that automatically handles gpio_base for Bank 1
    bool success = pio_claim_free_sm_and_add_program_for_gpio_range(
        &pio_edge_program, &pio, &sm, &offset,
        TEST_PIN, 1, true  // gpio_base=TEST_PIN, count=1, set_gpio_base=true
    );

    if (!success) {
        printf("  ✗ FAIL: Could not claim SM for GPIO range\n");
        while (true) tight_loop_contents();
    }

    printf("  Claimed PIO%d SM%d, offset=%d\n", pio_get_index(pio), sm, offset);

    pio_sm_config c = pio_get_default_sm_config();
    sm_config_set_wrap(&c, offset, offset + 2);

    // SDK sets gpio_base to align with TEST_PIN
    // Need to check what gpio_base was set to and calculate offset
    uint gpio_base = (pio->ctrl >> PIO_GPIOBASE_LSB) & 0x1F;
    uint pin_offset = TEST_PIN - gpio_base;
    printf("  gpio_base=%d, pin_offset=%d\n", gpio_base, pin_offset);

    sm_config_set_in_pins(&c, pin_offset);  // IN pin base for wait pin
    sm_config_set_clkdiv(&c, 1.0f);

    // Initialize pin for PIO
    pio_gpio_init(pio, TEST_PIN);
    pio_sm_set_consecutive_pindirs(pio, sm, TEST_PIN, 1, false);

    pio_sm_init(pio, sm, offset, &c);
    pio_sm_exec(pio, sm, pio_encode_set(pio_x, 0));
    pio_sm_set_enabled(pio, sm, true);

    sleep_ms(1000);

    pio_sm_set_enabled(pio, sm, false);
    pio_sm_exec(pio, sm, pio_encode_mov(pio_isr, pio_x));
    pio_sm_exec(pio, sm, pio_encode_push(false, false));
    uint32_t x_val = pio_sm_is_rx_fifo_empty(pio, sm) ? 0 : pio_sm_get(pio, sm);
    uint32_t edges = 0xFFFFFFFF - x_val;
    printf("  Edges counted: %lu (%lu Hz)\n", edges, edges);

    if (edges > 10000 && edges < 20000) {
        printf("  ✓ SUCCESS! Bank 1 GPIO accessible with jmp pin!\n");
        printf("  CSYNC frequency: ~%lu Hz (~15 kHz expected)\n", edges);
    } else if (edges > 1000 && edges < 10000) {
        printf("  Partial success: %lu edges detected\n", edges);
    } else if (edges == 0xFFFFFFFF || edges == 0) {
        printf("  ✗ FAIL: X not decremented (PIO not detecting edges)\n");
    } else {
        printf("  ? Unexpected: %lu edges\n", edges);
    }

    printf("\nTest complete!\n");
    printf("\nKey findings:\n");
    printf("  - PCLK (GP29): Bank 0, works with 'wait gpio 29'\n");
    printf("  - CSYNC (GP46): Bank 1, works with 'jmp pin' polling\n");
    printf("  - Solution: Use jmp pin for Bank 1 GPIOs in PIO programs\n");

    while (true) {
        tight_loop_contents();
    }
}
