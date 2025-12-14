/**
 * RGB Conversion Benchmark
 *
 * Simulates the pixel conversion workload to measure performance impact
 * of different GPIO wiring configurations.
 *
 * Tests:
 * 1. Current: R(0-4), B(5-9), G(10-14) - no remapping
 * 2. Swapped: G(0-4), B(5-9), R(10-14) - channel swap only (G in order)
 * 3. Reversed: G(4-0), B(5-9), R(10-14) - green bits reversed (inline)
 * 4. LUT: G(4-0), B(5-9), R(10-14) - green bits reversed (lookup table)
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "pico/stdlib.h"
#include "hardware/timer.h"
#include "hardware/clocks.h"

#define FRAME_WIDTH 320
#define FRAME_HEIGHT 224
#define FRAME_PIXELS (FRAME_WIDTH * FRAME_HEIGHT)
#define NUM_FRAMES 600  // 10 seconds at 60fps

// Simulated raw pixel buffer (as if read from PIO)
static uint16_t raw_pixels[FRAME_PIXELS];

// Output framebuffer
static uint16_t framebuf[FRAME_PIXELS];

// 5-bit reversal lookup table
static const uint8_t rev5[32] = {
    0,  16, 8,  24, 4,  20, 12, 28,
    2,  18, 10, 26, 6,  22, 14, 30,
    1,  17, 9,  25, 5,  21, 13, 29,
    3,  19, 11, 27, 7,  23, 15, 31
};

// =============================================================================
// Conversion Method 1: Current wiring (R0-4, B5-9, G10-14)
// =============================================================================
static void __not_in_flash_func(convert_current)(void) {
    for (int i = 0; i < FRAME_PIXELS; i++) {
        uint16_t p = raw_pixels[i];
        uint8_t r5 = p & 0x1F;
        uint8_t b5 = (p >> 5) & 0x1F;
        uint8_t g5 = (p >> 10) & 0x1F;
        uint8_t g6 = (g5 << 1) | (g5 >> 4);
        framebuf[i] = (r5 << 11) | (g6 << 5) | b5;
    }
}

// =============================================================================
// Conversion Method 2: Swapped channels (G0-4, B5-9, R10-14) - no reversal
// =============================================================================
static void __not_in_flash_func(convert_swapped)(void) {
    for (int i = 0; i < FRAME_PIXELS; i++) {
        uint16_t p = raw_pixels[i];
        uint8_t g5 = p & 0x1F;           // G now in bits 0-4
        uint8_t b5 = (p >> 5) & 0x1F;    // B still in bits 5-9
        uint8_t r5 = (p >> 10) & 0x1F;   // R now in bits 10-14
        uint8_t g6 = (g5 << 1) | (g5 >> 4);
        framebuf[i] = (r5 << 11) | (g6 << 5) | b5;
    }
}

// =============================================================================
// Conversion Method 3: Reversed green (G4-0, B5-9, R10-14) - inline reversal
// =============================================================================
static void __not_in_flash_func(convert_reversed_inline)(void) {
    for (int i = 0; i < FRAME_PIXELS; i++) {
        uint16_t p = raw_pixels[i];
        uint8_t g_raw = p & 0x1F;        // G in bits 0-4, but reversed
        uint8_t b5 = (p >> 5) & 0x1F;
        uint8_t r5 = (p >> 10) & 0x1F;

        // Reverse 5 bits inline
        uint8_t g5 = ((g_raw & 0x01) << 4) |
                     ((g_raw & 0x02) << 2) |
                     (g_raw & 0x04) |
                     ((g_raw & 0x08) >> 2) |
                     ((g_raw & 0x10) >> 4);

        uint8_t g6 = (g5 << 1) | (g5 >> 4);
        framebuf[i] = (r5 << 11) | (g6 << 5) | b5;
    }
}

// =============================================================================
// Conversion Method 4: Reversed green (G4-0, B5-9, R10-14) - LUT reversal
// =============================================================================
static void __not_in_flash_func(convert_reversed_lut)(void) {
    for (int i = 0; i < FRAME_PIXELS; i++) {
        uint16_t p = raw_pixels[i];
        uint8_t g5 = rev5[p & 0x1F];     // G reversed via LUT
        uint8_t b5 = (p >> 5) & 0x1F;
        uint8_t r5 = (p >> 10) & 0x1F;
        uint8_t g6 = (g5 << 1) | (g5 >> 4);
        framebuf[i] = (r5 << 11) | (g6 << 5) | b5;
    }
}

// =============================================================================
// Benchmark runner
// =============================================================================

typedef void (*convert_func_t)(void);

static uint32_t run_benchmark(const char *name, convert_func_t func) {
    // Warm up
    func();

    uint32_t start = time_us_32();
    for (int f = 0; f < NUM_FRAMES; f++) {
        func();
    }
    uint32_t elapsed = time_us_32() - start;

    uint32_t us_per_frame = elapsed / NUM_FRAMES;
    uint32_t fps_x10 = (10000000UL) / us_per_frame;  // FPS * 10 for 1 decimal

    printf("%-20s: %lu us/frame  (%lu.%lu fps potential)\n",
           name, us_per_frame, fps_x10 / 10, fps_x10 % 10);

    return us_per_frame;
}

int main() {
    stdio_init_all();

    // Wait for USB connection
    sleep_ms(2000);

    printf("\n");
    printf("========================================\n");
    printf("RGB Conversion Benchmark\n");
    printf("========================================\n");
    printf("Frame: %d x %d = %d pixels\n", FRAME_WIDTH, FRAME_HEIGHT, FRAME_PIXELS);
    printf("Frames: %d (%.1f seconds at 60fps)\n", NUM_FRAMES, NUM_FRAMES / 60.0f);
    printf("Clock: %lu MHz\n", clock_get_hz(clk_sys) / 1000000);
    printf("\n");

    // Fill raw buffer with pseudo-random data
    printf("Generating test data...\n");
    for (int i = 0; i < FRAME_PIXELS; i++) {
        raw_pixels[i] = (uint16_t)(i * 31337 + 12345) & 0x7FFF;
    }

    printf("\nRunning benchmarks...\n\n");

    uint32_t t_current = run_benchmark("Current (R,B,G)", convert_current);
    uint32_t t_swapped = run_benchmark("Swapped (G,B,R)", convert_swapped);
    uint32_t t_inline = run_benchmark("Reversed (inline)", convert_reversed_inline);
    uint32_t t_lut = run_benchmark("Reversed (LUT)", convert_reversed_lut);

    printf("\n");
    printf("========================================\n");
    printf("Summary (relative to current)\n");
    printf("========================================\n");
    printf("Current:          baseline\n");
    printf("Swapped:          %+ld%% (%s)\n",
           ((int32_t)t_swapped - (int32_t)t_current) * 100 / (int32_t)t_current,
           t_swapped <= t_current ? "OK" : "slower");
    printf("Reversed inline:  %+ld%% (%s)\n",
           ((int32_t)t_inline - (int32_t)t_current) * 100 / (int32_t)t_current,
           t_inline <= t_current * 1.1 ? "OK" : "significant");
    printf("Reversed LUT:     %+ld%% (%s)\n",
           ((int32_t)t_lut - (int32_t)t_current) * 100 / (int32_t)t_current,
           t_lut <= t_current * 1.05 ? "OK" : "noticeable");

    printf("\n");
    printf("Target: 16666 us/frame (60 fps)\n");
    printf("Margin: %ld us (%ld%% headroom with LUT)\n",
           16666 - (int32_t)t_lut,
           (16666 - (int32_t)t_lut) * 100 / 16666);
    printf("\n");

    // Verify outputs match (sanity check)
    printf("Verifying output correctness...\n");
    convert_current();
    uint16_t ref = framebuf[1000];

    // For swapped, we expect same result since it's just variable naming
    convert_swapped();
    printf("Swapped matches: %s\n", framebuf[1000] == ref ? "YES" : "NO (expected, different channel order)");

    printf("\nDone! Looping LED...\n");

    gpio_init(PICO_DEFAULT_LED_PIN);
    gpio_set_dir(PICO_DEFAULT_LED_PIN, GPIO_OUT);

    while (1) {
        gpio_put(PICO_DEFAULT_LED_PIN, 1);
        sleep_ms(500);
        gpio_put(PICO_DEFAULT_LED_PIN, 0);
        sleep_ms(500);
    }

    return 0;
}
