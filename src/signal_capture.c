/**
 * Signal Capture Tool - Automated Binary Output
 *
 * Automatically captures audio samples and outputs binary data.
 * Use with viewer/analyze_capture.py for processing.
 *
 * Protocol:
 *   1. Outputs "READY\n" when ready
 *   2. Outputs frequency info as text
 *   3. Outputs "DATA:<count>\n" header
 *   4. Outputs <count> * 4 bytes (L16 + R16 per sample, little-endian)
 *   5. Outputs "DONE\n"
 */

#include <stdio.h>
#include <string.h>
#include "pico/stdlib.h"
#include "hardware/gpio.h"
#include "hardware/clocks.h"

#include "audio_config.h"

// Capture ~1 second at ~55kHz with 24-bit samples
#define CAPTURE_SAMPLES 60000

// Store 24-bit values (to capture all possible bits)
static uint32_t capture_left[CAPTURE_SAMPLES];
static uint32_t capture_right[CAPTURE_SAMPLES];

// Capture 16 bits on BCK rising edges
static inline uint16_t capture_channel_16bit(void) {
    uint16_t value = 0;
    for (int i = 0; i < 16; i++) {
        while (gpio_get(AUDIO_PIN_BCK)) tight_loop_contents();
        while (!gpio_get(AUDIO_PIN_BCK)) tight_loop_contents();
        value = (value << 1) | gpio_get(AUDIO_PIN_DAT);
    }
    return value;
}

// Capture all bits until WS changes
static inline int capture_until_ws_change(bool current_ws, uint32_t *value) {
    *value = 0;
    int bits = 0;
    while (gpio_get(AUDIO_PIN_WS) == current_ws && bits < 32) {
        while (gpio_get(AUDIO_PIN_BCK)) {
            if (gpio_get(AUDIO_PIN_WS) != current_ws) return bits;
        }
        while (!gpio_get(AUDIO_PIN_BCK)) {
            if (gpio_get(AUDIO_PIN_WS) != current_ws) return bits;
        }
        *value = (*value << 1) | gpio_get(AUDIO_PIN_DAT);
        bits++;
    }
    return bits;
}

int main() {
    stdio_init_all();

    gpio_init(PICO_DEFAULT_LED_PIN);
    gpio_set_dir(PICO_DEFAULT_LED_PIN, GPIO_OUT);
    gpio_put(PICO_DEFAULT_LED_PIN, 1);

    // Configure audio pins
    gpio_init(AUDIO_PIN_BCK);
    gpio_init(AUDIO_PIN_DAT);
    gpio_init(AUDIO_PIN_WS);
    gpio_set_dir(AUDIO_PIN_BCK, GPIO_IN);
    gpio_set_dir(AUDIO_PIN_DAT, GPIO_IN);
    gpio_set_dir(AUDIO_PIN_WS, GPIO_IN);

    // Wait for USB enumeration
    sleep_ms(2000);

    printf("READY\n");
    stdio_flush();

    // Measure frequencies (500ms)
    uint32_t bck_edges = 0, ws_edges = 0;
    bool last_bck = gpio_get(AUDIO_PIN_BCK);
    bool last_ws = gpio_get(AUDIO_PIN_WS);

    absolute_time_t start = get_absolute_time();
    while (absolute_time_diff_us(start, get_absolute_time()) < 500000) {
        bool bck = gpio_get(AUDIO_PIN_BCK);
        bool ws = gpio_get(AUDIO_PIN_WS);
        if (bck != last_bck) { bck_edges++; last_bck = bck; }
        if (ws != last_ws) { ws_edges++; last_ws = ws; }
    }

    float bck_freq = bck_edges / 1000.0;
    float ws_freq = ws_edges / 1000.0;
    float bits_per_frame = (ws_edges > 0) ? (float)bck_edges / ws_edges : 0;

    printf("FREQ:BCK=%.1f,WS=%.1f,BPF=%.1f\n", bck_freq, ws_freq, bits_per_frame);
    stdio_flush();

    // Analyze frame structure (10 frames)
    while (!gpio_get(AUDIO_PIN_WS)) tight_loop_contents();
    while (gpio_get(AUDIO_PIN_WS)) tight_loop_contents();

    int total_l_bits = 0, total_r_bits = 0;
    for (int f = 0; f < 10; f++) {
        uint32_t left_val, right_val;
        int left_bits = capture_until_ws_change(false, &left_val);
        while (!gpio_get(AUDIO_PIN_WS)) tight_loop_contents();
        int right_bits = capture_until_ws_change(true, &right_val);
        while (gpio_get(AUDIO_PIN_WS)) tight_loop_contents();
        total_l_bits += left_bits;
        total_r_bits += right_bits;
    }

    printf("BITS:L=%d,R=%d\n", total_l_bits / 10, total_r_bits / 10);
    stdio_flush();

    // Main capture
    gpio_put(PICO_DEFAULT_LED_PIN, 0);

    while (!gpio_get(AUDIO_PIN_WS)) tight_loop_contents();
    while (gpio_get(AUDIO_PIN_WS)) tight_loop_contents();

    absolute_time_t cap_start = get_absolute_time();

    for (int i = 0; i < CAPTURE_SAMPLES; i++) {
        // Capture all bits until WS goes high (left channel during WS=low)
        uint32_t left_val;
        capture_until_ws_change(false, &left_val);
        capture_left[i] = left_val;

        while (!gpio_get(AUDIO_PIN_WS)) tight_loop_contents();

        // Capture all bits until WS goes low (right channel during WS=high)
        uint32_t right_val;
        capture_until_ws_change(true, &right_val);
        capture_right[i] = right_val;

        while (gpio_get(AUDIO_PIN_WS)) tight_loop_contents();

        if ((i % 10000) == 0) {
            gpio_xor_mask(1 << PICO_DEFAULT_LED_PIN);
        }
    }

    uint32_t cap_time_us = absolute_time_diff_us(cap_start, get_absolute_time());
    float actual_rate = (float)CAPTURE_SAMPLES * 1000000.0 / cap_time_us;

    printf("RATE:%.1f\n", actual_rate);
    printf("DATA32:%d\n", CAPTURE_SAMPLES);  // DATA32 indicates 32-bit values
    stdio_flush();

    // Output binary data (little-endian: L32, R32 per sample = 8 bytes/sample)
    for (int i = 0; i < CAPTURE_SAMPLES; i++) {
        putchar(capture_left[i] & 0xFF);
        putchar((capture_left[i] >> 8) & 0xFF);
        putchar((capture_left[i] >> 16) & 0xFF);
        putchar((capture_left[i] >> 24) & 0xFF);
        putchar(capture_right[i] & 0xFF);
        putchar((capture_right[i] >> 8) & 0xFF);
        putchar((capture_right[i] >> 16) & 0xFF);
        putchar((capture_right[i] >> 24) & 0xFF);
    }
    stdio_flush();

    printf("DONE\n");
    stdio_flush();

    gpio_put(PICO_DEFAULT_LED_PIN, 1);

    while (1) {
        sleep_ms(1000);
    }

    return 0;
}
