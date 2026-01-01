/**
 * GPIO Activity Map - Hardware-in-Loop Diagnostic
 *
 * Scans all 48 GPIOs for 10ms and counts physical toggles.
 * This bypasses PIO entirely to verify MVS is physically connected.
 */

#include <stdio.h>
#include "pico/stdlib.h"
#include "hardware/gpio.h"
#include "hardware/clocks.h"

#define SCAN_DURATION_MS 10
#define SAMPLE_INTERVAL_US 0  // MAX SPEED - no delay between samples

int main() {
    stdio_init_all();
    sleep_ms(5000);  // Wait for USB CDC

    printf("\n\n=== GPIO ACTIVITY MAP ===\n");
    printf("Scanning all 48 GPIOs for %dms to detect MVS signals\n\n", SCAN_DURATION_MS);
    printf("System Clock: %lu MHz\n", clock_get_hz(clk_sys) / 1000000);

    // Initialize all GPIOs as inputs with pulls disabled
    printf("\nInitializing GPIOs...\n");
    for (uint i = 0; i < 48; i++) {
        gpio_init(i);
        gpio_set_dir(i, GPIO_IN);
        gpio_disable_pulls(i);
    }

    printf("GPIO init complete\n");

    // Verify critical pin functions
    printf("\nGPIO Function Check:\n");
    printf("  GP29 function: %d\n", gpio_get_function(29));
    printf("  GP46 function: %d\n", gpio_get_function(46));

    printf("\nStarting scan...\n");
    sleep_ms(100);

    // Activity counters
    uint32_t toggle_count[48] = {0};
    bool last_state[48];

    // Read initial state
    for (uint i = 0; i < 48; i++) {
        last_state[i] = gpio_get(i);
    }

    // Scan for activity
    absolute_time_t start = get_absolute_time();
    absolute_time_t end = delayed_by_ms(start, SCAN_DURATION_MS);

    uint32_t total_samples = 0;
    while (absolute_time_diff_us(get_absolute_time(), end) > 0) {
        for (uint i = 0; i < 48; i++) {
            bool current = gpio_get(i);
            if (current != last_state[i]) {
                toggle_count[i]++;
                last_state[i] = current;
            }
        }
        total_samples++;
        busy_wait_us(SAMPLE_INTERVAL_US);
    }

    printf("\n=== RESULTS ===\n");
    printf("Total samples: %lu (%.1f kHz)\n\n",
           total_samples, (float)total_samples / SCAN_DURATION_MS);

    // Group by activity level
    printf("HIGH ACTIVITY (>1000 toggles - likely clocks):\n");
    for (uint i = 0; i < 48; i++) {
        if (toggle_count[i] > 1000) {
            printf("  GP%02u: %lu toggles (~%lu kHz)\n",
                   i, toggle_count[i], toggle_count[i] / SCAN_DURATION_MS);
        }
    }

    printf("\nMEDIUM ACTIVITY (100-1000 toggles - likely sync signals):\n");
    for (uint i = 0; i < 48; i++) {
        if (toggle_count[i] >= 100 && toggle_count[i] <= 1000) {
            printf("  GP%02u: %lu toggles (~%lu Hz)\n",
                   i, toggle_count[i], toggle_count[i] * 100 / SCAN_DURATION_MS);
        }
    }

    printf("\nLOW ACTIVITY (1-99 toggles):\n");
    for (uint i = 0; i < 48; i++) {
        if (toggle_count[i] > 0 && toggle_count[i] < 100) {
            printf("  GP%02u: %lu toggles\n", i, toggle_count[i]);
        }
    }

    printf("\nNO ACTIVITY (0 toggles - check connections):\n");
    bool all_active = true;
    for (uint i = 0; i < 48; i++) {
        if (toggle_count[i] == 0) {
            printf("  GP%02u: STATIC (state=%d)\n", i, last_state[i]);
            all_active = false;
        }
    }

    if (all_active) {
        printf("  (All GPIOs showed activity!)\n");
    }

    printf("\n=== EXPECTED FOR MVS ===\n");
    printf("  GP29 (PCLK):  ~60,000 toggles (6 MHz)\n");
    printf("  GP46 (CSYNC): ~160 toggles (16 kHz)\n");
    printf("  GP31-45 (RGB): Variable (pixel data)\n");

    printf("\n=== ANALYSIS ===\n");
    if (toggle_count[29] > 50000) {
        printf("✅ PCLK detected on GP29\n");
    } else if (toggle_count[29] > 0) {
        printf("⚠️  GP29 has activity but frequency is wrong (%lu toggles)\n", toggle_count[29]);
    } else {
        printf("❌ NO PCLK on GP29 - MVS might be OFF or disconnected\n");
    }

    if (toggle_count[46] > 100 && toggle_count[46] < 300) {
        printf("✅ CSYNC detected on GP46\n");
    } else if (toggle_count[46] > 0) {
        printf("⚠️  GP46 has activity but frequency is wrong (%lu toggles)\n", toggle_count[46]);
    } else {
        printf("❌ NO CSYNC on GP46 - check CSYNC connection\n");
    }

    // Check for RP2350-E9 sticky pins (static high ~2.1V)
    printf("\n=== RP2350-E9 STICKY PIN CHECK ===\n");
    bool sticky_found = false;
    for (uint i = 32; i < 48; i++) {
        if (toggle_count[i] == 0 && last_state[i] == 1) {
            printf("⚠️  GP%02u stuck HIGH - possible E9 erratum (sticky pull-down)\n", i);
            sticky_found = true;
        }
    }
    if (!sticky_found) {
        printf("  No sticky pins detected\n");
    }

    printf("\nTest complete. Reset to run again.\n");

    while (true) {
        tight_loop_contents();
    }
}
