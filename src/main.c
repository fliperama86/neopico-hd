#include <stdio.h>
#include <string.h>
#include "pico/stdlib.h"
#include "hardware/pio.h"
#include "hardware/clocks.h"
#include "hardware/dma.h"
#include "hardware/irq.h"
#include "mvs_sync.pio.h"

#define PIN_CSYNC 0  // GP0 - Composite sync
#define PIN_PCLK  1  // GP1 - Pixel clock (C2, 6MHz)
#define PIN_R4    2  // GP2 - Red MSB (most significant bit)

#define H_THRESHOLD 288  // 3/4 of 384 pixels - distinguishes HSYNC from equalization

// MVS timing
#define NEO_H_SYNCLEN   29
#define NEO_H_BACKPORCH 28
#define NEO_H_ACTIVE    320
#define NEO_V_SYNCLEN   3
#define NEO_V_BACKPORCH 21
#define NEO_V_ACTIVE    224
#define NEO_V_TOTAL     264

// DMA capture buffer - capture FULL frame including blanking
// ~264 lines × 384 pixels = ~101,376 pixels
// Stored as 32-bit words (32 pixels per word) = 3,168 words
#define RAW_BUFFER_WORDS 3200  // Slightly larger for safety
uint32_t raw_pixel_buffer[RAW_BUFFER_WORDS];

// Final output buffer - only active pixels
#define FRAME_WIDTH  320
#define FRAME_HEIGHT 224
#define FRAME_BYTES  (FRAME_WIDTH * FRAME_HEIGHT / 8)
uint8_t pixel_buffer[FRAME_BYTES];

// Capture state
volatile bool frame_ready = false;
volatile uint32_t captured_words = 0;

