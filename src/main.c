#include <stdio.h>
#include <string.h>
#include "pico/stdlib.h"
#include "hardware/pio.h"
#include "hardware/clocks.h"
#include "hardware/dma.h"
#include "hardware/irq.h"
#include "mvs_sync.pio.h"

#define PIN_CSYNC 0
#define PIN_PCLK 1
#define PIN_R4 2

#define H_THRESHOLD 288

// MVS timing
#define NEO_H_TOTAL 384 // Approximate pixels per line
#define NEO_V_TOTAL 264
#define NEO_V_ACTIVE_START 24 // Line where active region starts
#define NEO_V_ACTIVE_END 248  // Line where active region ends (24 + 224)
#define NEO_H_ACTIVE_START 0  // Pixel where active region starts (fine-tune for 1-bit capture)
#define H_OFFSET_PIXELS 0     // Fine-tune horizontal alignment

// DMA buffer for full frame capture + margin (2-bit RG)
// We'll capture more than one frame, then find the correct frame boundary
// Frame size: 264 lines × 384 pixels × 2 bits = 203,776 bits = 6,369 words
// Add ~20 lines margin and buffer for safety = 8,000 words
#define RAW_BUFFER_WORDS 8000
uint32_t raw_pixel_buffer[RAW_BUFFER_WORDS];

// Output buffer (2 bits per pixel = 4 grayscale levels)
#define FRAME_WIDTH 320
#define FRAME_HEIGHT 224
uint8_t pixel_buffer[FRAME_WIDTH * FRAME_HEIGHT / 4];

// State
volatile bool frame_complete = false;
int dma_chan;

