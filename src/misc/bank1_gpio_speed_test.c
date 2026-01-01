/**
 * Bank 1 GPIO Read Speed Test
 *
 * Tests actual gpio_get() performance on Bank 0 vs Bank 1 pins.
 * Measures maximum toggle detection rate to identify Bank 1 limitations.
 */

#include <stdio.h>
#include "pico/stdlib.h"
#include "hardware/gpio.h"
#include "hardware/clocks.h"

#define TEST_DURATION_MS 100
#define PIN_BANK0_TEST 28  // GP28 in Bank 0
#define PIN_BANK1_PCLK 29  // GP29 in Bank 1
#define PIN_BANK1_CSYNC 46  // GP46 in Bank 1

uint32_t count_toggles_tight_loop(uint pin, uint32_t duration_ms) {
    uint32_t toggle_count = 0;
    bool last_state = gpio_get(pin);

    absolute_time_t start = get_absolute_time();
    absolute_time_t end = delayed_by_ms(start, duration_ms);

    while (absolute_time_diff_us(get_absolute_time(), end) > 0) {
        bool current = gpio_get(pin);
        if (current != last_state) {
            toggle_count++;
            last_state = current;
        }
    }

    return toggle_count;
}

int main() {
    stdio_init_all();
    sleep_ms(5000);

    printf("\n\n=== BANK 1 GPIO READ SPEED TEST ===\n");
    printf("System Clock: %lu MHz\n\n", clock_get_hz(clk_sys) / 1000000);

    // Init all test pins as inputs
    gpio_init(PIN_BANK0_TEST);
    gpio_set_dir(PIN_BANK0_TEST, GPIO_IN);
    gpio_disable_pulls(PIN_BANK0_TEST);

    gpio_init(PIN_BANK1_PCLK);
    gpio_set_dir(PIN_BANK1_PCLK, GPIO_IN);
    gpio_disable_pulls(PIN_BANK1_PCLK);

    gpio_init(PIN_BANK1_CSYNC);
    gpio_set_dir(PIN_BANK1_CSYNC, GPIO_IN);
    gpio_disable_pulls(PIN_BANK1_CSYNC);

    printf("GPIO Functions:\n");
    printf("  GP%d (Bank 0): function=%d\n", PIN_BANK0_TEST, gpio_get_function(PIN_BANK0_TEST));
    printf("  GP%d (Bank 1): function=%d\n", PIN_BANK1_PCLK, gpio_get_function(PIN_BANK1_PCLK));
    printf("  GP%d (Bank 1): function=%d\n", PIN_BANK1_CSYNC, gpio_get_function(PIN_BANK1_CSYNC));

    printf("\nRunning tight-loop toggle counting for %dms on each pin...\n\n", TEST_DURATION_MS);

    // Test Bank 0
    printf("Testing GP%d (Bank 0)...\n", PIN_BANK0_TEST);
    uint32_t bank0_toggles = count_toggles_tight_loop(PIN_BANK0_TEST, TEST_DURATION_MS);
    printf("  Toggles detected: %lu (%lu Hz)\n\n",
           bank0_toggles, bank0_toggles * 1000 / TEST_DURATION_MS);

    // Test Bank 1 - PCLK
    printf("Testing GP%d (Bank 1 - PCLK)...\n", PIN_BANK1_PCLK);
    uint32_t pclk_toggles = count_toggles_tight_loop(PIN_BANK1_PCLK, TEST_DURATION_MS);
    printf("  Toggles detected: %lu (%lu Hz)\n",
           pclk_toggles, pclk_toggles * 1000 / TEST_DURATION_MS);
    printf("  Expected for 6 MHz: ~1,200,000 toggles\n");
    printf("  Detection efficiency: %.2f%%\n\n",
           (float)pclk_toggles / 1200000.0f * 100.0f);

    // Test Bank 1 - CSYNC
    printf("Testing GP%d (Bank 1 - CSYNC)...\n", PIN_BANK1_CSYNC);
    uint32_t csync_toggles = count_toggles_tight_loop(PIN_BANK1_CSYNC, TEST_DURATION_MS);
    printf("  Toggles detected: %lu (%lu Hz)\n",
           csync_toggles, csync_toggles * 1000 / TEST_DURATION_MS);
    printf("  Expected for 15.7 kHz: ~3,140 toggles\n");
    printf("  Detection efficiency: %.2f%%\n\n",
           (float)csync_toggles / 3140.0f * 100.0f);

    printf("=== ANALYSIS ===\n");
    if (pclk_toggles < 100000) {
        printf("❌ Bank 1 GPIO reads are TOO SLOW to catch 6 MHz signal\n");
        printf("   This explains why PIO capture fails - gpio_get() misses edges\n");
    } else {
        printf("✅ Bank 1 GPIO reads are fast enough\n");
    }

    printf("\nTest complete.\n");

    while (true) {
        tight_loop_contents();
    }
}
