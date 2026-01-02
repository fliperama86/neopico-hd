// Simple LA test - output known patterns on GP12-19
#include "pico/stdlib.h"
#include "hardware/gpio.h"

#define LED_PIN 25
#define TRIGGER_PIN 3

// GP12-19 are HSTX pins
#define HSTX_BASE 12

int main(void) {
    stdio_init_all();

    gpio_init(LED_PIN);
    gpio_set_dir(LED_PIN, GPIO_OUT);

    gpio_init(TRIGGER_PIN);
    gpio_set_dir(TRIGGER_PIN, GPIO_OUT);
    gpio_put(TRIGGER_PIN, 1);  // Idle HIGH

    // Initialize GP12-19 as outputs
    for (int i = 0; i < 8; i++) {
        gpio_init(HSTX_BASE + i);
        gpio_set_dir(HSTX_BASE + i, GPIO_OUT);
        gpio_put(HSTX_BASE + i, 0);
    }

    sleep_ms(1000);
    printf("\nLA Test - Simple patterns on GP12-19\n");
    printf("GP12 = CLK_N, GP13 = CLK_P (alternating)\n");
    printf("GP14-19 = D0-D2 (static pattern)\n");

    uint32_t counter = 0;

    while (1) {
        // Trigger pulse every 1000 cycles
        if (counter % 1000 == 0) {
            gpio_put(TRIGGER_PIN, 0);  // Falling edge trigger
        } else if (counter % 1000 == 100) {
            gpio_put(TRIGGER_PIN, 1);  // Back to idle
        }

        // GP12 (CLK_N) and GP13 (CLK_P) - complementary clock
        // At ~20MHz sys_clock, this gives ~10MHz toggle rate
        gpio_put(12, counter & 1);       // CLK_N
        gpio_put(13, (counter & 1) ^ 1); // CLK_P (inverted)

        // GP14-19: Static pattern for easy verification
        // D0_N=14, D0_P=15, D1_N=16, D1_P=17, D2_N=18, D2_P=19
        gpio_put(14, 0);  // D0_N = 0
        gpio_put(15, 1);  // D0_P = 1
        gpio_put(16, 0);  // D1_N = 0
        gpio_put(17, 1);  // D1_P = 1
        gpio_put(18, 0);  // D2_N = 0
        gpio_put(19, 1);  // D2_P = 1

        counter++;

        // Blink LED slowly
        gpio_put(LED_PIN, (counter >> 16) & 1);
    }

    return 0;
}