int main()
{
    stdio_init_all();
    sleep_ms(5000);

    // Setup PIO
    PIO pio = pio0;

    // SM0: Sync decoder
    uint offset_sync = pio_add_program(pio, &mvs_sync_4a_program);
    uint sm_sync = pio_claim_unused_sm(pio, true);
    mvs_sync_4a_program_init(pio, sm_sync, offset_sync, PIN_CSYNC, PIN_PCLK);

    // SM1: 2-bit RG pixel capture (will wait for IRQ 4 trigger)
    uint offset_pixel = pio_add_program(pio, &mvs_pixel_capture_program);
    uint sm_pixel = pio_claim_unused_sm(pio, true);
    mvs_pixel_capture_program_init(pio, sm_pixel, offset_pixel, PIN_R4, PIN_PCLK);

    // Pixel PIO is running but blocked at "wait 1 irq 4" instruction
    // It will start capturing when we set IRQ 4

    // Setup DMA: PIO FIFO → raw_pixel_buffer
    dma_chan = dma_claim_unused_channel(true);
    dma_channel_config cfg = dma_channel_get_default_config(dma_chan);

    channel_config_set_read_increment(&cfg, false);                    // Always read from PIO FIFO
    channel_config_set_write_increment(&cfg, true);                    // Write to buffer sequentially
    channel_config_set_dreq(&cfg, pio_get_dreq(pio, sm_pixel, false)); // Paced by PIO

    dma_channel_configure(
        dma_chan,
        &cfg,
        raw_pixel_buffer,    // Destination
        &pio->rxf[sm_pixel], // Source (PIO RX FIFO)
        RAW_BUFFER_WORDS,    // Transfer count
        false                // Don't start yet
    );

    // Wait for TWO VSYNCs to ensure we start at a known frame boundary
    uint32_t v_pos = 0;
    uint32_t equ_count = 0;
    uint32_t vsync_count = 0;
    bool capturing = false;
    uint32_t capture_start_line = 0;

    while (!capturing)
    {
        if (!pio_sm_is_rx_fifo_empty(pio, sm_sync))
        {
            uint32_t h_ctr = pio_sm_get(pio, sm_sync);

            if (h_ctr <= H_THRESHOLD)
            {
                equ_count++;
            }
            else
            {
                // Normal HSYNC
                if (equ_count >= 8)
                {
                    // VSYNC detected!
                    vsync_count++;

                    if (vsync_count == 1)
                    {
                        // First VSYNC - just noted, continue waiting
                        equ_count = 0;
                    }
                    else
                    {
                        // Second VSYNC detected - now wait for first normal HSYNC after VSYNC
                        // Drain FIFO to get close to real-time
                        while (!pio_sm_is_rx_fifo_empty(pio, sm_sync))
                        {
                            pio_sm_get(pio, sm_sync);
                        }

                        // Now wait for the very next HSYNC (first line after VSYNC)
                        // This gives us a deterministic trigger point
                        equ_count = 0;
                        bool found_first_hsync = false;

                        while (!found_first_hsync)
                        {
                            if (!pio_sm_is_rx_fifo_empty(pio, sm_sync))
                            {
                                uint32_t h_ctr = pio_sm_get(pio, sm_sync);

                                if (h_ctr <= H_THRESHOLD)
                                {
                                    // Still in VSYNC region, skip
                                    equ_count++;
                                }
                                else
                                {
                                    // First normal HSYNC after VSYNC!
                                    found_first_hsync = true;
                                }
                            }
                        }

                        capturing = true;
                        capture_start_line = v_pos;

                        // Clear pixel FIFO before triggering
                        pio_sm_clear_fifos(pio, sm_pixel);

                        // Start DMA first (before triggering pixel capture)
                        dma_channel_start(dma_chan);

                        // Trigger pixel PIO via hardware IRQ (deterministic, no C code latency)
                        // Execute "irq set 4" instruction directly in the sync state machine
                        // This sets the PIO internal IRQ flag that pixel SM is waiting for
                        pio_sm_exec(pio, sm_sync, pio_encode_irq_set(false, 4));
                        break;
                    }
                }
                else
                {
                    equ_count = 0;
                }
                v_pos++;
            }
        }
    }

    // Monitor for frame complete
    v_pos = 0;
    equ_count = 0;

    while (!frame_complete)
    {
        if (!pio_sm_is_rx_fifo_empty(pio, sm_sync))
        {
            uint32_t h_ctr = pio_sm_get(pio, sm_sync);

            if (h_ctr <= H_THRESHOLD)
            {
                equ_count++;
                if (equ_count % 2 == 0)
                    v_pos++;
            }
            else
            {
                if (equ_count >= 8)
                {
                    // Frame complete!
                    frame_complete = true;
                    break;
                }
                equ_count = 0;
                v_pos++;
            }
        }
    }

    // Stop capture
    dma_channel_abort(dma_chan);
    pio_sm_set_enabled(pio, sm_pixel, false);

    uint32_t words_captured = RAW_BUFFER_WORDS - dma_channel_hw_addr(dma_chan)->transfer_count;

    // Post-process: Extract active pixels
    memset(pixel_buffer, 0, sizeof(pixel_buffer));

    // We start capturing a few lines after VSYNC (after FIFO drain + delay)
    // Active region starts at line 24, so try extracting from around line 20-30
    // Try different values: 15, 20, 25, 30 until alignment is correct
    uint32_t capture_offset_lines = 20; // TUNING PARAMETER - adjusted to capture top of frame

    uint32_t raw_pixel_idx = 0;
    uint32_t out_pixel_idx = 0;

    // Start from offset (multiply by 2 for 2-bit per pixel)
    raw_pixel_idx = capture_offset_lines * NEO_H_TOTAL * 2;

    // Extract 224 lines of 320 pixels each (2 bits per pixel)
    for (uint32_t line = 0; line < FRAME_HEIGHT && out_pixel_idx < (FRAME_WIDTH * FRAME_HEIGHT); line++)
    {
        // Skip horizontal blanking
        raw_pixel_idx += NEO_H_ACTIVE_START * 2;

        // Capture 320 active pixels (2 bits each)
        for (uint32_t x = 0; x < FRAME_WIDTH; x++)
        {
            uint32_t word_idx = raw_pixel_idx / 32;
            uint32_t bit_idx = raw_pixel_idx % 32;

            if (word_idx < words_captured)
            {
                uint8_t pixel = (raw_pixel_buffer[word_idx] >> bit_idx) & 3;  // Extract 2 bits

                uint32_t byte_idx = out_pixel_idx / 4;
                uint32_t out_bit_idx = (out_pixel_idx % 4) * 2;
                pixel_buffer[byte_idx] |= (pixel << out_bit_idx);
            }

            raw_pixel_idx += 2;  // Skip 2 bits for next pixel
            out_pixel_idx++;
        }

        // Skip to next line (skip remaining horizontal blanking)
        raw_pixel_idx += (NEO_H_TOTAL - NEO_H_ACTIVE_START - FRAME_WIDTH) * 2;
    }

    // Output PGM (grayscale, 4 levels from 2-bit RG)
    printf("P2\n");
    printf("%d %d\n", FRAME_WIDTH, FRAME_HEIGHT);
    printf("3\n");  // Max value: 3 (2-bit)

    for (uint32_t i = 0; i < FRAME_WIDTH * FRAME_HEIGHT; i++)
    {
        uint32_t byte_idx = i / 4;
        uint32_t bit_idx = (i % 4) * 2;
        uint8_t pixel = (pixel_buffer[byte_idx] >> bit_idx) & 3;
        printf("%d ", pixel);
        if ((i + 1) % FRAME_WIDTH == 0)
            printf("\n");
    }

    // Blink LED
    gpio_init(PICO_DEFAULT_LED_PIN);
    gpio_set_dir(PICO_DEFAULT_LED_PIN, GPIO_OUT);

    while (true)
    {
        gpio_put(PICO_DEFAULT_LED_PIN, 1);
        sleep_ms(500);
        gpio_put(PICO_DEFAULT_LED_PIN, 0);
        sleep_ms(500);
    }

    return 0;
}
