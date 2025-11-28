#include <stdio.h>
#include <string.h>
#include "pico/stdlib.h"
#include "hardware/pio.h"
#include "hardware/clocks.h"
#include "hardware/dma.h"
#include "hardware/irq.h"
#include "mvs_sync.pio.h"

#define PIN_CSYNC 0
#define PIN_PCLK  1
#define PIN_R4    2
#define PIN_G4    3
#define PIN_B4    4

#define H_THRESHOLD 288

// MVS timing
#define NEO_H_TOTAL     384
#define NEO_V_TOTAL     264
#define NEO_V_ACTIVE_START 24
#define NEO_V_ACTIVE_END   248
#define NEO_H_ACTIVE_START 57

// DMA buffer for full frame capture (3 bits per pixel)
// ~102K pixels × 3 bits = ~306K bits = ~9,600 words
#define RAW_BUFFER_WORDS 10000
uint32_t raw_pixel_buffer[RAW_BUFFER_WORDS];

// Output buffer - RGB pixels (packed: 0bRRRGGGBB format, but we only use 1 bit each)
#define FRAME_WIDTH  320
#define FRAME_HEIGHT 224

// Store pixels as packed RGB in single bytes (bits: 0bxxxxRGB)
uint8_t pixel_buffer[FRAME_WIDTH * FRAME_HEIGHT];

// State
volatile bool frame_complete = false;
int dma_chan;