int main() {
    stdio_init_all();
    sleep_ms(5000);  // Wait for USB

    fprintf(stderr, "\n=== MVS R4 DMA Capture ===\n");
    fprintf(stderr, "GP0: CSYNC | GP1: PCLK | GP2: R4\n");
    fprintf(stderr, "Using DMA for zero-CPU capture\n\n");

    // Clear buffers
    memset(raw_pixel_buffer, 0, sizeof(raw_pixel_buffer));
    memset(pixel_buffer, 0, FRAME_BYTES);

    // Setup PIO - two state machines
    PIO pio = pio0;

    // SM0: Sync decoder
    uint offset_sync = pio_add_program(pio, &mvs_sync_4a_program);
    uint sm_sync = pio_claim_unused_sm(pio, true);
    mvs_sync_4a_program_init(pio, sm_sync, offset_sync, PIN_CSYNC, PIN_PCLK);

    // SM1: Pixel capture (start disabled)
    uint offset_pixel = pio_add_program(pio, &mvs_pixel_capture_program);
    uint sm_pixel = pio_claim_unused_sm(pio, true);
    mvs_pixel_capture_program_init(pio, sm_pixel, offset_pixel, PIN_R4, PIN_PCLK);

    // Stop pixel capture until we're ready
    pio_sm_set_enabled(pio, sm_pixel, false);

    fprintf(stderr, "PIO initialized. Waiting for VSYNC to start capture...\n");

    // Frame capture state
    uint32_t v_pos = 0;
    uint32_t equ_count = 0;
    bool capturing = false;
    bool frame_complete = false;
    uint32_t pixel_count = 0;

    // Skip offset: tune this to align the image
    // Approximate calculation:
    // - 3 VSYNC lines + 21 back porch = 24 lines before active
    // - Each line ~384 pixels = ~9,216 pixels
    // - Plus horizontal offset (57 pixels per line for sync+backporch)
    // Let's try different values to find the right alignment
    uint32_t skip_pixels = 9200;  // Trying middle value
    uint32_t skipped = 0;

    while (!frame_complete) {
        // Check for sync events (line boundaries)
        if (!pio_sm_is_rx_fifo_empty(pio, sm_sync)) {
            uint32_t h_ctr = pio_sm_get(pio, sm_sync);
            bool is_equ = (h_ctr <= H_THRESHOLD);

            if (is_equ) {
                equ_count++;
                if (equ_count % 2 == 0) {
                    v_pos++;
                }
            } else {
                // Normal HSYNC
                if (equ_count > 0) {
                    if (equ_count >= 8) {
                        if (capturing) {
                            // Frame complete!
                            frame_complete = true;
                            fprintf(stderr, "Frame complete (line %lu)\n", v_pos);
                        } else {
                            // Start capturing - enable pixel PIO immediately
                            capturing = true;
                            v_pos = 0;

                            pio_sm_restart(pio, sm_pixel);
                            pio_sm_clear_fifos(pio, sm_pixel);
                            pio_sm_set_enabled(pio, sm_pixel, true);

                            fprintf(stderr, "VSYNC detected, starting pixel capture\n");
                        }
                    }
                    equ_count = 0;
                }
                v_pos++;
            }
        }

        // Continuously drain pixel FIFO while capturing
        if (capturing && !frame_complete) {
            while (!pio_sm_is_rx_fifo_empty(pio, sm_pixel)) {
                uint32_t pixels = pio_sm_get(pio, sm_pixel);

                for (int bit = 0; bit < 32; bit++) {
                    // Skip initial pixels until we reach active region
                    if (skipped < skip_pixels) {
                        skipped++;
                        continue;
                    }

                    // Capture active pixels
                    if (pixel_count < (FRAME_WIDTH * FRAME_HEIGHT)) {
                        uint8_t pixel_bit = (pixels >> bit) & 1;

                        uint32_t byte_idx = pixel_count / 8;
                        uint32_t bit_idx = pixel_count % 8;
                        if (pixel_bit) {
                            pixel_buffer[byte_idx] |= (1 << bit_idx);
                        }
                        pixel_count++;

                        // Stop if buffer full
                        if (pixel_count >= (FRAME_WIDTH * FRAME_HEIGHT)) {
                            break;
                        }
                    }
                }
            }
        }
    }

    fprintf(stderr, "Frame captured!\n");
    fprintf(stderr, "  Total lines in frame: %lu\n", v_pos);
    fprintf(stderr, "  Skipped pixels: %lu (target: %lu)\n", skipped, skip_pixels);
    fprintf(stderr, "  Captured pixels: %lu (expected: %d)\n\n",
            pixel_count, FRAME_WIDTH * FRAME_HEIGHT);

    // Output as PBM image (black & white, R4 channel)
    printf("P1\n");                           // PBM ASCII format
    printf("%d %d\n", FRAME_WIDTH, FRAME_HEIGHT);

    // Output pixels
    for (uint32_t y = 0; y < FRAME_HEIGHT; y++) {
        for (uint32_t x = 0; x < FRAME_WIDTH; x++) {
            uint32_t pixel_idx = y * FRAME_WIDTH + x;
            uint32_t byte_idx = pixel_idx / 8;
            uint32_t bit_idx = pixel_idx % 8;

            uint8_t pixel = (pixel_buffer[byte_idx] >> bit_idx) & 1;
            printf("%d ", pixel);
        }
        printf("\n");
    }

    fprintf(stderr, "=== PBM output complete ===\n");
    fprintf(stderr, "Expected: 320x224 = 71,680 pixels\n");
    fprintf(stderr, "Save output to file.pbm to view\n");

    // Done - blink LED forever
    const uint LED_PIN = PICO_DEFAULT_LED_PIN;
    gpio_init(LED_PIN);
    gpio_set_dir(LED_PIN, GPIO_OUT);

    printf("\nCapture complete. LED blinking.\n");

    while (true) {
        gpio_put(LED_PIN, 1);
        sleep_ms(500);
        gpio_put(LED_PIN, 0);
        sleep_ms(500);
    }

    return 0;
}
