/**
 * I2S Debug - Simple pin state reader
 */

#include <stdio.h>
#include "pico/stdlib.h"
#include "hardware/gpio.h"

#define BCK 22
#define DAT 23
#define WS  24

int main() {
    stdio_init_all();

    gpio_init(BCK); gpio_set_dir(BCK, GPIO_IN);
    gpio_init(DAT); gpio_set_dir(DAT, GPIO_IN);
    gpio_init(WS);  gpio_set_dir(WS, GPIO_IN);
    gpio_init(PICO_DEFAULT_LED_PIN);
    gpio_set_dir(PICO_DEFAULT_LED_PIN, GPIO_OUT);

    // Wait for USB with blinking
    for (int i = 0; i < 30; i++) {
        gpio_put(PICO_DEFAULT_LED_PIN, i & 1);
        sleep_ms(100);
    }
    gpio_put(PICO_DEFAULT_LED_PIN, 1);

    printf("\n\n================================\n");
    printf("  Simple Pin State Debug\n");
    printf("================================\n\n");

    // Just count edges for 100ms without blocking waits
    printf("Counting edges for 100ms...\n");
    uint32_t bck_edges = 0, dat_edges = 0, ws_edges = 0;
    uint32_t last_bck = gpio_get(BCK);
    uint32_t last_dat = gpio_get(DAT);
    uint32_t last_ws = gpio_get(WS);

    uint32_t start = time_us_32();
    while (time_us_32() - start < 100000) {
        uint32_t b = gpio_get(BCK);
        uint32_t d = gpio_get(DAT);
        uint32_t w = gpio_get(WS);
        if (b != last_bck) { bck_edges++; last_bck = b; }
        if (d != last_dat) { dat_edges++; last_dat = d; }
        if (w != last_ws) { ws_edges++; last_ws = w; }
    }

    printf("In 100ms:\n");
    printf("  BCK (GPIO%d): %lu edges = %.1f kHz\n", BCK, bck_edges, bck_edges/200.0);
    printf("  DAT (GPIO%d): %lu edges = %.1f kHz\n", DAT, dat_edges, dat_edges/200.0);
    printf("  WS  (GPIO%d): %lu edges = %.1f kHz\n", WS, ws_edges, ws_edges/200.0);
    printf("\n");

    // Sample 64 bits manually
    printf("Sampling 64 DAT bits on BCK edges:\n");
    char bits[65];
    for (int i = 0; i < 64; i++) {
        // Wait for BCK transition (either edge)
        uint32_t initial = gpio_get(BCK);
        uint32_t timeout = 10000;
        while (gpio_get(BCK) == initial && timeout > 0) { timeout--; }
        bits[i] = gpio_get(DAT) ? '1' : '0';
    }
    bits[64] = 0;
    printf("  %s\n\n", bits);

    printf("Done!\n");
    stdio_flush();

    while (1) {
        gpio_xor_mask(1 << PICO_DEFAULT_LED_PIN);
        sleep_ms(500);
    }
    return 0;
}
