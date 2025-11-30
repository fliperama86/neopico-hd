/**
 * GPIO Test - Simple pin toggle test
 *
 * Toggles all DVI pins slowly so you can probe them with a multimeter
 * to verify wiring continuity.
 */

#include <stdio.h>
#include "pico/stdlib.h"
#include "hardware/gpio.h"

// Our DVI pins
#define PIN_D0N  16
#define PIN_D0P  17
#define PIN_D1N  18
#define PIN_D1P  19
#define PIN_D2N  20
#define PIN_D2P  21
#define PIN_CLKN 26
#define PIN_CLKP 27

const uint dvi_pins[] = {PIN_D0N, PIN_D0P, PIN_D1N, PIN_D1P, PIN_D2N, PIN_D2P, PIN_CLKN, PIN_CLKP};
const char* pin_names[] = {"D0N(16)", "D0P(17)", "D1N(18)", "D1P(19)", "D2N(20)", "D2P(21)", "CLKN(26)", "CLKP(27)"};
#define NUM_PINS 8

int main() {
    stdio_init_all();

    // Init LED
    gpio_init(PICO_DEFAULT_LED_PIN);
    gpio_set_dir(PICO_DEFAULT_LED_PIN, GPIO_OUT);

    // Init all DVI pins as outputs
    for (int i = 0; i < NUM_PINS; i++) {
        gpio_init(dvi_pins[i]);
        gpio_set_dir(dvi_pins[i], GPIO_OUT);
        gpio_put(dvi_pins[i], 0);
    }

    printf("\n\n=== GPIO Test for DVI Pins ===\n");
    printf("Each pin will go HIGH for 2 seconds, then LOW.\n");
    printf("Use a multimeter to verify ~3.3V on the corresponding Spotpear pad.\n\n");

    sleep_ms(2000);

    while (true) {
        for (int i = 0; i < NUM_PINS; i++) {
            printf(">>> PIN %s = HIGH <<<\n", pin_names[i]);
            gpio_put(PICO_DEFAULT_LED_PIN, 1);
            gpio_put(dvi_pins[i], 1);

            sleep_ms(2000);

            printf("    PIN %s = low\n\n", pin_names[i]);
            gpio_put(PICO_DEFAULT_LED_PIN, 0);
            gpio_put(dvi_pins[i], 0);

            sleep_ms(500);
        }

        printf("\n--- Cycle complete, restarting ---\n\n");
        sleep_ms(1000);
    }

    return 0;
}
