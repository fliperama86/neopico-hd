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

// DMA buffer for full frame capture + margin (4-bit RGBA, 3-bit used)
// We'll capture more than one frame, then find the correct frame boundary
// Frame size: 264 lines × 384 pixels × 4 bits = 407,552 bits = 12,736 words
// Add ~20 lines margin and buffer for safety = 14,000 words
#define RAW_BUFFER_WORDS 14000
uint32_t raw_pixel_buffer[RAW_BUFFER_WORDS];

// Output buffer (3-bit RGB: 1 byte per pixel)
#define FRAME_WIDTH 320
#define FRAME_HEIGHT 224
uint8_t pixel_buffer[FRAME_WIDTH * FRAME_HEIGHT];

// State
volatile bool frame_complete = false;
int dma_chan;

// =============================================================================
// VSYNC Detection State Machine
// =============================================================================

typedef enum
{
    SYNC_WAIT_FIRST_VSYNC,
    SYNC_WAIT_SECOND_VSYNC,
    SYNC_WAIT_FIRST_HSYNC_AFTER_VSYNC,
    SYNC_READY_TO_CAPTURE
} sync_state_t;

// Helper: drain the sync FIFO to get close to real-time
static inline void drain_sync_fifo(PIO pio, uint sm)
{
    while (!pio_sm_is_rx_fifo_empty(pio, sm))
    {
        pio_sm_get(pio, sm);
    }
}

// Helper: blocking read from sync FIFO
static inline uint32_t read_sync_fifo(PIO pio, uint sm)
{
    while (pio_sm_is_rx_fifo_empty(pio, sm))
    {
        tight_loop_contents();
    }
    return pio_sm_get(pio, sm);
}

// Wait for two VSYNCs and return at first HSYNC after second VSYNC
// This ensures we start at a known, deterministic frame boundary
void wait_for_frame_start(PIO pio, uint sm_sync)
{
    sync_state_t state = SYNC_WAIT_FIRST_VSYNC;
    uint32_t equ_count = 0;

    while (state != SYNC_READY_TO_CAPTURE)
    {
        uint32_t h_ctr = read_sync_fifo(pio, sm_sync);
        bool is_short_pulse = (h_ctr <= H_THRESHOLD);

        switch (state)
        {
        case SYNC_WAIT_FIRST_VSYNC:
            if (is_short_pulse)
            {
                equ_count++;
            }
            else if (equ_count >= 8)
            {
                // First VSYNC complete
                state = SYNC_WAIT_SECOND_VSYNC;
                equ_count = 0;
            }
            else
            {
                equ_count = 0;
            }
            break;

        case SYNC_WAIT_SECOND_VSYNC:
            if (is_short_pulse)
            {
                equ_count++;
            }
            else if (equ_count >= 8)
            {
                // Second VSYNC complete - drain FIFO and look for first HSYNC
                drain_sync_fifo(pio, sm_sync);
                state = SYNC_WAIT_FIRST_HSYNC_AFTER_VSYNC;
                equ_count = 0;
            }
            else
            {
                equ_count = 0;
            }
            break;

        case SYNC_WAIT_FIRST_HSYNC_AFTER_VSYNC:
            if (is_short_pulse)
            {
                equ_count++; // Still in equalizing pulses, keep waiting
            }
            else
            {
                // First normal HSYNC after VSYNC - we're ready!
                state = SYNC_READY_TO_CAPTURE;
            }
            break;

        case SYNC_READY_TO_CAPTURE:
            // Should never reach here in the loop
            break;
        }
    }
}

// Wait for next VSYNC (frame complete detection)
void wait_for_vsync(PIO pio, uint sm_sync)
{
    uint32_t equ_count = 0;

    while (true)
    {
        uint32_t h_ctr = read_sync_fifo(pio, sm_sync);
        bool is_short_pulse = (h_ctr <= H_THRESHOLD);

        if (is_short_pulse)
        {
            equ_count++;
        }
        else
        {
            if (equ_count >= 8)
            {
                // VSYNC detected - frame complete!
                return;
            }
            equ_count = 0;
        }
    }
}

// =============================================================================
// Main
// =============================================================================

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

    // SM1: 4-bit pixel capture (will wait for IRQ 4 trigger)
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

    // Wait for frame boundary (two VSYNCs, then first HSYNC)
    wait_for_frame_start(pio, sm_sync);

    // Clear pixel FIFO and start capture
    pio_sm_clear_fifos(pio, sm_pixel);
    dma_channel_start(dma_chan);

    // Trigger pixel PIO via hardware IRQ (deterministic, no C code latency)
    // This sets the PIO internal IRQ flag that pixel SM is waiting for
    pio_sm_exec(pio, sm_sync, pio_encode_irq_set(false, 4));

    // Wait for frame complete (next VSYNC)
    wait_for_vsync(pio, sm_sync);

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

    // Start from offset (multiply by 4 for 4-bit per pixel: R, G, B, GND)
    raw_pixel_idx = capture_offset_lines * NEO_H_TOTAL * 4;

    // Extract 224 lines of 320 pixels each (4 bits per pixel: R4, G4, B4, GND)
    for (uint32_t line = 0; line < FRAME_HEIGHT && out_pixel_idx < (FRAME_WIDTH * FRAME_HEIGHT); line++)
    {
        // Skip horizontal blanking
        raw_pixel_idx += NEO_H_ACTIVE_START * 4;

        // Capture 320 active pixels (4 bits each, but we only use 3 bits for RGB)
        for (uint32_t x = 0; x < FRAME_WIDTH; x++)
        {
            uint32_t word_idx = raw_pixel_idx / 32;
            uint32_t bit_idx = raw_pixel_idx % 32;

            if (word_idx < words_captured)
            {
                // Extract 4 bits (R, G, B, GND) and mask to 3 bits (R, G, B)
                uint8_t pixel_4bit = (raw_pixel_buffer[word_idx] >> bit_idx) & 15;
                uint8_t pixel_rgb = pixel_4bit & 7; // Mask to 3 bits (discard GND bit)
                pixel_buffer[out_pixel_idx] = pixel_rgb;
            }

            raw_pixel_idx += 4; // Skip 4 bits for next pixel
            out_pixel_idx++;
        }

        // Skip to next line (skip remaining horizontal blanking)
        raw_pixel_idx += (NEO_H_TOTAL - NEO_H_ACTIVE_START - FRAME_WIDTH) * 4;
    }

    // Output PPM (color, 3-bit RGB: 8 colors)
    printf("P3\n");
    printf("%d %d\n", FRAME_WIDTH, FRAME_HEIGHT);
    printf("7\n"); // Max value: 7 (3-bit)

    for (uint32_t i = 0; i < FRAME_WIDTH * FRAME_HEIGHT; i++)
    {
        uint8_t rgb = pixel_buffer[i];
        uint8_t r = ((rgb >> 0) & 1) * 7; // Red (bit 0)
        uint8_t g = ((rgb >> 1) & 1) * 7; // Green (bit 1)
        uint8_t b = ((rgb >> 2) & 1) * 7; // Blue (bit 2)
        printf("%d %d %d ", r, g, b);
        if ((i + 1) % FRAME_WIDTH == 0)
            printf("\n");
    }

    // Blink LED to indicate completion
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