int main() {
    stdio_init_all();
    sleep_ms(5000);

    fprintf(stderr, "\n=== MVS RGB DMA Capture ===\n");
    fprintf(stderr, "Capturing R4, G4, B4 (3-bit color)\n\n");

    // Setup PIO
    PIO pio = pio0;

    // SM0: Sync decoder
    uint offset_sync = pio_add_program(pio, &mvs_sync_4a_program);
    uint sm_sync = pio_claim_unused_sm(pio, true);
    mvs_sync_4a_program_init(pio, sm_sync, offset_sync, PIN_CSYNC, PIN_PCLK);

    // SM1: RGB capture
    uint offset_rgb = pio_add_program(pio, &mvs_rgb_capture_program);
    uint sm_rgb = pio_claim_unused_sm(pio, true);
    mvs_rgb_capture_program_init(pio, sm_rgb, offset_rgb, PIN_R4, PIN_PCLK);

    // Setup DMA: PIO FIFO → raw_pixel_buffer
    dma_chan = dma_claim_unused_channel(true);
    dma_channel_config cfg = dma_channel_get_default_config(dma_chan);

    channel_config_set_read_increment(&cfg, false);
    channel_config_set_write_increment(&cfg, true);
    channel_config_set_dreq(&cfg, pio_get_dreq(pio, sm_rgb, false));

    dma_channel_configure(
        dma_chan,
        &cfg,
        raw_pixel_buffer,
        &pio->rxf[sm_rgb],
        RAW_BUFFER_WORDS,
        false
    );

    fprintf(stderr, "Waiting for VSYNC...\n");

    // Wait for first VSYNC
    uint32_t v_pos = 0;
    uint32_t equ_count = 0;
    bool capturing = false;

    while (!capturing) {
        if (!pio_sm_is_rx_fifo_empty(pio, sm_sync)) {
            uint32_t h_ctr = pio_sm_get(pio, sm_sync);

            if (h_ctr <= H_THRESHOLD) {
                equ_count++;
            } else {
                if (equ_count >= 8) {
                    fprintf(stderr, "VSYNC detected, starting DMA capture...\n");
                    capturing = true;

                    // Start RGB PIO and DMA simultaneously
                    pio_sm_set_enabled(pio, sm_rgb, true);
                    dma_channel_start(dma_chan);
                    break;
                }
                equ_count = 0;
            }
        }
    }

    // Monitor for frame complete
    v_pos = 0;
    equ_count = 0;

    while (!frame_complete) {
        if (!pio_sm_is_rx_fifo_empty(pio, sm_sync)) {
            uint32_t h_ctr = pio_sm_get(pio, sm_sync);

            if (h_ctr <= H_THRESHOLD) {
                equ_count++;
                if (equ_count % 2 == 0) v_pos++;
            } else {
                if (equ_count >= 8) {
                    frame_complete = true;
                    fprintf(stderr, "Frame complete at line %lu\n", v_pos);
                    break;
                }
                equ_count = 0;
                v_pos++;
            }
        }
    }

    // Stop capture
    dma_channel_abort(dma_chan);
    pio_sm_set_enabled(pio, sm_rgb, false);

    uint32_t words_captured = RAW_BUFFER_WORDS - dma_channel_hw_addr(dma_chan)->transfer_count;
    fprintf(stderr, "DMA captured %lu words\n", words_captured);

    // Post-process: Extract active RGB pixels
    fprintf(stderr, "Extracting RGB active region...\n");
    memset(pixel_buffer, 0, sizeof(pixel_buffer));

    uint32_t raw_bit_idx = 0;
    uint32_t out_pixel_idx = 0;

    // Skip to first active line
    raw_bit_idx = NEO_V_ACTIVE_START * NEO_H_TOTAL * 3;  // 3 bits per pixel

    // Extract 224 lines of 320 pixels each
    for (uint32_t line = 0; line < FRAME_HEIGHT && out_pixel_idx < (FRAME_WIDTH * FRAME_HEIGHT); line++) {
        // Skip horizontal blanking
        raw_bit_idx += NEO_H_ACTIVE_START * 3;

        // Capture 320 active pixels
        for (uint32_t x = 0; x < FRAME_WIDTH; x++) {
            // Each pixel is 3 bits: RGB
            uint32_t word_idx = raw_bit_idx / 32;
            uint32_t bit_offset = raw_bit_idx % 32;

            if (word_idx < words_captured) {
                // Extract 3 bits from the packed data
                uint32_t rgb_bits;

                if (bit_offset <= 29) {
                    // All 3 bits in same word
                    rgb_bits = (raw_pixel_buffer[word_idx] >> bit_offset) & 0x7;
                } else {
                    // Bits span across two words
                    uint32_t bits_in_first = 32 - bit_offset;
                    uint32_t bits_in_second = 3 - bits_in_first;

                    uint32_t first_part = (raw_pixel_buffer[word_idx] >> bit_offset);
                    uint32_t second_part = (word_idx + 1 < words_captured) ?
                                          (raw_pixel_buffer[word_idx + 1] & ((1 << bits_in_second) - 1)) << bits_in_first : 0;

                    rgb_bits = first_part | second_part;
                }

                // Store packed RGB: bit 0 = R, bit 1 = G, bit 2 = B
                pixel_buffer[out_pixel_idx] = rgb_bits & 0x7;
            }

            raw_bit_idx += 3;
            out_pixel_idx++;
        }

        // Skip remaining horizontal blanking
        raw_bit_idx += (NEO_H_TOTAL - NEO_H_ACTIVE_START - FRAME_WIDTH) * 3;
    }

    fprintf(stderr, "Extracted %lu pixels\n\n", out_pixel_idx);

    // Output PPM (color format)
    printf("P3\n");
    printf("%d %d\n", FRAME_WIDTH, FRAME_HEIGHT);
    printf("255\n");

    for (uint32_t i = 0; i < FRAME_WIDTH * FRAME_HEIGHT; i++) {
        uint8_t rgb = pixel_buffer[i];
        uint8_t r = (rgb & 1) ? 255 : 0;
        uint8_t g = (rgb & 2) ? 255 : 0;
        uint8_t b = (rgb & 4) ? 255 : 0;
        printf("%d %d %d ", r, g, b);
        if ((i + 1) % FRAME_WIDTH == 0) printf("\n");
    }

    fprintf(stderr, "Capture complete!\n");

    // Blink LED
    gpio_init(PICO_DEFAULT_LED_PIN);
    gpio_set_dir(PICO_DEFAULT_LED_PIN, GPIO_OUT);

    while (true) {
        gpio_put(PICO_DEFAULT_LED_PIN, 1);
        sleep_ms(500);
        gpio_put(PICO_DEFAULT_LED_PIN, 0);
        sleep_ms(500);
    }

    return 0;
}
